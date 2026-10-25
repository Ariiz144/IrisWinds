// Persistent content-addressed build cache for IrisWinds game code recompilation.
#include "recomp_cache.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <dirent.h>
#define mkdir(p, m) _mkdir(p)
#define getpid _getpid
#define SEP "\\"
#define DIR_SEP_CHAR '\\'
#else
#include <dirent.h>
#include <unistd.h>
#define SEP "/"
#define DIR_SEP_CHAR '/'
#endif

#ifndef LOG
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define SCHEMA_OBJ_VERSION "obj_v2.1"
#define SCHEMA_GEN_VERSION "gen_v2.1"

// Default cache quota: 10 GB
#define DEFAULT_MAX_CACHE_BYTES (10ull * 1024ull * 1024ull * 1024ull)

RecompCache g_recomp_cache;
static uint32_t g_temp_counter = 0;

// ------------------------------------------------------------------ Filesystem helpers
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

static bool remove_dir_recursive(const char *path) {
    if (!path || !*path) return false;
#ifdef _WIN32
    char search[1200];
    snprintf(search, sizeof search, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[1200];
            snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_dir_recursive(child);
            } else {
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryA(path) != 0 || errno == ENOENT;
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char child[1200];
            snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
            struct stat st;
            if (stat(child, &st) == 0) {
                if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
                else unlink(child);
            }
        }
        closedir(d);
    }
    return rmdir(path) == 0 || errno == ENOENT;
#endif
}

static bool copy_file_direct(const char *src, const char *dst) {
#ifdef _WIN32
    return CopyFileA(src, dst, FALSE) != 0;
#else
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            unlink(dst);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
#endif
}

// Atomically copy file: writes to unique temp file first, then renames to dst
static bool copy_file_atomic(const char *src, const char *dst) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d.%u", dst, (int)getpid(), ++g_temp_counter);
    if (!copy_file_direct(src, tmp)) {
        remove(tmp);
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        return false;
    }
    return true;
#else
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        return false;
    }
    return true;
#endif
}

// Hardlinks or copies compiled object files (.o) where modification will not occur
static bool link_or_copy_obj(const char *src, const char *dst) {
#ifdef _WIN32
    remove(dst);
    if (CreateHardLinkA(dst, src, NULL)) return true;
    return copy_file_atomic(src, dst);
#else
    remove(dst);
    if (link(src, dst) == 0) return true;
    return copy_file_atomic(src, dst);
#endif
}

// ------------------------------------------------------------------ Hashing utilities

static void hash_file_into(Sha256Ctx *ctx, const char *path) {
    char h[65];
    if (sha256_file(path, h)) {
        sha256_update(ctx, h, 64);
    } else {
        sha256_update(ctx, "missing", 7);
    }
}

// ------------------------------------------------------------------ Baseline Fingerprints
static bool compute_compiler_key(const char *cc_path, bool is_zig, char out_hex[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, is_zig ? "zig:" : "cc:", 4);
    hash_file_into(&ctx, cc_path);
    uint8_t d[32];
    sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[64] = 0;
    return true;
}

static bool compute_headers_key(const char *inc_dir, char out_hex[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    static const char *headers[] = {
        "cpu.h", "simd.h", "simd_fp.h", "gamelib.h"
    };
    for (size_t i = 0; i < sizeof headers / sizeof *headers; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s" SEP "%s", inc_dir, headers[i]);
        sha256_update(&ctx, headers[i], strlen(headers[i]));
        hash_file_into(&ctx, p);
    }
    uint8_t d[32];
    sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[64] = 0;
    return true;
}

static bool compute_cflags_key(const char *cflags, const char *zigflags, char out_hex[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    if (cflags) sha256_update(&ctx, cflags, strlen(cflags));
    if (zigflags) sha256_update(&ctx, zigflags, strlen(zigflags));
    uint8_t d[32];
    sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[64] = 0;
    return true;
}

bool recomp_cache_init(const char *data_root, const char *cc_path, bool is_zig,
                       const char *inc_dir, const char *cflags, const char *zigflags) {
    if (!data_root || !*data_root) return false;

    snprintf(g_recomp_cache.root, sizeof g_recomp_cache.root, "%s" SEP "cache" SEP "recomp", data_root);
    snprintf(g_recomp_cache.obj_dir, sizeof g_recomp_cache.obj_dir, "%s" SEP "obj", g_recomp_cache.root);
    snprintf(g_recomp_cache.gen_dir, sizeof g_recomp_cache.gen_dir, "%s" SEP "gen", g_recomp_cache.root);
    snprintf(g_recomp_cache.zig_dir, sizeof g_recomp_cache.zig_dir, "%s" SEP "zig", g_recomp_cache.root);

    make_dir(g_recomp_cache.root);
    make_dir(g_recomp_cache.obj_dir);
    make_dir(g_recomp_cache.gen_dir);
    make_dir(g_recomp_cache.zig_dir);

    compute_compiler_key(cc_path, is_zig, g_recomp_cache.compiler_fingerprint);
    compute_headers_key(inc_dir, g_recomp_cache.headers_fingerprint);
    compute_cflags_key(cflags, zigflags, g_recomp_cache.cflags_fingerprint);

    LOG("[gamebuild] cache init: compiler=%.12s headers=%.12s flags=%.12s\n",
        g_recomp_cache.compiler_fingerprint,
        g_recomp_cache.headers_fingerprint,
        g_recomp_cache.cflags_fingerprint);

    return true;
}

const char *recomp_cache_zig_dir(void) {
    return g_recomp_cache.zig_dir;
}

// ------------------------------------------------------------------ Object Caching
static bool compute_obj_key(const char *src_path, const char *stem, bool uses_gamelib_id,
                            const char *gamelib_id, char out_hex[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, SCHEMA_OBJ_VERSION, strlen(SCHEMA_OBJ_VERSION));
    sha256_update(&ctx, stem, strlen(stem));
    hash_file_into(&ctx, src_path);
    sha256_update(&ctx, g_recomp_cache.compiler_fingerprint, 64);
    sha256_update(&ctx, g_recomp_cache.headers_fingerprint, 64);
    sha256_update(&ctx, g_recomp_cache.cflags_fingerprint, 64);

    if (uses_gamelib_id && gamelib_id) {
        sha256_update(&ctx, "id:", 3);
        sha256_update(&ctx, gamelib_id, strlen(gamelib_id));
    }

    uint8_t d[32];
    sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[64] = 0;
    return true;
}

static bool validate_obj_integrity(const char *cached_path, const char *meta_path) {
    struct stat st;
    if (stat(cached_path, &st) != 0 || st.st_size < 32) return false;

    // Check .meta sidecar with payload SHA-256
    FILE *mf = fopen(meta_path, "rb");
    if (mf) {
        char line[256];
        char expected_hash[65] = {0};
        while (fgets(line, sizeof line, mf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "sha256=", 7)) snprintf(expected_hash, sizeof expected_hash, "%s", line + 7);
        }
        fclose(mf);
        if (expected_hash[0]) {
            char actual_hash[65];
            if (!sha256_file(cached_path, actual_hash) || strcmp(actual_hash, expected_hash) != 0) {
                return false;
            }
        }
        return true;
    }

    // Fallback: Validate binary object headers (COFF / ELF)
    FILE *f = fopen(cached_path, "rb");
    if (!f) return false;
    uint8_t hdr[20];
    size_t nr = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (nr < 20) return false;

    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') return true;
    uint16_t machine = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint16_t num_sections = (uint16_t)(hdr[2] | (hdr[3] << 8));
    if ((machine == 0x8664 || machine == 0xAA64 || machine == 0x014C) && num_sections > 0) return true;

    return false;
}

bool recomp_cache_get_obj(const char *src_path, const char *stem, bool uses_gamelib_id,
                          const char *gamelib_id, const char *dst_obj_path) {
    if (!g_recomp_cache.obj_dir[0]) return false;

    char key[65];
    if (!compute_obj_key(src_path, stem, uses_gamelib_id, gamelib_id, key)) return false;

    char cached_path[1200], meta_path[1200];
    snprintf(cached_path, sizeof cached_path, "%s" SEP "%s.o", g_recomp_cache.obj_dir, key);
    snprintf(meta_path, sizeof meta_path, "%s" SEP "%s.meta", g_recomp_cache.obj_dir, key);

    if (!validate_obj_integrity(cached_path, meta_path)) {
        remove(cached_path);
        remove(meta_path);
        g_recomp_cache.obj_misses++;
        return false;
    }

    struct stat st;
    stat(cached_path, &st);

    if (!link_or_copy_obj(cached_path, dst_obj_path)) {
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

    char cached_path[1200], meta_path[1200];
    snprintf(cached_path, sizeof cached_path, "%s" SEP "%s.o", g_recomp_cache.obj_dir, key);
    snprintf(meta_path, sizeof meta_path, "%s" SEP "%s.meta", g_recomp_cache.obj_dir, key);

    char obj_hex[65];
    if (!sha256_file(compiled_obj_path, obj_hex)) return false;

    if (!copy_file_atomic(compiled_obj_path, cached_path)) return false;

    char meta_tmp[1200];
    snprintf(meta_tmp, sizeof meta_tmp, "%s.tmp.%d.%u", meta_path, (int)getpid(), ++g_temp_counter);
    FILE *mf = fopen(meta_tmp, "wb");
    if (mf) {
        fprintf(mf, "size=%llu\nsha256=%s\n", (unsigned long long)st.st_size, obj_hex);
        fclose(mf);
        remove(meta_path);
        rename(meta_tmp, meta_path);
    }

    return true;
}

// ------------------------------------------------------------------ Complete Module Generator Fingerprinting
static bool compute_generator_fingerprint(const char *gen_tool_path, const char *gen_script_path,
                                         const char *gen_options, char out_hex[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);

    if (gen_script_path && gen_script_path[0]) {
        sha256_update(&ctx, "source_mode", 11);

        char py_hex[65] = {0};
        if (sha256_file(gen_tool_path, py_hex)) {
            sha256_update(&ctx, "python:", 7);
            sha256_update(&ctx, py_hex, 64);
        }

        char script_hex[65] = {0};
        if (sha256_file(gen_script_path, script_hex)) {
            sha256_update(&ctx, "gen_script:", 11);
            sha256_update(&ctx, script_hex, 64);
        }

        char script_dir[1100];
        snprintf(script_dir, sizeof script_dir, "%s", gen_script_path);
        char *last_sep = strrchr(script_dir, '/');
        if (!last_sep) last_sep = strrchr(script_dir, '\\');
        if (last_sep) *last_sep = 0;
        else snprintf(script_dir, sizeof script_dir, ".");

        static const char *deps[] = {
            "lifter.py", "analyze.py", "nso.py", "passes.py", "patches.py",
            "layout.py", "game_sites.py", "simd_c.py"
        };
        for (size_t i = 0; i < sizeof deps / sizeof *deps; i++) {
            char p[1200], h[65];
            snprintf(p, sizeof p, "%s" SEP "%s", script_dir, deps[i]);
            if (sha256_file(p, h)) {
                sha256_update(&ctx, deps[i], strlen(deps[i]));
                sha256_update(&ctx, h, 64);
            }
        }
    } else if (gen_tool_path && gen_tool_path[0]) {
        sha256_update(&ctx, "packaged_mode", 13);
        char exe_hex[65] = {0};
        if (sha256_file(gen_tool_path, exe_hex)) {
            sha256_update(&ctx, "swgen:", 6);
            sha256_update(&ctx, exe_hex, 64);
        }
    }

    if (gen_options && gen_options[0]) {
        sha256_update(&ctx, "opts:", 5);
        sha256_update(&ctx, gen_options, strlen(gen_options));
    }

    uint8_t d[32];
    sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[64] = 0;
    return true;
}

static bool compute_module_key(const char *module_name, const char *nso_path,
                               const char *layout_txt_path, const char *hle_symbols_path,
                               const char *gen_tool_path, const char *gen_script_path,
                               const char *gen_options, char out_hex[65]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, SCHEMA_GEN_VERSION, strlen(SCHEMA_GEN_VERSION));
    sha256_update(&ctx, module_name, strlen(module_name));

    hash_file_into(&ctx, nso_path);
    if (layout_txt_path) hash_file_into(&ctx, layout_txt_path);
    if (hle_symbols_path) hash_file_into(&ctx, hle_symbols_path);

    char gen_fp[65];
    if (compute_generator_fingerprint(gen_tool_path, gen_script_path, gen_options, gen_fp)) {
        sha256_update(&ctx, gen_fp, 64);
    }

    uint8_t d[32];
    sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", d[i]);
    out_hex[64] = 0;
    return true;
}

// ------------------------------------------------------------------ FIX 1: Verified Module Cache
int recomp_cache_get_module(const char *module_name, const char *nso_path,
                            const char *layout_txt_path, const char *hle_symbols_path,
                            const char *gen_tool_path, const char *gen_script_path,
                            const char *gen_options, const char *dst_dir) {
    if (!g_recomp_cache.gen_dir[0]) return -1;

    char key[65];
    if (!compute_module_key(module_name, nso_path, layout_txt_path, hle_symbols_path,
                            gen_tool_path, gen_script_path, gen_options, key)) {
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
    char manifest_schema[64] = {0};
    char manifest_module[64] = {0};
    char manifest_key[65] = {0};
    int has_table = -1;
    char manifest_table_hash[65] = {0};

    char chunk_hashes[512][65];
    bool hash_seen[512];
    memset(chunk_hashes, 0, sizeof chunk_hashes);
    memset(hash_seen, 0, sizeof hash_seen);
    bool manifest_corrupt = false;

    while (fgets(line, sizeof line, mf)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "schema=", 7)) snprintf(manifest_schema, sizeof manifest_schema, "%s", line + 7);
        else if (!strncmp(line, "module=", 7)) snprintf(manifest_module, sizeof manifest_module, "%s", line + 7);
        else if (!strncmp(line, "key=", 4)) snprintf(manifest_key, sizeof manifest_key, "%s", line + 4);
        else if (!strncmp(line, "files=", 6)) expected_files = atoi(line + 6);
        else if (!strncmp(line, "has_table=", 10)) has_table = atoi(line + 10);
        else if (!strncmp(line, "hash_table=", 11)) snprintf(manifest_table_hash, sizeof manifest_table_hash, "%s", line + 11);
        else if (!strncmp(line, "hash_", 5)) {
            int idx = atoi(line + 5);
            char *eq = strchr(line, '=');
            if (eq && idx >= 0 && idx < 512) {
                if (hash_seen[idx]) { manifest_corrupt = true; break; }
                char *val = eq + 1;
                if (strlen(val) != 64) { manifest_corrupt = true; break; }
                snprintf(chunk_hashes[idx], 65, "%s", val);
                hash_seen[idx] = true;
            } else {
                manifest_corrupt = true; break;
            }
        }
        else if (!strncmp(line, "status=complete", 15)) complete = true;
    }
    fclose(mf);

    // Validate manifest metadata integrity
    if (manifest_corrupt || !complete || expected_files <= 0 || expected_files > 512 ||
        strcmp(manifest_schema, SCHEMA_GEN_VERSION) != 0 ||
        strcmp(manifest_module, module_name) != 0 ||
        strcmp(manifest_key, key) != 0 ||
        has_table < 0 || (has_table && strlen(manifest_table_hash) != 64)) {
        LOG("[gamebuild] cache entry %s: invalid or corrupted manifest; purging\n", entry_dir);
        remove_dir_recursive(entry_dir);
        g_recomp_cache.gen_misses++;
        return -1;
    }

    // Verify every expected chunk hash exists
    for (int i = 0; i < expected_files; i++) {
        if (!hash_seen[i] || strlen(chunk_hashes[i]) != 64) {
            LOG("[gamebuild] cache entry %s: missing hash for chunk %d; purging\n", entry_dir, i);
            remove_dir_recursive(entry_dir);
            g_recomp_cache.gen_misses++;
            return -1;
        }
    }

    // Genuine SHA-256 payload integrity check of all cached chunks BEFORE touching workspace
    for (int i = 0; i < expected_files; i++) {
        char src[1200];
        snprintf(src, sizeof src, "%s" SEP "%s_%03d.c", entry_dir, module_name, i);
        struct stat st;
        if (stat(src, &st) != 0 || st.st_size <= 0) {
            LOG("[gamebuild] cache entry %s: chunk %d missing or empty; purging\n", entry_dir, i);
            remove_dir_recursive(entry_dir);
            g_recomp_cache.gen_misses++;
            return -1;
        }
        char actual_hash[65];
        if (!sha256_file(src, actual_hash) || strcmp(actual_hash, chunk_hashes[i]) != 0) {
            LOG("[gamebuild] cache corruption: chunk %s_%03d.c payload hash mismatch; purging\n", module_name, i);
            remove_dir_recursive(entry_dir);
            g_recomp_cache.gen_misses++;
            return -1;
        }
    }

    if (has_table) {
        char table_src[1200];
        snprintf(table_src, sizeof table_src, "%s" SEP "%s_table.c", entry_dir, module_name);
        struct stat tst;
        if (stat(table_src, &tst) != 0 || tst.st_size <= 0) {
            LOG("[gamebuild] cache entry %s: table file missing or empty; purging\n", entry_dir);
            remove_dir_recursive(entry_dir);
            g_recomp_cache.gen_misses++;
            return -1;
        }
        char actual_table_hash[65];
        if (!sha256_file(table_src, actual_table_hash) || strcmp(actual_table_hash, manifest_table_hash) != 0) {
            LOG("[gamebuild] cache corruption: module %s table file payload hash mismatch; purging\n", module_name);
            remove_dir_recursive(entry_dir);
            g_recomp_cache.gen_misses++;
            return -1;
        }
    }

    // Cache entry is 100% verified. Proceed to workspace restoration.
    make_dir(dst_dir);

    char key_file[1200];
    snprintf(key_file, sizeof key_file, "%s" SEP ".module_key", dst_dir);

    bool workspace_matches = false;
    FILE *kf = fopen(key_file, "rb");
    if (kf) {
        char kline[256];
        while (fgets(kline, sizeof kline, kf)) {
            kline[strcspn(kline, "\r\n")] = 0;
            if (!strncmp(kline, "key=", 4)) {
                if (!strcmp(kline + 4, key)) workspace_matches = true;
                break;
            }
        }
        fclose(kf);
    }

    if (!workspace_matches) {
        bool has_existing = false;
        char test_chunk[1200];
        snprintf(test_chunk, sizeof test_chunk, "%s" SEP "%s_000.c", dst_dir, module_name);
        struct stat tst;
        if (stat(test_chunk, &tst) == 0 && tst.st_size > 0) has_existing = true;

        if (has_existing) {
            char backup_dir[1200];
            snprintf(backup_dir, sizeof backup_dir, "%s.backup.%llu", dst_dir, (unsigned long long)time(NULL));
            LOG("[gamebuild] module %s: generation key changed; backing up old workspace to %s\n", module_name, backup_dir);
            remove_dir_recursive(backup_dir);
            rename(dst_dir, backup_dir);
            make_dir(dst_dir);
        }
    }

    // Restore files using COPIES (never hardlinks) to ensure cache remains immutable
    for (int i = 0; i < expected_files; i++) {
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s" SEP "%s_%03d.c", entry_dir, module_name, i);
        snprintf(dst, sizeof dst, "%s" SEP "%s_%03d.c", dst_dir, module_name, i);

        struct stat dst_st;
        if (workspace_matches && stat(dst, &dst_st) == 0 && dst_st.st_size > 0) {
            char cur_hex[65];
            if (sha256_file(dst, cur_hex) && strcmp(cur_hex, chunk_hashes[i]) != 0) {
                LOG("[gamebuild] module %s: preserving local modification in %s_%03d.c\n", module_name, module_name, i);
                continue;
            }
            continue;
        }

        if (!copy_file_atomic(src, dst)) {
            g_recomp_cache.gen_misses++;
            return -1;
        }
#ifdef _WIN32
        SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
#else
        chmod(dst, 0644);
#endif
    }

    if (has_table) {
        char table_src[1200], table_dst[1200];
        snprintf(table_src, sizeof table_src, "%s" SEP "%s_table.c", entry_dir, module_name);
        snprintf(table_dst, sizeof table_dst, "%s" SEP "%s_table.c", dst_dir, module_name);
        copy_file_atomic(table_src, table_dst);
#ifdef _WIN32
        SetFileAttributesA(table_dst, FILE_ATTRIBUTE_NORMAL);
#else
        chmod(table_dst, 0644);
#endif
    }

    // Update workspace .module_key
    FILE *out_kf = fopen(key_file, "wb");
    if (out_kf) {
        fprintf(out_kf, "key=%s\nfiles=%d\n", key, expected_files);
        fclose(out_kf);
    }

    g_recomp_cache.gen_hits++;
    LOG("[gamebuild] module %s: reused from cache (%d files, immutable copy)\n", module_name, expected_files);
    return expected_files;
}

bool recomp_cache_put_module(const char *module_name, const char *nso_path,
                             const char *layout_txt_path, const char *hle_symbols_path,
                             const char *gen_tool_path, const char *gen_script_path,
                             const char *gen_options, const char *src_dir,
                             int file_count, bool has_table) {
    if (!g_recomp_cache.gen_dir[0] || file_count <= 0 || file_count > 512) return false;

    char key[65];
    if (!compute_module_key(module_name, nso_path, layout_txt_path, hle_symbols_path,
                            gen_tool_path, gen_script_path, gen_options, key)) return false;

    char entry_dir[1200], tmp_dir[1200];
    snprintf(entry_dir, sizeof entry_dir, "%s" SEP "%s_%.16s", g_recomp_cache.gen_dir, module_name, key);
    snprintf(tmp_dir, sizeof tmp_dir, "%s.tmp.%d.%u", entry_dir, (int)getpid(), ++g_temp_counter);

    remove_dir_recursive(tmp_dir);
    make_dir(tmp_dir);

    // Write manifest with chunk hashes
    char manifest_path[1200];
    snprintf(manifest_path, sizeof manifest_path, "%s" SEP "manifest.txt", tmp_dir);
    FILE *mf = fopen(manifest_path, "wb");
    if (!mf) {
        remove_dir_recursive(tmp_dir);
        return false;
    }
    fprintf(mf, "schema=%s\n", SCHEMA_GEN_VERSION);
    fprintf(mf, "module=%s\n", module_name);
    fprintf(mf, "key=%s\n", key);
    fprintf(mf, "files=%d\n", file_count);
    fprintf(mf, "has_table=%d\n", has_table ? 1 : 0);

    for (int i = 0; i < file_count; i++) {
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s" SEP "%s_%03d.c", src_dir, module_name, i);
        snprintf(dst, sizeof dst, "%s" SEP "%s_%03d.c", tmp_dir, module_name, i);
        struct stat st;
        if (stat(src, &st) != 0 || !copy_file_atomic(src, dst)) {
            fclose(mf);
            remove_dir_recursive(tmp_dir);
            return false;
        }
        char hx[65];
        if (!sha256_file(dst, hx)) {
            fclose(mf);
            remove_dir_recursive(tmp_dir);
            return false;
        }
        fprintf(mf, "hash_%03d=%s\n", i, hx);
#ifdef _WIN32
        SetFileAttributesA(dst, FILE_ATTRIBUTE_READONLY);
#else
        chmod(dst, 0444);
#endif
    }

    if (has_table) {
        char table_src[1200], table_dst[1200];
        snprintf(table_src, sizeof table_src, "%s" SEP "%s_table.c", src_dir, module_name);
        snprintf(table_dst, sizeof table_dst, "%s" SEP "%s_table.c", tmp_dir, module_name);
        struct stat tst;
        if (stat(table_src, &tst) != 0 || !copy_file_atomic(table_src, table_dst)) {
            fclose(mf);
            remove_dir_recursive(tmp_dir);
            return false;
        }
        char table_hx[65];
        if (!sha256_file(table_dst, table_hx)) {
            fclose(mf);
            remove_dir_recursive(tmp_dir);
            return false;
        }
        fprintf(mf, "hash_table=%s\n", table_hx);
#ifdef _WIN32
        SetFileAttributesA(table_dst, FILE_ATTRIBUTE_READONLY);
#else
        chmod(table_dst, 0444);
#endif
    }

    fprintf(mf, "status=complete\n");
    fclose(mf);

    // Atomic promotion of directory
    remove_dir_recursive(entry_dir);
    if (rename(tmp_dir, entry_dir) != 0) {
#ifdef _WIN32
        if (!MoveFileExA(tmp_dir, entry_dir, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            remove_dir_recursive(tmp_dir);
            return false;
        }
#else
        remove_dir_recursive(tmp_dir);
        return false;
#endif
    }

    LOG("[gamebuild] module %s: saved to cache (%d files, key=%.16s)\n", module_name, file_count, key);
    return true;
}

// ------------------------------------------------------------------ FIX 2: Strict Binary & Sidecar Promotion
bool recomp_cache_is_valid_lib(const char *path, char *why, size_t why_size) {
    if (!path) {
        if (why) snprintf(why, why_size, "library path is NULL");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        if (why) snprintf(why, why_size, "library file does not exist");
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 1024 * 1024) {
        if (why) snprintf(why, why_size, "library is truncated (%llu bytes < 1MB)", (unsigned long long)st.st_size);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (why) snprintf(why, why_size, "cannot open library for inspection");
        return false;
    }

#ifdef _WIN32
    // Windows PE validation:
    // 1. DOS Header: 64 bytes
    uint8_t dos_hdr[64];
    if (fread(dos_hdr, 1, sizeof dos_hdr, f) != sizeof dos_hdr) {
        fclose(f);
        if (why) snprintf(why, why_size, "truncated DOS header");
        return false;
    }
    if (dos_hdr[0] != 'M' || dos_hdr[1] != 'Z') {
        fclose(f);
        if (why) snprintf(why, why_size, "missing MZ signature");
        return false;
    }
    // Read e_lfanew at offset 0x3C
    uint32_t e_lfanew = (uint32_t)dos_hdr[0x3C] |
                        ((uint32_t)dos_hdr[0x3D] << 8) |
                        ((uint32_t)dos_hdr[0x3E] << 16) |
                        ((uint32_t)dos_hdr[0x3F] << 24);
    if (e_lfanew < 0x40 || (uint64_t)e_lfanew + 26 > (uint64_t)st.st_size) {
        fclose(f);
        if (why) snprintf(why, why_size, "invalid or out-of-bounds e_lfanew (0x%x)", e_lfanew);
        return false;
    }

    if (fseek(f, (long)e_lfanew, SEEK_SET) != 0) {
        fclose(f);
        if (why) snprintf(why, why_size, "cannot seek to PE header");
        return false;
    }

    // Read PE signature (4 bytes) + COFF header (20 bytes) + Optional header magic (2 bytes)
    uint8_t pe_data[26];
    if (fread(pe_data, 1, sizeof pe_data, f) != sizeof pe_data) {
        fclose(f);
        if (why) snprintf(why, why_size, "truncated PE/COFF header");
        return false;
    }
    // Check 'PE\0\0' (0x00004550)
    if (pe_data[0] != 'P' || pe_data[1] != 'E' || pe_data[2] != 0 || pe_data[3] != 0) {
        fclose(f);
        if (why) snprintf(why, why_size, "invalid PE signature");
        return false;
    }
    // Check Machine: AMD64 (0x8664)
    uint16_t machine = (uint16_t)pe_data[4] | ((uint16_t)pe_data[5] << 8);
    if (machine != 0x8664) {
        fclose(f);
        if (why) snprintf(why, why_size, "unsupported machine type (0x%04x != 0x8664 AMD64)", machine);
        return false;
    }
    // Check Optional Header Magic: PE32+ (0x020B)
    uint16_t opt_magic = (uint16_t)pe_data[24] | ((uint16_t)pe_data[25] << 8);
    if (opt_magic != 0x020B) {
        fclose(f);
        if (why) snprintf(why, why_size, "unsupported optional header magic (0x%04x != 0x020B PE32+)", opt_magic);
        return false;
    }
#else
    uint8_t elf_hdr[64];
    if (fread(elf_hdr, 1, sizeof elf_hdr, f) != sizeof elf_hdr) {
        fclose(f);
        if (why) snprintf(why, why_size, "truncated ELF header");
        return false;
    }
    if (elf_hdr[0] != 0x7F || elf_hdr[1] != 'E' || elf_hdr[2] != 'L' || elf_hdr[3] != 'F') {
        fclose(f);
        if (why) snprintf(why, why_size, "invalid ELF magic");
        return false;
    }
    if (elf_hdr[4] != 2) { // ELFCLASS64
        fclose(f);
        if (why) snprintf(why, why_size, "not ELFCLASS64");
        return false;
    }
    uint16_t e_machine = (uint16_t)elf_hdr[18] | ((uint16_t)elf_hdr[19] << 8);
    if (e_machine != 62 && e_machine != 183) { // EM_X86_64 = 62, EM_AARCH64 = 183
        fclose(f);
        if (why) snprintf(why, why_size, "unsupported ELF machine (0x%04x)", e_machine);
        return false;
    }
#endif

    fclose(f);
    return true;
}

static bool is_valid_sidecar(const char *sidecar_path, const char *expected_id) {
    if (!sidecar_path) return false;
    FILE *f = fopen(sidecar_path, "rb");
    if (!f) return false;
    char line[256] = {0};
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (expected_id && *expected_id) {
        return strcmp(line, expected_id) == 0;
    }
    return strlen(line) >= 8;
}

bool recomp_cache_promote_lib(const char *tmp_lib_path, const char *dst_lib_path,
                             const char *gamelib_id, char *why, size_t why_size) {
    if (!tmp_lib_path || !dst_lib_path || !gamelib_id) {
        if (why) snprintf(why, why_size, "invalid promotion arguments");
        return false;
    }

    // 1. Strict binary validation of the candidate library BEFORE touching working library
    if (!recomp_cache_is_valid_lib(tmp_lib_path, why, why_size)) {
        return false;
    }

    // 2. Prepare temporary .id sidecar and validate content
    char tmp_id_path[1200], dst_id_path[1200];
    snprintf(tmp_id_path, sizeof tmp_id_path, "%s.id.tmp.%d.%u", dst_lib_path, (int)getpid(), ++g_temp_counter);
    snprintf(dst_id_path, sizeof dst_id_path, "%s.id", dst_lib_path);

    FILE *idf = fopen(tmp_id_path, "wb");
    if (!idf || fprintf(idf, "%s\n", gamelib_id) < 0) {
        if (idf) fclose(idf);
        remove(tmp_id_path);
        if (why) snprintf(why, why_size, "cannot write temporary identity sidecar");
        return false;
    }
    fclose(idf);

    if (!is_valid_sidecar(tmp_id_path, gamelib_id)) {
        remove(tmp_id_path);
        if (why) snprintf(why, why_size, "temporary identity sidecar verification failed");
        return false;
    }

    // 3. Staged replacement with backup and rollback
    char dst_backup[1200], dst_id_backup[1200];
    snprintf(dst_backup, sizeof dst_backup, "%s.old", dst_lib_path);
    snprintf(dst_id_backup, sizeof dst_id_backup, "%s.id.old", dst_lib_path);

    bool dst_had_lib = recomp_cache_is_valid_lib(dst_lib_path, NULL, 0);
    bool dst_had_id = is_valid_sidecar(dst_id_path, NULL);

    if (dst_had_lib) {
        remove(dst_backup);
        if (!copy_file_direct(dst_lib_path, dst_backup)) {
            remove(tmp_id_path);
            if (why) snprintf(why, why_size, "cannot create backup of current game library");
            return false;
        }
        if (dst_had_id) {
            remove(dst_id_backup);
            copy_file_direct(dst_id_path, dst_id_backup);
        }
    }

    // 4. Promote library binary into place
#ifdef _WIN32
    bool lib_promoted = MoveFileExA(tmp_lib_path, dst_lib_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    bool lib_promoted = (rename(tmp_lib_path, dst_lib_path) == 0);
#endif
    if (!lib_promoted) {
        if (dst_had_lib) {
            copy_file_direct(dst_backup, dst_lib_path);
            if (dst_had_id) copy_file_direct(dst_id_backup, dst_id_path);
        }
        remove(tmp_id_path);
        if (why) snprintf(why, why_size, "failed to move new game library into destination");
        return false;
    }

    // 5. Promote identity sidecar into place
#ifdef _WIN32
    bool id_promoted = MoveFileExA(tmp_id_path, dst_id_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    bool id_promoted = (rename(tmp_id_path, dst_id_path) == 0);
#endif
    if (!id_promoted) {
        // Rollback both binary and sidecar!
        if (dst_had_lib) {
#ifdef _WIN32
            MoveFileExA(dst_backup, dst_lib_path, MOVEFILE_REPLACE_EXISTING);
            if (dst_had_id) MoveFileExA(dst_id_backup, dst_id_path, MOVEFILE_REPLACE_EXISTING);
#else
            rename(dst_backup, dst_lib_path);
            if (dst_had_id) rename(dst_id_backup, dst_id_path);
#endif
        } else {
            remove(dst_lib_path);
            remove(dst_id_path);
        }
        remove(tmp_id_path);
        if (why) snprintf(why, why_size, "failed to promote identity sidecar; rolled back");
        return false;
    }

    // 6. Post-promotion integrity verification
    if (!recomp_cache_is_valid_lib(dst_lib_path, NULL, 0) || !is_valid_sidecar(dst_id_path, gamelib_id)) {
        LOG("[gamebuild] WARNING: promoted library validation failed; rolling back\n");
        if (dst_had_lib) {
#ifdef _WIN32
            MoveFileExA(dst_backup, dst_lib_path, MOVEFILE_REPLACE_EXISTING);
            if (dst_had_id) MoveFileExA(dst_id_backup, dst_id_path, MOVEFILE_REPLACE_EXISTING);
#else
            rename(dst_backup, dst_lib_path);
            if (dst_had_id) rename(dst_id_backup, dst_id_path);
#endif
        } else {
            remove(dst_lib_path);
            remove(dst_id_path);
        }
        if (why) snprintf(why, why_size, "post-promotion validation failed; rolled back to previous version");
        return false;
    }

    // 7. Success: clean up backups
    remove(dst_backup);
    remove(dst_id_backup);
    LOG("[gamebuild] successfully promoted %s with identity %s\n", dst_lib_path, gamelib_id);
    return true;
}

void recomp_cache_recover_lib(const char *dst_lib_path) {
    if (!dst_lib_path) return;
    char dst_backup[1200], dst_id[1200], dst_id_backup[1200];
    snprintf(dst_backup, sizeof dst_backup, "%s.old", dst_lib_path);
    snprintf(dst_id, sizeof dst_id, "%s.id", dst_lib_path);
    snprintf(dst_id_backup, sizeof dst_id_backup, "%s.id.old", dst_lib_path);

    bool cur_lib_valid = recomp_cache_is_valid_lib(dst_lib_path, NULL, 0);
    bool cur_id_valid = is_valid_sidecar(dst_id, NULL);
    bool bak_lib_valid = recomp_cache_is_valid_lib(dst_backup, NULL, 0);
    bool bak_id_valid = is_valid_sidecar(dst_id_backup, NULL);

    if ((!cur_lib_valid || !cur_id_valid) && bak_lib_valid) {
        LOG("[gamelib] recovering interrupted game library from %s\n", dst_backup);
#ifdef _WIN32
        MoveFileExA(dst_backup, dst_lib_path, MOVEFILE_REPLACE_EXISTING);
        if (bak_id_valid) MoveFileExA(dst_id_backup, dst_id, MOVEFILE_REPLACE_EXISTING);
#else
        rename(dst_backup, dst_lib_path);
        if (bak_id_valid) rename(dst_id_backup, dst_id);
#endif
    } else if (cur_lib_valid && cur_id_valid && bak_lib_valid) {
        remove(dst_backup);
        remove(dst_id_backup);
    }
}

// ------------------------------------------------------------------ FIX 3: Multi-Tier Quota & True LRU Eviction
typedef struct {
    char path[1200];
    uint64_t size;
    uint64_t mtime;
    bool is_dir;
} CacheItem;

static int compare_cache_items_lru(const void *a, const void *b) {
    const CacheItem *ia = (const CacheItem *)a;
    const CacheItem *ib = (const CacheItem *)b;
    if (ia->mtime < ib->mtime) return -1;
    if (ia->mtime > ib->mtime) return 1;
    return 0;
}

static uint64_t get_dir_size_and_mtime(const char *dir_path, uint64_t *out_mtime) {
    uint64_t total = 0;
    uint64_t newest_mtime = 0;
#ifdef _WIN32
    char search[1200];
    snprintf(search, sizeof search, "%s\\*", dir_path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[1200];
            snprintf(child, sizeof child, "%s\\%s", dir_path, fd.cFileName);
            struct stat st;
            if (stat(child, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    uint64_t sub_mt = 0;
                    total += get_dir_size_and_mtime(child, &sub_mt);
                    if (sub_mt > newest_mtime) newest_mtime = sub_mt;
                } else {
                    total += (uint64_t)st.st_size;
                    if ((uint64_t)st.st_mtime > newest_mtime) newest_mtime = (uint64_t)st.st_mtime;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char child[1200];
            snprintf(child, sizeof child, "%s/%s", dir_path, ent->d_name);
            struct stat st;
            if (stat(child, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    uint64_t sub_mt = 0;
                    total += get_dir_size_and_mtime(child, &sub_mt);
                    if (sub_mt > newest_mtime) newest_mtime = sub_mt;
                } else {
                    total += (uint64_t)st.st_size;
                    if ((uint64_t)st.st_mtime > newest_mtime) newest_mtime = (uint64_t)st.st_mtime;
                }
            }
        }
        closedir(d);
    }
#endif
    if (out_mtime) *out_mtime = newest_mtime;
    return total;
}

void recomp_cache_enforce_limit(void) {
    if (!g_recomp_cache.root[0]) return;
    const char *lim_env = getenv("SAVAGEWIND_CACHE_MAX_GB");
    uint64_t max_bytes = DEFAULT_MAX_CACHE_BYTES;
    if (lim_env && *lim_env) {
        double gb = atof(lim_env);
        if (gb > 0) max_bytes = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
    }

    uint64_t now = (uint64_t)time(NULL);
    uint64_t protect_threshold = now > 900 ? now - 900 : 0; // Protect files modified in last 15 mins

    size_t cap = 2048, count = 0;
    CacheItem *items = malloc(cap * sizeof(CacheItem));
    if (!items) return;

    uint64_t total_bytes = 0;

    // 1. obj_dir: account for .o and .meta sidecars
    DIR *d = opendir(g_recomp_cache.obj_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strstr(ent->d_name, ".tmp")) continue; // protect active temporary files
            if (strstr(ent->d_name, ".meta")) continue; // sidecar handled with .o
            char p[1200];
            snprintf(p, sizeof p, "%s" SEP "%s", g_recomp_cache.obj_dir, ent->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
                uint64_t item_size = (uint64_t)st.st_size;
                // Include .meta sidecar in size calculation
                char meta[1200];
                snprintf(meta, sizeof meta, "%s.meta", p);
                struct stat mst;
                if (stat(meta, &mst) == 0) item_size += (uint64_t)mst.st_size;

                total_bytes += item_size;
                if (count == cap) { cap *= 2; items = realloc(items, cap * sizeof(CacheItem)); }
                snprintf(items[count].path, sizeof items[count].path, "%s", p);
                items[count].size = item_size;
                items[count].mtime = (uint64_t)st.st_mtime;
                items[count].is_dir = false;
                count++;
            }
        }
        closedir(d);
    }

    // 2. gen_dir: module directories
    d = opendir(g_recomp_cache.gen_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strstr(ent->d_name, ".tmp")) continue; // protect active temporary module builds
            char p[1200];
            snprintf(p, sizeof p, "%s" SEP "%s", g_recomp_cache.gen_dir, ent->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                uint64_t dir_mtime = 0;
                uint64_t dir_size = get_dir_size_and_mtime(p, &dir_mtime);
                total_bytes += dir_size;
                if (count == cap) { cap *= 2; items = realloc(items, cap * sizeof(CacheItem)); }
                snprintf(items[count].path, sizeof items[count].path, "%s", p);
                items[count].size = dir_size;
                items[count].mtime = dir_mtime ? dir_mtime : (uint64_t)st.st_mtime;
                items[count].is_dir = true;
                count++;
            }
        }
        closedir(d);
    }

    // 3. zig_dir: zig cache directories (safe eviction of completed subdirectories)
    if (g_recomp_cache.zig_dir[0]) {
        d = opendir(g_recomp_cache.zig_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char p[1200];
                snprintf(p, sizeof p, "%s" SEP "%s", g_recomp_cache.zig_dir, ent->d_name);
                struct stat st;
                if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                    uint64_t dir_mtime = 0;
                    uint64_t dir_size = get_dir_size_and_mtime(p, &dir_mtime);
                    total_bytes += dir_size;
                    if (count == cap) { cap *= 2; items = realloc(items, cap * sizeof(CacheItem)); }
                    snprintf(items[count].path, sizeof items[count].path, "%s", p);
                    items[count].size = dir_size;
                    items[count].mtime = dir_mtime ? dir_mtime : (uint64_t)st.st_mtime;
                    items[count].is_dir = true;
                    count++;
                }
            }
            closedir(d);
        }
    }

    if (total_bytes > max_bytes) {
        LOG("[gamebuild] total cache disk usage %llu MB exceeds quota %llu MB; enforcing LRU eviction\n",
            (unsigned long long)(total_bytes >> 20), (unsigned long long)(max_bytes >> 20));

        // Sort candidates ascending by mtime (oldest first)
        qsort(items, count, sizeof(CacheItem), compare_cache_items_lru);

        uint64_t target_bytes = (max_bytes * 8) / 10; // Target 80% of quota
        uint64_t bytes_reclaimed = 0;

        for (size_t i = 0; i < count && total_bytes > target_bytes; i++) {
            // Protect recent entries (within last 15 minutes)
            if (items[i].mtime > protect_threshold) continue;

            if (items[i].is_dir) {
                if (remove_dir_recursive(items[i].path)) {
                    total_bytes -= items[i].size;
                    bytes_reclaimed += items[i].size;
                }
            } else {
                if (remove(items[i].path) == 0) {
                    total_bytes -= items[i].size;
                    bytes_reclaimed += items[i].size;
                    char meta[1200];
                    snprintf(meta, sizeof meta, "%s.meta", items[i].path);
                    remove(meta);
                }
            }
        }

        LOG("[gamebuild] LRU cache eviction complete: reclaimed %llu MB; new total: %llu MB\n",
            (unsigned long long)(bytes_reclaimed >> 20), (unsigned long long)(total_bytes >> 20));

        if (total_bytes > max_bytes) {
            LOG("[gamebuild] WARNING: cache disk usage %llu MB still exceeds quota %llu MB (remaining entries protected by 15-minute recency window)\n",
                (unsigned long long)(total_bytes >> 20), (unsigned long long)(max_bytes >> 20));
        }
    }

    free(items);
}

void recomp_cache_log_summary(double build_seconds) {
    uint32_t total_obj = g_recomp_cache.obj_hits + g_recomp_cache.obj_misses;
    uint32_t total_gen = g_recomp_cache.gen_hits + g_recomp_cache.gen_misses;
    double obj_hit_rate = total_obj ? (100.0 * g_recomp_cache.obj_hits / total_obj) : 0.0;

    LOG("[gamebuild] cache summary: %u/%u modules reused, %u/%u objects reused (%.1f%% hit rate, %llu MB saved), build completed in %.1fs\n",
        g_recomp_cache.gen_hits, total_gen,
        g_recomp_cache.obj_hits, total_obj, obj_hit_rate,
        (unsigned long long)(g_recomp_cache.obj_bytes_reused >> 20), build_seconds);
}
