// Persistent content-addressed build cache for IrisWinds game code recompilation.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char root[1100];        // <data_root>/cache/recomp
    char obj_dir[1100];     // <data_root>/cache/recomp/obj
    char gen_dir[1100];     // <data_root>/cache/recomp/gen
    char zig_dir[1100];     // <data_root>/cache/recomp/zig

    char compiler_fingerprint[65];
    char headers_fingerprint[65];
    char cflags_fingerprint[65];

    // Build session statistics
    uint32_t gen_hits;
    uint32_t gen_misses;
    uint32_t obj_hits;
    uint32_t obj_misses;
    uint64_t obj_bytes_reused;
} RecompCache;

extern RecompCache g_recomp_cache;

// Initializes the persistent build cache inside data_root/cache/recomp.
// Computes baseline fingerprints for the compiler, headers, and flags.
bool recomp_cache_init(const char *data_root, const char *cc_path, bool is_zig,
                       const char *inc_dir, const char *cflags, const char *zigflags);

// Returns the persistent directory to use for ZIG_GLOBAL_CACHE_DIR and ZIG_LOCAL_CACHE_DIR
const char *recomp_cache_zig_dir(void);

// ------------------------------------------------------------------ Object cache
// Checks if a precompiled object exists for src_path.
// If found, restores it to dst_obj_path and returns true.
bool recomp_cache_get_obj(const char *src_path, const char *stem, bool uses_gamelib_id,
                          const char *gamelib_id, const char *dst_obj_path);

// Saves a newly compiled object into the persistent cache.
bool recomp_cache_put_obj(const char *src_path, const char *stem, bool uses_gamelib_id,
                          const char *gamelib_id, const char *compiled_obj_path);

// ------------------------------------------------------------------ Module generation cache
// Checks if cached generated code exists for a module.
// If valid, restores all chunk C files and the table file into dst_dir and returns file count.
// Returns -1 on cache miss.
int recomp_cache_get_module(const char *module_name, const char *nso_path,
                            const char *layout_txt_path, const char *hle_symbols_path,
                            const char *gen_tool_path, const char *dst_dir);

// Saves successfully generated module code into the persistent cache.
bool recomp_cache_put_module(const char *module_name, const char *nso_path,
                             const char *layout_txt_path, const char *hle_symbols_path,
                             const char *gen_tool_path, const char *src_dir,
                             int file_count, bool has_table);

// Checks cache disk usage against limit (default 10 GB or SAVAGEWIND_CACHE_MAX_GB) and evicts oldest entries.
void recomp_cache_enforce_limit(void);

// Formats a summary of cache activity during the build session.
void recomp_cache_log_summary(double build_seconds);

#ifdef __cplusplus
}
#endif
