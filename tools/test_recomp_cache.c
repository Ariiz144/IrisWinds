#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "recomp_cache.h"
#include "sha256.h"

#ifdef _WIN32
#include <windows.h>
#define SEP "\\"
#else
#include <unistd.h>
#define SEP "/"
#endif

static void test_sha256(void) {
    printf("[test] Running SHA-256 validation...\n");
    // Standard test vector: "abc"
    // sha256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, "abc", 3);
    uint8_t hash[32];
    sha256_final(&ctx, hash);
    char hex[65];
    sha256_to_hex(hash, hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    // Empty string
    // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    sha256_init(&ctx);
    sha256_final(&ctx, hash);
    sha256_to_hex(hash, hex);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    printf("       SHA-256 test passed.\n");
}

static void test_obj_caching(const char *test_dir) {
    printf("[test] Running Object Cache verification...\n");
    char root[1100], inc[1100], cc[1100], src[1100], obj[1100], dst[1100];
    snprintf(root, sizeof root, "%s" SEP "data", test_dir);
    snprintf(inc, sizeof inc, "%s" SEP "inc", test_dir);
    snprintf(cc, sizeof cc, "%s" SEP "fake_cc.exe", test_dir);

#ifdef _WIN32
    CreateDirectoryA(test_dir, NULL);
    CreateDirectoryA(inc, NULL);
#else
    mkdir(test_dir, 0755);
    mkdir(inc, 0755);
#endif

    // Create fake compiler and header
    FILE *f = fopen(cc, "wb"); fputs("fake-compiler-v1", f); fclose(f);
    char h_cpu[1100];
    snprintf(h_cpu, sizeof h_cpu, "%s" SEP "cpu.h", inc);
    f = fopen(h_cpu, "wb"); fputs("typedef int s32;\n", f); fclose(f);

    // Init cache
    bool init_ok = recomp_cache_init(root, cc, false, inc, "-O2", "");
    assert(init_ok);

    // Create test source file
    snprintf(src, sizeof src, "%s" SEP "main_000.c", test_dir);
    f = fopen(src, "wb"); fputs("void test_fn() {}\n", f); fclose(f);

    // Create fake compiled object
    snprintf(obj, sizeof obj, "%s" SEP "main_000.o", test_dir);
    f = fopen(obj, "wb"); fputs("\x7f" "ELF" "fake_object_code_1234567890_extra_padding_bytes_to_exceed_32", f); fclose(f);

    // 1. Put into cache
    bool put_ok = recomp_cache_put_obj(src, "main_000", false, NULL, obj);
    assert(put_ok);

    // 2. Query cache -> Expect HIT
    snprintf(dst, sizeof dst, "%s" SEP "restored_000.o", test_dir);
    remove(dst);
    bool hit = recomp_cache_get_obj(src, "main_000", false, NULL, dst);
    assert(hit);
    assert(g_recomp_cache.obj_hits == 1);

    // Verify restored content matches
    f = fopen(dst, "rb");
    assert(f != NULL);
    char buf[64] = {0};
    fread(buf, 1, sizeof buf, f);
    fclose(f);
    assert(strstr(buf, "fake_object_code") != NULL);

    // 3. Scenario 3 Verification: Source modification invalidates cache
    f = fopen(src, "wb"); fputs("void test_fn() { int x = 42; }\n", f); fclose(f);
    bool miss_after_modify = recomp_cache_get_obj(src, "main_000", false, NULL, dst);
    assert(!miss_after_modify);
    assert(g_recomp_cache.obj_misses == 1);

    // 4. Scenario 4 Verification: Header modification invalidates cache
    // Re-init with modified header
    f = fopen(src, "wb"); fputs("void test_fn() {}\n", f); fclose(f); // restore original source
    // modify header
    f = fopen(h_cpu, "wb"); fputs("typedef long long s64;\n", f); fclose(f);
    recomp_cache_init(root, cc, false, inc, "-O2", "");
    bool miss_after_header = recomp_cache_get_obj(src, "main_000", false, NULL, dst);
    assert(!miss_after_header);

    // 5. Macro Isolation Verification: GAMELIB_ID isolation
    f = fopen(src, "wb"); fputs("void test_fn() {}\n", f); fclose(f);
    // Put with uses_gamelib_id = false
    recomp_cache_put_obj(src, "main_000", false, "id_v1", obj);
    // Query with different id -> Should still HIT because uses_gamelib_id = false!
    bool hit_different_id = recomp_cache_get_obj(src, "main_000", false, "id_v2", dst);
    assert(hit_different_id);

    // Now test gamelib_entry with uses_gamelib_id = true
    recomp_cache_put_obj(src, "gamelib_entry", true, "id_v1", obj);
    bool entry_hit_same = recomp_cache_get_obj(src, "gamelib_entry", true, "id_v1", dst);
    assert(entry_hit_same);
    bool entry_miss_diff = recomp_cache_get_obj(src, "gamelib_entry", true, "id_v2", dst);
    assert(!entry_miss_diff);

    printf("       Object cache tests passed (Hit, Miss, Invalidation, Macro isolation).\n");
}

static void test_module_caching(const char *test_dir) {
    printf("[test] Running Module Generation Cache verification...\n");
    char root[1100], exefs[1100], nso[1100], layout[1100], hle[1100], gen_tool[1100];
    char src_dir[1100], dst_dir[1100];

    snprintf(root, sizeof root, "%s" SEP "data", test_dir);
    snprintf(exefs, sizeof exefs, "%s" SEP "exefs", test_dir);
    snprintf(nso, sizeof nso, "%s" SEP "main", exefs);
    snprintf(layout, sizeof layout, "%s" SEP "layout.txt", test_dir);
    snprintf(hle, sizeof hle, "%s" SEP "hle.txt", test_dir);
    snprintf(gen_tool, sizeof gen_tool, "%s" SEP "gen.exe", test_dir);
    snprintf(src_dir, sizeof src_dir, "%s" SEP "gen_src", test_dir);
    snprintf(dst_dir, sizeof dst_dir, "%s" SEP "gen_dst", test_dir);

#ifdef _WIN32
    CreateDirectoryA(exefs, NULL);
    CreateDirectoryA(src_dir, NULL);
    CreateDirectoryA(dst_dir, NULL);
#else
    mkdir(exefs, 0755);
    mkdir(src_dir, 0755);
    mkdir(dst_dir, 0755);
#endif

    FILE *f = fopen(nso, "wb"); fputs("fake_nso_data_12345678", f); fclose(f);
    f = fopen(layout, "wb"); fputs("main 7000000000 1d37000\n", f); fclose(f);
    f = fopen(hle, "wb"); fputs("hle_sym_1\n", f); fclose(f);
    f = fopen(gen_tool, "wb"); fputs("generator_binary_code", f); fclose(f);

    // Create 3 fake chunks and 1 table
    for (int i = 0; i < 3; i++) {
        char cp[1100];
        snprintf(cp, sizeof cp, "%s" SEP "main_%03d.c", src_dir, i);
        f = fopen(cp, "wb"); fprintf(f, "/* chunk %d code */\n", i); fclose(f);
    }
    char tp[1100];
    snprintf(tp, sizeof tp, "%s" SEP "main_table.c", src_dir);
    f = fopen(tp, "wb"); fputs("/* table code */\n", f); fclose(f);

    // 1. Put module into cache
    bool put_ok = recomp_cache_put_module("main", nso, layout, hle, gen_tool, src_dir, 3, true);
    assert(put_ok);

    // 2. Query module from cache
    int files_restored = recomp_cache_get_module("main", nso, layout, hle, gen_tool, dst_dir);
    assert(files_restored == 3);

    // Verify chunk files exist in dst
    for (int i = 0; i < 3; i++) {
        char cp[1100];
        snprintf(cp, sizeof cp, "%s" SEP "main_%03d.c", dst_dir, i);
        f = fopen(cp, "rb");
        assert(f != NULL);
        char cbuf[64] = {0};
        fread(cbuf, 1, sizeof cbuf, f);
        fclose(f);
        char expected[64];
        snprintf(expected, sizeof expected, "/* chunk %d code */", i);
        assert(strstr(cbuf, expected) != NULL);
    }

    // 3. User modification preservation:
    // If user modifies main_001.c in dst_dir, recomp_cache_get_module must NOT overwrite it!
    char edit_file[1100];
    snprintf(edit_file, sizeof edit_file, "%s" SEP "main_001.c", dst_dir);
    f = fopen(edit_file, "wb"); fputs("/* USER MODIFIED CODE */\n", f); fclose(f);

    int query_again = recomp_cache_get_module("main", nso, layout, hle, gen_tool, dst_dir);
    assert(query_again == 3);

    f = fopen(edit_file, "rb");
    assert(f != NULL);
    char mbuf[64] = {0};
    fread(mbuf, 1, sizeof mbuf, f);
    fclose(f);
    assert(strstr(mbuf, "USER MODIFIED CODE") != NULL);

    // 4. Invalidation when NSO changes
    f = fopen(nso, "wb"); fputs("CHANGED_NSO_DATA_NEW_VERSION", f); fclose(f);
    int miss_after_nso = recomp_cache_get_module("main", nso, layout, hle, gen_tool, dst_dir);
    assert(miss_after_nso == -1);

    printf("       Module cache tests passed (Restore, User Preserv, NSO Invalidation).\n");
}

int main(void) {
    printf("=========================================\n");
    printf("IrisWinds Recompilation Cache Test Suite \n");
    printf("=========================================\n");

    test_sha256();

    const char *test_dir = "test_cache_sandbox";
    test_obj_caching(test_dir);
    test_module_caching(test_dir);

    printf("=========================================\n");
    printf("ALL 12/12 CACHE VERIFICATION TESTS PASSED!\n");
    printf("=========================================\n");
    return 0;
}
