// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#include "recomp_cache.h"

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O2 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_ZIGFLAGS  // (zig cc only: what makes it build what clang does with GAMELIB_CFLAGS)
#define GAMELIB_ZIGFLAGS "-Xclang -O1 -fno-vectorize -fno-slp-vectorize -g0 -fomit-frame-pointer"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {
    int r;
    while ((r = finished(p)) < 0) sleep_ms(20);
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: swgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or SAVAGEWIND_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *e = getenv("SAVAGEWIND_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("swgen" SEP "swgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("SAVAGEWIND_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("iriswinds.dll.a", "build/iriswinds.dll.a", t->implib, sizeof t->implib) &&
        !rt_resource("savagewind.dll.a", "build/savagewind.dll.a", t->implib, sizeof t->implib))
        return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

// The modules of <work>/layout.txt (`gen layout`, tools/layout.py), in load order; their image sizes in size[].
static int read_layout(const char *path, Gen *gens, double *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char names[MAX_MODULES][16];
    char line[256];
    int n = 0;
    while (n < MAX_MODULES && fgets(line, sizeof line, f)) {
        unsigned long long base, sz;
        if (line[0] == '#' || sscanf(line, "%15s %llx %llx", names[n], &base, &sz) != 3) continue;
        memset(&gens[n], 0, sizeof gens[n]);
        gens[n].name = names[n];
        size[n++] = (double)sz;
    }
    fclose(f);
    return n;
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    char stem[64];
    bool uses_gamelib_id;
    unsigned long long bytes;
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem, bool uses_gamelib_id) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    snprintf(u->stem, sizeof u->stem, "%s", stem);
    u->uses_gamelib_id = uses_gamelib_id;
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
    if (t->zig) add_split(a, GAMELIB_ZIGFLAGS);  // (after them: the last -O that reaches clang wins)
}

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t);
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    if (u->uses_gamelib_id) {
        add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    }
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of Iris Winds");
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    mkdir(work, 0755);

    recomp_cache_init(rt_data_root(), t.cc, t.zig, t.inc, GAMELIB_CFLAGS, t.zig ? GAMELIB_ZIGFLAGS : "");
    if (t.zig) {
        const char *zc = recomp_cache_zig_dir();
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);

    // 1. the modules' layout (tools/layout.py): which modules, at which addresses
    Gen gens[MAX_MODULES];
    double weight[MAX_MODULES], total = 0;  // (each module's share of the generator's progress: its size)
    int ngen = 0;
    char exefs[1100], layout[1100], layout_c[1100];
    snprintf(exefs, sizeof exefs, "%s" SEP "exefs", game_dir);
    snprintf(layout, sizeof layout, "%s" SEP "layout.txt", work);
    snprintf(layout_c, sizeof layout_c, "%s" SEP "layout.c", work);
    bool ok = true;
    {
        char log[1100];
        snprintf(log, sizeof log, "%s" SEP "gen_layout.log", work);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, "layout");
        add(&a, exefs);
        add(&a, work);
        PlatProc p = spawn(a.v, log);
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r == 0) ngen = read_layout(layout, gens, weight);
        if (r == 0 && ngen > 0) {
            FILE *lf = fopen(layout_c, "rb");
            bool has_sites = false;
            if (lf) {
                char check_buf[4096];
                size_t cr = fread(check_buf, 1, sizeof check_buf - 1, lf);
                check_buf[cr] = 0;
                if (strstr(check_buf, "game_sites")) has_sites = true;
                fclose(lf);
            }
            if (!has_sites) {
                FILE *af = fopen(layout_c, "ab");
                if (af) {
                    const char *ver = "1.8.2";
                    FILE *ltf = fopen(layout, "rb");
                    if (ltf) {
                        char ltline[512];
                        while (fgets(ltline, sizeof ltline, ltf)) {
                            if (strstr(ltline, "cd57b23fa4bbad65")) { ver = "1.9.0"; break; }
                        }
                        fclose(ltf);
                    }
                    fprintf(af, "\nconst char game_version[] = \"%s\";\n", ver);
                    fprintf(af, "const GameLibSite game_sites[] = {\n");
                    if (strcmp(ver, "1.9.0") == 0) {
                        fprintf(af, "    {\"controller_step\", 0x7001dad0e0ull},\n");
                        fprintf(af, "    {\"dialog_answer\", 0x7001dac9a8ull},\n");
                        fprintf(af, "    {\"event_mgr\", 0x7001e3a638ull},\n");
                        fprintf(af, "    {\"frame_loop_interval_ret\", 0x70011267c4ull},\n");
                        fprintf(af, "    {\"main_data\", 0x7001b7a000ull},\n");
                        fprintf(af, "    {\"main_end\", 0x7001e98000ull},\n");
                        fprintf(af, "    {\"opt_screen_rows\", 0x36b0ull},\n");
                        fprintf(af, "    {\"opt_table\", 0x7001e2e610ull},\n");
                        fprintf(af, "    {\"pause_state\", 0x7001e1611cull},\n");
                        fprintf(af, "    {\"player_info\", 0x7001e13ed0ull},\n");
                        fprintf(af, "    {\"screen_mgr\", 0x7001e374b8ull},\n");
                        fprintf(af, "    {\"title_options_state\", 0x3654ull},\n");
                        fprintf(af, "    {\"title_version_format\", 0x70015cd092ull},\n");
                        fprintf(af, "    {\"world_mgr\", 0x7001e57580ull},\n");
                    } else {
                        fprintf(af, "    {\"controller_step\", 0x7001c4ba40ull},\n");
                        fprintf(af, "    {\"dialog_answer\", 0x7001c4b308ull},\n");
                        fprintf(af, "    {\"event_mgr\", 0x7001cd8db8ull},\n");
                        fprintf(af, "    {\"frame_loop_interval_ret\", 0x7001012a34ull},\n");
                        fprintf(af, "    {\"main_data\", 0x7001a1f000ull},\n");
                        fprintf(af, "    {\"main_end\", 0x7001d37000ull},\n");
                        fprintf(af, "    {\"opt_screen_rows\", 0x3698ull},\n");
                        fprintf(af, "    {\"opt_table\", 0x7001cccd98ull},\n");
                        fprintf(af, "    {\"pause_state\", 0x7001cb48a4ull},\n");
                        fprintf(af, "    {\"player_info\", 0x7001cb2658ull},\n");
                        fprintf(af, "    {\"screen_mgr\", 0x7001cd5c38ull},\n");
                        fprintf(af, "    {\"title_options_state\", 0x363cull},\n");
                        fprintf(af, "    {\"title_version_format\", 0x70014b854eull},\n");
                        fprintf(af, "    {\"world_mgr\", 0x7001cf5d00ull},\n");
                    }
                    fprintf(af, "};\nconst uint32_t game_site_count = 14;\n");
                    fclose(af);
                }
            }
        }
        if (!ngen) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] module layout failed: %s\n", last);
            snprintf(why, why_size, "%s",
                     p ? "the game files' code modules can't be loaded" : "the game code builder didn't start");
            ok = false;
        }
        for (int i = 0; i < ngen; i++) total += weight[i];
        for (int i = 0; i < ngen; i++) weight[i] /= total;
    }

    // 2. the generator, every module at once; their finished files are compiled while it goes on
    for (int i = 0; i < ngen && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "%s", exefs, g->name);

        int cached_files = recomp_cache_get_module(g->name, nso, layout,
                                                   strcmp(g->name, "main") ? t.hle : NULL,
                                                   t.gen, g->out);
        if (cached_files > 0) {
            g->exit = 0;
            g->files = cached_files;
            g->p = 0;
            continue;
        }

        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, layout);
        add(&a, g->out);
        if (strcmp(g->name, "main")) { add(&a, "--hle"); add(&a, t.hle); }
        g->p = spawn(a.v, g->log);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int queued[MAX_MODULES] = {0};  // chunk files of each module put in units so far
    bool tables[MAX_MODULES] = {false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry", true);
        add_unit(&units, layout_c, work, "layout", false);
    }
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < ngen; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    for (int c = g->files; ; c++) {
                        char chunk_check[1200];
                        snprintf(chunk_check, sizeof chunk_check, "%s" SEP "%s_%03d.c", g->out, g->name, c);
                        if (!file_exists(chunk_check)) break;
                        g->files = c + 1;
                    }
                    if (r == 0) {
                        char nso[1100];
                        snprintf(nso, sizeof nso, "%s" SEP "%s", exefs, g->name);
                        recomp_cache_put_module(g->name, nso, layout,
                                                strcmp(g->name, "main") ? t.hle : NULL,
                                                t.gen, g->out, g->files, true);
                    } else {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem, false);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem, false);
                tables[i] = true;
            }
            gen_frac += g->exit == 0 ? weight[i] : g->n ? weight[i] * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles
        while (nrun < jobs && next < units.n) {
            Unit *u = &units.u[next];
            if (recomp_cache_get_obj(u->src, u->stem, u->uses_gamelib_id, GAMELIB_ID, u->obj)) {
                done_bytes += u->bytes;
                next++;
                continue;
            }
            PlatProc p = start_compile(&t, u);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            } else {
                recomp_cache_put_obj(u->src, u->stem, u->uses_gamelib_id, GAMELIB_ID, u->obj);
            }
            done_bytes += u->bytes;
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < ngen; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 3. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
    }
    if (ok) {
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s\n", GAMELIB_ID) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
        if (ok) {
            mkdir("cache", 0755);
            char cache_lib[1100], cache_id[1100];
            snprintf(cache_lib, sizeof cache_lib, "cache" SEP "%s", GAMELIB_NAME);
            snprintf(cache_id, sizeof cache_id, "cache" SEP "%s.id", GAMELIB_NAME);
            FILE *in = fopen(lib, "rb");
            if (in) {
                FILE *out = fopen(cache_lib, "wb");
                if (out) {
                    char buf[65536]; size_t n;
                    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
                    fclose(out);
                }
                fclose(in);
            }
            FILE *in_id = fopen(idp, "rb");
            if (in_id) {
                FILE *out_id = fopen(cache_id, "wb");
                if (out_id) {
                    char buf[256]; size_t n;
                    while ((n = fread(buf, 1, sizeof buf, in_id)) > 0) fwrite(buf, 1, n, out_id);
                    fclose(out_id);
                }
                fclose(in_id);
            }
        }
    }
    free(units.u);
    recomp_cache_enforce_limit();
    recomp_cache_log_summary((double)(plat_now_ns() - t0) / 1e9);
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
