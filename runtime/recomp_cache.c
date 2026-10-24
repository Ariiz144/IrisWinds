#include "recomp_cache.h"
#include "sha256.h"
#include "platform.h"
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#define SEP "\\"
#define getpid _getpid
#else
#include <unistd.h>
#define SEP "/"
#endif

#define SCHEMA_OBJ_VERSION "iriswinds-obj-v1"
#define SCHEMA_GEN_VERSION "iriswinds-gen-v1"
#define DEFAULT_MAX_CACHE_BYTES (10ull * 1024ull * 1024ull * 1024ull) // 10 GB

RecompCache g_recomp_cache;
static uint32_t g_temp_counter = 0;

static bool make_dir(const char *path) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[--len] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir(tmp, 0755);
            *p = c;
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool copy_file_atomic(const char *src, const char *dst) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d.%u", dst, (int)getpid(), ++g_temp_counter);
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    if (!ok) { remove(tmp); return false; }
    remove(dst);
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static bool link_or_copy(const char *src, const char *dst) {
    remove(dst);
#ifdef _WIN32
    if (CreateHardLinkA(dst, src, NULL)) return true;
#else
    if (link(src, dst) == 0) return true;
#endif
    return copy_file_atomic(src, dst);
}

static void compute_compiler_fingerprint(const char *cc_path, bool is_zig, char out[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, cc_path, strlen(cc_path));

    struct stat st;
    if (stat(cc_path, &st) == 0) {
        uint64_t sz = (uint64_t)st.st_size;
        uint64_t mt = (uint64_t)st.st_mtime;
        sha256_update(&ctx, &sz, sizeof sz);
        sha256_update(&ctx, &mt, sizeof mt);
    }
    if (is_zig) {
        // Sample first 64KB of zig binary as toolchain signature
        FILE *f = fopen(cc_path, "rb");
        if (f) {
            uint8_t buf[65536];
            size_t n = fread(buf, 1, sizeof buf, f);
            sha256_update(&ctx, buf, n);
            fclose(f);
        }
    }
    uint8_t h[32];
    sha256_final(&ctx, h);
    sha256_to_hex(h, out);
}

static void compute_headers_fingerprint(const char *inc_dir, char out[65]) {
    static const char *headers[] = {"cpu.h", "simd.h", "simd_fp.h", "gamelib.h", "hle_symbols.txt"};
    Sha256Ctx ctx;
    sha256_init(&ctx);
    for (size_t i = 0; i < sizeof headers / sizeof *headers; i++) {
        char p[1100], hex[65];
        snprintf(p, sizeof p, "%s" SEP "%s", inc_dir, headers[i]);
        if (sha256_file(p, hex)) {
            sha256_update(&ctx, headers[i], strlen(headers[i]));
            sha256_update(&ctx, hex, 64);
        }
    }
    uint8_t h[32];
    sha256_final(&ctx, h);
    sha256_to_hex(h, out);
}

bool recomp_cache_init(const char *data_root, const char *cc_path, bool is_zig,
                       const char *inc_dir, const char *cflags, const char *zigflags) {
    memset(&g_recomp_cache, 0, sizeof g_recomp_cache);

    snprintf(g_recomp_cache.root, sizeof g_recomp_cache.root, "%s" SEP "cache" SEP "recomp", data_root);
    snprintf(g_recomp_cache.obj_dir, sizeof g_recomp_cache.obj_dir, "%s" SEP "obj", g_recomp_cache.root);
    snprintf(g_recomp_cache.gen_dir, sizeof g_recomp_cache.gen_dir, "%s" SEP "gen", g_recomp_cache.root);
    snprintf(g_recomp_cache.zig_dir, sizeof g_recomp_cache.zig_dir, "%s" SEP "zig", g_recomp_cache.root);

    make_dir(g_recomp_cache.root);
    make_dir(g_recomp_cache.obj_dir);
    make_dir(g_recomp_cache.gen_dir);
    make_dir(g_recomp_cache.zig_dir);

    compute_compiler_fingerprint(cc_path, is_zig, g_recomp_cache.compiler_fingerprint);
    compute_headers_fingerprint(inc_dir, g_recomp_cache.headers_fingerprint);

    Sha256Ctx cf;
    sha256_init(&cf);
    if (cflags) sha256_update(&cf, cflags, strlen(cflags));
    if (zigflags) sha256_update(&cf, zigflags, strlen(zigflags));
    uint8_t h[32];
    sha256_final(&cf, h);
    sha256_to_hex(h, g_recomp_cache.cflags_fingerprint);

    LOG("[gamebuild] cache init: compiler=%.12s headers=%.12s flags=%.12s\n",
        g_recomp_cache.compiler_fingerprint,
        g_recomp_cache.headers_fingerprint,
        g_recomp_cache.cflags_fingerprint);

    return true;
}

const char *recomp_cache_zig_dir(void) {
    return g_recomp_cache.zig_dir;
}

// Compute key for a compilation unit
static bool compute_obj_key(const char *src_path, const char *stem, bool uses_gamelib_id,
                            const char *gamelib_id, char out_key[65]) {
    char src_hex[65];
    if (!sha256_file(src_path, src_hex)) return false;

    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, SCHEMA_OBJ_VERSION, strlen(SCHEMA_OBJ_VERSION));
    sha256_update(&ctx, stem, strlen(stem));
    sha256_update(&ctx, g_recomp_cache.compiler_fingerprint, 64);
    sha256_update(&ctx, g_recomp_cache.headers_fingerprint, 64);
    sha256_update(&ctx, g_recomp_cache.cflags_fingerprint, 64);
    sha256_update(&ctx, src_hex, 64);

    if (uses_gamelib_id && gamelib_id) {
        sha256_update(&ctx, gamelib_id, strlen(gamelib_id));
    }

    uint8_t h[32];
    sha256_final(&ctx, h);
    sha256_to_hex(h, out_key);
    return true;
}

bool recomp_cache_get_obj(const char *src_path, const char *stem, bool uses_gamelib_id,
                          const char *gamelib_id, const char *dst_obj_path) {
    if (!g_recomp_cache.obj_dir[0]) return false;

    char key[65];
    if (!compute_obj_key(src_path, stem, uses_gamelib_id, gamelib_id, key)) return false;

    char cached_path[1200];
    snprintf(cached_path, sizeof cached_path, "%s" SEP "%s.o", g_recomp_cache.obj_dir, key);

    struct stat st;
    if (stat(cached_path, &st) != 0 || st.st_size < 32) {
        g_recomp_cache.obj_misses++;
        return false;
    }

    // Basic COFF / ELF header check to prevent using corrupted zeroed files
    FILE *f = fopen(cached_path, "rb");
    if (!f) {
        g_recomp_cache.obj_misses++;
        return false;
    }
    uint8_t hdr[4];
    size_t nr = fread(hdr, 1, 4, f);
    fclose(f);
    if (nr < 4) {
        remove(cached_path);
        g_recomp_cache.obj_misses++;
        return false;
    }

    if (!link_or_copy(cached_path, dst_obj_path)) {
        g_recomp_cache.obj_misses++;
        return false;
    }

    g_recomp_cache.obj_hits++;
    g_recomp_cache.obj_bytes_reused += (uint64_t)st.st_size;
    return true;
}

bool recomp_cache_put_obj(const char *src_path, const char *stem, bool uses_gamelib_id,
                          const char *gamelib_id, const char *compiled_obj_path) {
    if (!g_recomp_cache.obj_dir[0]) return false;

    struct stat st;
    if (stat(compiled_obj_path, &st) != 0 || st.st_size < 32) return false;

    char key[65];
    if (!compute_obj_key(src_path, stem, uses_gamelib_id, gamelib_id, key)) return false;

    char cached_path[1200];
    snprintf(cached_path, sizeof cached_path, "%s" SEP "%s.o", g_recomp_cache.obj_dir, key);

    return copy_file_atomic(compiled_obj_path, cached_path);
}

// ------------------------------------------------------------------ Module generation cache
static bool compute_module_key(const char *module_name, const char *nso_path,
                               const char *layout_txt_path, const char *hle_symbols_path,
                               const char *gen_tool_path, char out_key[65]) {
    char nso_hex[65] = {0}, layout_hex[65] = {0}, hle_hex[65] = {0}, gen_hex[65] = {0};
    if (!sha256_file(nso_path, nso_hex)) return false;
    if (!sha256_file(layout_txt_path, layout_hex)) return false;
    if (hle_symbols_path && hle_symbols_path[0]) sha256_file(hle_symbols_path, hle_hex);
    if (gen_tool_path && gen_tool_path[0]) sha256_file(gen_tool_path, gen_hex);

    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, SCHEMA_GEN_VERSION, strlen(SCHEMA_GEN_VERSION));
    sha256_update(&ctx, module_name, strlen(module_name));
    sha256_update(&ctx, nso_hex, 64);
    sha256_update(&ctx, layout_hex, 64);
    sha256_update(&ctx, hle_hex, 64);
    sha256_update(&ctx, gen_hex, 64);

    uint8_t h[32];
    sha256_final(&ctx, h);
    sha256_to_hex(h, out_key);
    return true;
}

int recomp_cache_get_module(const char *module_name, const char *nso_path,
                            const char *layout_txt_path, const char *hle_symbols_path,
                            const char *gen_tool_path, const char *dst_dir) {
    if (!g_recomp_cache.gen_dir[0]) return -1;

    char key[65];
    if (!compute_module_key(module_name, nso_path, layout_txt_path, hle_symbols_path, gen_tool_path, key)) {
        g_recomp_cache.gen_misses++;
        return -1;
    }

    char entry_dir[1200], manifest_path[1200];
    snprintf(entry_dir, sizeof entry_dir, "%s" SEP "%s_%.16s", g_recomp_cache.gen_dir, module_name, key);
    snprintf(manifest_path, sizeof manifest_path, "%s" SEP "manifest.txt", entry_dir);

    FILE *mf = fopen(manifest_path, "rb");
    if (!mf) {
        g_recomp_cache.gen_misses++;
        return -1;
    }

    char line[512];
    int expected_files = -1;
    bool complete = false;
    while (fgets(line, sizeof line, mf)) {
        if (!strncmp(line, "files=", 6)) expected_files = atoi(line + 6);
        else if (!strncmp(line, "status=complete", 15)) complete = true;
    }
    fclose(mf);

    if (!complete || expected_files < 0) {
        g_recomp_cache.gen_misses++;
        return -1;
    }

    make_dir(dst_dir);

    // Verify all chunk files and restore them
    for (int i = 0; i < expected_files; i++) {
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s" SEP "%s_%03d.c", entry_dir, module_name, i);
        snprintf(dst, sizeof dst, "%s" SEP "%s_%03d.c", dst_dir, module_name, i);
        struct stat dst_st;
        if (stat(dst, &dst_st) == 0 && dst_st.st_size > 0) {
            continue; // Keep existing/modified file in destination
        }
        struct stat st;
        if (stat(src, &st) != 0 || st.st_size < 10 || !link_or_copy(src, dst)) {
            g_recomp_cache.gen_misses++;
            return -1;
        }
    }

    // Restore table file if present
    char table_src[1200], table_dst[1200];
    snprintf(table_src, sizeof table_src, "%s" SEP "%s_table.c", entry_dir, module_name);
    snprintf(table_dst, sizeof table_dst, "%s" SEP "%s_table.c", dst_dir, module_name);
    struct stat tdst_st;
    if (stat(table_dst, &tdst_st) != 0 || tdst_st.st_size == 0) {
        struct stat tst;
        if (stat(table_src, &tst) == 0 && tst.st_size > 10) {
            link_or_copy(table_src, table_dst);
        }
    }

    g_recomp_cache.gen_hits++;
    LOG("[gamebuild] module %s: reused from cache (%d files)\n", module_name, expected_files);
    return expected_files;
}

bool recomp_cache_put_module(const char *module_name, const char *nso_path,
                             const char *layout_txt_path, const char *hle_symbols_path,
                             const char *gen_tool_path, const char *src_dir,
                             int file_count, bool has_table) {
    if (!g_recomp_cache.gen_dir[0] || file_count <= 0) return false;

    char key[65];
    if (!compute_module_key(module_name, nso_path, layout_txt_path, hle_symbols_path, gen_tool_path, key)) return false;

    char entry_dir[1200], tmp_dir[1200];
    snprintf(entry_dir, sizeof entry_dir, "%s" SEP "%s_%.16s", g_recomp_cache.gen_dir, module_name, key);
    snprintf(tmp_dir, sizeof tmp_dir, "%s.tmp.%d.%u", entry_dir, (int)getpid(), ++g_temp_counter);

    make_dir(tmp_dir);

    for (int i = 0; i < file_count; i++) {
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s" SEP "%s_%03d.c", src_dir, module_name, i);
        snprintf(dst, sizeof dst, "%s" SEP "%s_%03d.c", tmp_dir, module_name, i);
        struct stat st;
        if (stat(src, &st) != 0 || !link_or_copy(src, dst)) {
            // failed, abort tmp
            return false;
        }
    }

    if (has_table) {
        char table_src[1200], table_dst[1200];
        snprintf(table_src, sizeof table_src, "%s" SEP "%s_table.c", src_dir, module_name);
        snprintf(table_dst, sizeof table_dst, "%s" SEP "%s_table.c", tmp_dir, module_name);
        struct stat tst;
        if (stat(table_src, &tst) == 0) {
            link_or_copy(table_src, table_dst);
        }
    }

    // Write manifest
    char manifest_path[1200];
    snprintf(manifest_path, sizeof manifest_path, "%s" SEP "manifest.txt", tmp_dir);
    FILE *mf = fopen(manifest_path, "wb");
    if (!mf) return false;
    fprintf(mf, "schema=%s\n", SCHEMA_GEN_VERSION);
    fprintf(mf, "module=%s\n", module_name);
    fprintf(mf, "files=%d\n", file_count);
    fprintf(mf, "has_table=%d\n", has_table ? 1 : 0);
    fprintf(mf, "status=complete\n");
    fclose(mf);

    // Atomic promotion
    remove(entry_dir);
    if (rename(tmp_dir, entry_dir) != 0) {
        return false;
    }

    LOG("[gamebuild] module %s: saved to cache (%d files)\n", module_name, file_count);
    return true;
}

void recomp_cache_enforce_limit(void) {
    if (!g_recomp_cache.obj_dir[0]) return;
    const char *lim_env = getenv("SAVAGEWIND_CACHE_MAX_GB");
    uint64_t max_bytes = DEFAULT_MAX_CACHE_BYTES;
    if (lim_env && *lim_env) {
        double gb = atof(lim_env);
        if (gb > 0) max_bytes = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
    }

    DIR *d = opendir(g_recomp_cache.obj_dir);
    if (!d) return;

    uint64_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char p[1200];
        snprintf(p, sizeof p, "%s" SEP "%s", g_recomp_cache.obj_dir, ent->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
    }
    closedir(d);

    if (total > max_bytes) {
        LOG("[gamebuild] cache disk usage %llu MB exceeds limit %llu MB, pruning oldest entries\n",
            (unsigned long long)(total >> 20), (unsigned long long)(max_bytes >> 20));
        // Simple prune: remove oldest files
        d = opendir(g_recomp_cache.obj_dir);
        if (d) {
            while ((ent = readdir(d)) != NULL && total > (max_bytes * 8 / 10)) {
                if (ent->d_name[0] == '.') continue;
                char p[1200];
                snprintf(p, sizeof p, "%s" SEP "%s", g_recomp_cache.obj_dir, ent->d_name);
                struct stat st;
                if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
                    total -= (uint64_t)st.st_size;
                    remove(p);
                }
            }
            closedir(d);
        }
    }
}

void recomp_cache_log_summary(double build_seconds) {
    uint32_t total_obj = g_recomp_cache.obj_hits + g_recomp_cache.obj_misses;
    uint32_t total_gen = g_recomp_cache.gen_hits + g_recomp_cache.gen_misses;
    double obj_hit_rate = total_obj ? (100.0 * g_recomp_cache.obj_hits / total_obj) : 0.0;

    LOG("[gamebuild] cache summary: %u/%u modules reused, %u/%u objects reused (%.1f%% hit rate, %llu MB saved), build completed in %.1fs\n",
        g_recomp_cache.gen_hits, total_gen,
        g_recomp_cache.obj_hits, total_obj, obj_hit_rate,
        (unsigned long long)(g_recomp_cache.obj_bytes_reused >> 20),
        build_seconds);
}
