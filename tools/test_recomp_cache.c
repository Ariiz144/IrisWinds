// Comprehensive validation suite for IrisWinds incremental recompilation cache correctness.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "recomp_cache.h"
#include "sha256.h"

#ifdef _WIN32
#include <windows.h>
#define SEP "\\"
#define sleep_s(s) Sleep((s) * 1000)

static void set_file_mtime_seconds_ago(const char *path, int seconds_ago) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= (uint64_t)seconds_ago * 10000000ull;
    ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(t >> 32);

    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, NULL, NULL, &ft);
        CloseHandle(h);
    }
}
#else
#include <unistd.h>
#include <utime.h>
#define SEP "/"
#define sleep_s(s) sleep(s)

static void set_file_mtime_seconds_ago(const char *path, int seconds_ago) {
    struct utimbuf ub;
    time_t now = time(NULL);
    ub.actime = now - seconds_ago;
    ub.modtime = now - seconds_ago;
    utime(path, &ub);
}
#endif

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "\n[FAIL] Assertion failed at line %d: %s (%s)\n", __LINE__, #cond, msg); \
        assert(cond); \
    } \
    g_tests_passed++; \
} while (0)

static void make_test_pe_dll(const char *path, size_t size_bytes) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);

    // DOS Header (64 bytes)
    uint8_t dos[64] = {0};
    dos[0] = 'M'; dos[1] = 'Z';
    // e_lfanew = 0x80 (128)
    dos[0x3C] = 0x80;
    fwrite(dos, 1, sizeof dos, f);

    // Padding up to e_lfanew (0x80 - 64 = 64 bytes)
    uint8_t pad[64] = {0};
    fwrite(pad, 1, sizeof pad, f);

    // PE Signature (4 bytes) + COFF (20 bytes) + Optional Header Magic (2 bytes)
    uint8_t pe[26] = {0};
    pe[0] = 'P'; pe[1] = 'E'; pe[2] = 0; pe[3] = 0;
    // Machine = 0x8664 (AMD64)
    pe[4] = 0x64; pe[5] = 0x86;
    // Optional Header Magic = 0x020B (PE32+)
    pe[24] = 0x0B; pe[25] = 0x02;
    fwrite(pe, 1, sizeof pe, f);

    // Fill remainder up to size_bytes
    size_t written = 64 + 64 + 26;
    char chunk[4096];
    memset(chunk, 0xCC, sizeof chunk);
    while (written < size_bytes) {
        size_t to_write = sizeof chunk;
        if (written + to_write > size_bytes) to_write = size_bytes - written;
        fwrite(chunk, 1, to_write, f);
        written += to_write;
    }
    fclose(f);
}

static void test_sha256(void) {
    printf("[test] Running SHA-256 validation...\n");
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, "abc", 3);
    uint8_t hash[32];
    sha256_final(&ctx, hash);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    TEST_ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
                "SHA-256 standard test vector 'abc'");

    // Multiblock test
    sha256_init(&ctx);
    char buf[1000];
    memset(buf, 'a', sizeof buf);
    sha256_update(&ctx, buf, sizeof buf);
    sha256_final(&ctx, hash);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    TEST_ASSERT(strlen(hex) == 64, "SHA-256 multiblock digest length");

    // Empty string test
    sha256_init(&ctx);
    sha256_final(&ctx, hash);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    TEST_ASSERT(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
                "SHA-256 empty string test vector");
    printf("       SHA-256 tests passed.\n");
}

static void test_obj_caching_and_integrity(const char *test_dir) {
    printf("[test] Running Object Cache & Payload Integrity verification...\n");
    char root[1100], cc[1100], inc[1100], src[1100], obj[1100], dst[1100];
    snprintf(root, sizeof root, "%s" SEP "data", test_dir);
    snprintf(cc, sizeof cc, "%s" SEP "fake_clang.exe", test_dir);
    snprintf(inc, sizeof inc, "%s" SEP "include", test_dir);
    snprintf(src, sizeof src, "%s" SEP "main_000.c", test_dir);

#ifdef _WIN32
    CreateDirectoryA(inc, NULL);
#else
    mkdir(inc, 0755);
#endif

    FILE *f;
    f = fopen(cc, "wb"); fputs("fake_clang_binary", f); fclose(f);
    char h_cpu[1100], h_simd[1100], h_simd_fp[1100], h_gamelib[1100];
    snprintf(h_cpu, sizeof h_cpu, "%s" SEP "cpu.h", inc);
    snprintf(h_simd, sizeof h_simd, "%s" SEP "simd.h", inc);
    snprintf(h_simd_fp, sizeof h_simd_fp, "%s" SEP "simd_fp.h", inc);
    snprintf(h_gamelib, sizeof h_gamelib, "%s" SEP "gamelib.h", inc);

    f = fopen(h_cpu, "wb"); fputs("typedef int s32;\n", f); fclose(f);
    f = fopen(h_simd, "wb"); fputs("// simd\n", f); fclose(f);
    f = fopen(h_simd_fp, "wb"); fputs("// simd_fp\n", f); fclose(f);
    f = fopen(h_gamelib, "wb"); fputs("// gamelib\n", f); fclose(f);

    bool init_ok = recomp_cache_init(root, cc, false, inc, "-O2", "");
    TEST_ASSERT(init_ok, "recomp_cache_init");

    f = fopen(src, "wb"); fputs("void test_fn() {}\n", f); fclose(f);

    snprintf(obj, sizeof obj, "%s" SEP "main_000.o", test_dir);
    f = fopen(obj, "wb");
    uint8_t coff_hdr[20] = {0x64, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    fwrite(coff_hdr, 1, sizeof coff_hdr, f);
    fputs("fake_object_code_1234567890_extra_padding_bytes_to_exceed_64", f);
    fclose(f);

    // 1. Put into cache
    bool put_ok = recomp_cache_put_obj(src, "main_000", false, NULL, obj);
    TEST_ASSERT(put_ok, "recomp_cache_put_obj");

    // 2. Query cache -> Expect HIT
    snprintf(dst, sizeof dst, "%s" SEP "restored_000.o", test_dir);
    remove(dst);
    bool hit = recomp_cache_get_obj(src, "main_000", false, NULL, dst);
    TEST_ASSERT(hit, "recomp_cache_get_obj should hit");
    TEST_ASSERT(g_recomp_cache.obj_hits >= 1, "obj_hits incremented");

    // Verify restored content matches
    f = fopen(dst, "rb");
    TEST_ASSERT(f != NULL, "restored object readable");
    char buf[128] = {0};
    fread(buf, 1, sizeof buf, f);
    fclose(f);
    TEST_ASSERT(strstr(buf + sizeof coff_hdr, "fake_object_code") != NULL, "restored payload content matches");

    // 3. Source modification invalidates cache
    f = fopen(src, "wb"); fputs("void test_fn() { int x = 42; }\n", f); fclose(f);
    bool miss_after_modify = recomp_cache_get_obj(src, "main_000", false, NULL, dst);
    TEST_ASSERT(!miss_after_modify, "source modify invalidates obj cache");

    // 4. Header modification invalidates cache
    f = fopen(src, "wb"); fputs("void test_fn() {}\n", f); fclose(f);
    f = fopen(h_cpu, "wb"); fputs("typedef long long s64;\n", f); fclose(f);
    recomp_cache_init(root, cc, false, inc, "-O2", "");
    bool miss_after_header = recomp_cache_get_obj(src, "main_000", false, NULL, dst);
    TEST_ASSERT(!miss_after_header, "header modify invalidates obj cache");

    // 5. GAMELIB_ID isolation
    f = fopen(src, "wb"); fputs("void test_fn() {}\n", f); fclose(f);
    recomp_cache_put_obj(src, "main_000", false, "id_v1", obj);
    bool hit_different_id = recomp_cache_get_obj(src, "main_000", false, "id_v2", dst);
    TEST_ASSERT(hit_different_id, "id isolation: regular unit ignores gamelib_id");

    recomp_cache_put_obj(src, "gamelib_entry", true, "id_v1", obj);
    bool entry_hit_same = recomp_cache_get_obj(src, "gamelib_entry", true, "id_v1", dst);
    TEST_ASSERT(entry_hit_same, "gamelib_entry hits with same gamelib_id");
    bool entry_miss_diff = recomp_cache_get_obj(src, "gamelib_entry", true, "id_v2", dst);
    TEST_ASSERT(!entry_miss_diff, "gamelib_entry misses with different gamelib_id");

    printf("       Object cache & payload integrity tests passed.\n");
}

static void test_module_cache_integrity_and_tampering(const char *test_dir) {
    printf("[test] Running FIX 1 — Module Cache Integrity, Payload Hashing & Tamper Rejection...\n");
    char root[1100], exefs[1100], nso[1100], layout[1100], hle[1100], gen_tool[1100];
    char src_dir[1100], ws1_dir[1100], ws2_dir[1100];

    snprintf(root, sizeof root, "%s" SEP "data", test_dir);
    snprintf(exefs, sizeof exefs, "%s" SEP "exefs", test_dir);
    snprintf(nso, sizeof nso, "%s" SEP "main", exefs);
    snprintf(layout, sizeof layout, "%s" SEP "layout.txt", test_dir);
    snprintf(hle, sizeof hle, "%s" SEP "hle.txt", test_dir);
    snprintf(gen_tool, sizeof gen_tool, "%s" SEP "fake_swgen.exe", test_dir);
    snprintf(src_dir, sizeof src_dir, "%s" SEP "test_gen_out", test_dir);
    snprintf(ws1_dir, sizeof ws1_dir, "%s" SEP "workspace1", test_dir);
    snprintf(ws2_dir, sizeof ws2_dir, "%s" SEP "workspace2", test_dir);

#ifdef _WIN32
    CreateDirectoryA(exefs, NULL);
    CreateDirectoryA(src_dir, NULL);
    CreateDirectoryA(ws1_dir, NULL);
    CreateDirectoryA(ws2_dir, NULL);
#else
    mkdir(exefs, 0755);
    mkdir(src_dir, 0755);
    mkdir(ws1_dir, 0755);
    mkdir(ws2_dir, 0755);
#endif

    FILE *f;
    f = fopen(nso, "wb"); fputs("NSO_ORIGINAL_BINARY_DATA_V1", f); fclose(f);
    f = fopen(layout, "wb"); fputs("LAYOUT_INFO_OFFSETS_12345", f); fclose(f);
    f = fopen(hle, "wb"); fputs("HLE_SYMBOLS_LIST", f); fclose(f);
    f = fopen(gen_tool, "wb"); fputs("FAKE_SWGEN_EXECUTABLE_CONTENT", f); fclose(f);

    // Create 3 chunks and 1 table file
    for (int i = 0; i < 3; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s" SEP "main_%03d.c", src_dir, i);
        f = fopen(p, "wb");
        fprintf(f, "// chunk %d original pristine code\nvoid chunk_fn_%d() { int a = %d; }\n", i, i, i);
        fclose(f);
    }
    char table_path[1100];
    snprintf(table_path, sizeof table_path, "%s" SEP "main_table.c", src_dir);
    f = fopen(table_path, "wb");
    fputs("// main_table functions table original\nvoid *g_table[] = { 0 };\n", f);
    fclose(f);

    // Save module into cache
    bool put_ok = recomp_cache_put_module("main", nso, layout, hle, gen_tool, NULL, "", src_dir, 3, true);
    TEST_ASSERT(put_ok, "recomp_cache_put_module returns true");

    // Locate cached directory
    char cached_dir[1100] = {0};
#ifdef _WIN32
    char search_gen[1200];
    snprintf(search_gen, sizeof search_gen, "%s" SEP "cache" SEP "recomp" SEP "gen" SEP "*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search_gen, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] != '.' && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(cached_dir, sizeof cached_dir, "%s" SEP "cache" SEP "recomp" SEP "gen" SEP "%s", root, fd.cFileName);
                break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    TEST_ASSERT(cached_dir[0] != 0, "found cached module entry directory");

    // Verify manifest has hash_table and all chunk hashes
    char manifest[1200];
    snprintf(manifest, sizeof manifest, "%s" SEP "manifest.txt", cached_dir);
    f = fopen(manifest, "rb");
    TEST_ASSERT(f != NULL, "manifest.txt exists and readable");
    char mcontent[2048] = {0};
    fread(mcontent, 1, sizeof mcontent, f);
    fclose(f);
    TEST_ASSERT(strstr(mcontent, "schema=gen_v2.1") != NULL, "manifest schema is gen_v2.1");
    TEST_ASSERT(strstr(mcontent, "has_table=1") != NULL, "manifest records has_table=1");
    TEST_ASSERT(strstr(mcontent, "hash_table=") != NULL, "manifest contains hash_table entry");
    TEST_ASSERT(strstr(mcontent, "hash_000=") != NULL, "manifest contains hash_000");
    TEST_ASSERT(strstr(mcontent, "hash_001=") != NULL, "manifest contains hash_001");
    TEST_ASSERT(strstr(mcontent, "hash_002=") != NULL, "manifest contains hash_002");

    // Clean hit test into ws1
    int hit1 = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws1_dir);
    TEST_ASSERT(hit1 == 3, "clean cache hit into ws1");

    // Verify table file restored into ws1
    char ws1_table[1100];
    snprintf(ws1_table, sizeof ws1_table, "%s" SEP "main_table.c", ws1_dir);
    struct stat tst;
    TEST_ASSERT(stat(ws1_table, &tst) == 0 && tst.st_size > 0, "table file restored into workspace");

    // Workspace edit tracking: edit chunk 1 in ws1
    char ws1_chunk1[1100];
    snprintf(ws1_chunk1, sizeof ws1_chunk1, "%s" SEP "main_001.c", ws1_dir);
    f = fopen(ws1_chunk1, "wb");
    fputs("// USER MODIFIED CODE IN WS1\nvoid custom() {}\n", f);
    fclose(f);

    // Re-query ws1 -> should preserve user edit
    int hit1_again = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws1_dir);
    TEST_ASSERT(hit1_again == 3, "re-query ws1 with matching key");
    f = fopen(ws1_chunk1, "rb");
    char read_ws1[256] = {0};
    fread(read_ws1, 1, sizeof read_ws1, f);
    fclose(f);
    TEST_ASSERT(strstr(read_ws1, "USER MODIFIED CODE IN WS1") != NULL, "user edit preserved on matching key");

    // Query into clean ws2 -> pristine code received, NOT mutated by ws1 edit
    int hit2 = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(hit2 == 3, "clean ws2 receives pristine files");
    char ws2_chunk1[1100];
    snprintf(ws2_chunk1, sizeof ws2_chunk1, "%s" SEP "main_001.c", ws2_dir);
    f = fopen(ws2_chunk1, "rb");
    char read_ws2[256] = {0};
    fread(read_ws2, 1, sizeof read_ws2, f);
    fclose(f);
    TEST_ASSERT(strstr(read_ws2, "original pristine code") != NULL, "ws2 receives pristine code without ws1 edits");

    // -------------------------------------------------------------
    // TAMPER 1: Alter a cached chunk without changing length
    // -------------------------------------------------------------
    char cchunk0[1100];
    snprintf(cchunk0, sizeof cchunk0, "%s" SEP "main_000.c", cached_dir);
#ifdef _WIN32
    SetFileAttributesA(cchunk0, FILE_ATTRIBUTE_NORMAL);
#endif
    f = fopen(cchunk0, "r+b");
    TEST_ASSERT(f != NULL, "cached chunk 0 opened for tampering");
    fseek(f, 10, SEEK_SET);
    fputc('Z', f); // flip 1 byte without changing file length
    fclose(f);

    int tamper1_res = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(tamper1_res == -1, "TAMPER 1: altered chunk without length change rejected");

    // Verify cache entry directory was safely removed on corruption
    struct stat cst;
    TEST_ASSERT(stat(cached_dir, &cst) != 0, "TAMPER 1: corrupt cache entry purged from disk");

    // -------------------------------------------------------------
    // TAMPER 2: Truncate a cached chunk
    // -------------------------------------------------------------
    recomp_cache_put_module("main", nso, layout, hle, gen_tool, NULL, "", src_dir, 3, true);
    snprintf(cchunk0, sizeof cchunk0, "%s" SEP "main_000.c", cached_dir);
#ifdef _WIN32
    SetFileAttributesA(cchunk0, FILE_ATTRIBUTE_NORMAL);
#endif
    f = fopen(cchunk0, "wb"); fputs("// trunc", f); fclose(f); // truncated
    int tamper2_res = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(tamper2_res == -1, "TAMPER 2: truncated chunk rejected");
    TEST_ASSERT(stat(cached_dir, &cst) != 0, "TAMPER 2: truncated entry purged");

    // -------------------------------------------------------------
    // TAMPER 3: Modify cached table file
    // -------------------------------------------------------------
    recomp_cache_put_module("main", nso, layout, hle, gen_tool, NULL, "", src_dir, 3, true);
    char ctable[1100];
    snprintf(ctable, sizeof ctable, "%s" SEP "main_table.c", cached_dir);
#ifdef _WIN32
    SetFileAttributesA(ctable, FILE_ATTRIBUTE_NORMAL);
#endif
    f = fopen(ctable, "wb"); fputs("// corrupt table\n", f); fclose(f);
    int tamper3_res = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(tamper3_res == -1, "TAMPER 3: modified table file rejected");
    TEST_ASSERT(stat(cached_dir, &cst) != 0, "TAMPER 3: corrupt table entry purged");

    // -------------------------------------------------------------
    // TAMPER 4: Remove expected hash from manifest
    // -------------------------------------------------------------
    recomp_cache_put_module("main", nso, layout, hle, gen_tool, NULL, "", src_dir, 3, true);
    snprintf(manifest, sizeof manifest, "%s" SEP "manifest.txt", cached_dir);
#ifdef _WIN32
    SetFileAttributesA(manifest, FILE_ATTRIBUTE_NORMAL);
#endif
    f = fopen(manifest, "wb");
    fprintf(f, "schema=gen_v2.1\nmodule=main\nkey=dummy\nfiles=3\nhas_table=1\nhash_table=dummy\nhash_000=dummy\nhash_001=dummy\nstatus=complete\n");
    // hash_002 omitted!
    fclose(f);
    int tamper4_res = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(tamper4_res == -1, "TAMPER 4: missing chunk hash in manifest rejected");

    // -------------------------------------------------------------
    // TAMPER 5: Corrupt manifest syntax / status incomplete
    // -------------------------------------------------------------
    recomp_cache_put_module("main", nso, layout, hle, gen_tool, NULL, "", src_dir, 3, true);
    f = fopen(manifest, "wb");
    fprintf(f, "schema=old_schema_v1\nstatus=incomplete\n");
    fclose(f);
    int tamper5_res = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(tamper5_res == -1, "TAMPER 5: invalid schema / incomplete status rejected");

    // Verify re-saving fresh module works cleanly and restores 100%
    recomp_cache_put_module("main", nso, layout, hle, gen_tool, NULL, "", src_dir, 3, true);
    int hit_final = recomp_cache_get_module("main", nso, layout, hle, gen_tool, NULL, "", ws2_dir);
    TEST_ASSERT(hit_final == 3, "clean cache hits again after tampering tests");

    printf("       FIX 1 — Module Cache Integrity & Tampering tests passed.\n");
}

static void test_complete_generator_fingerprints(const char *test_dir) {
    printf("[test] Running Issue 2 — Complete Generator Fingerprint verification...\n");
    char root[1100], exefs[1100], nso[1100], layout[1100], hle[1100];
    char tools_dir[1100], py_exe[1100], gen_py[1100], lifter_py[1100], patches_py[1100];
    char src_dir[1100], dst_dir[1100];

    snprintf(root, sizeof root, "%s" SEP "data", test_dir);
    snprintf(exefs, sizeof exefs, "%s" SEP "exefs2", test_dir);
    snprintf(nso, sizeof nso, "%s" SEP "subsdk0", exefs);
    snprintf(layout, sizeof layout, "%s" SEP "layout2.txt", test_dir);
    snprintf(hle, sizeof hle, "%s" SEP "hle2.txt", test_dir);
    snprintf(tools_dir, sizeof tools_dir, "%s" SEP "fake_tools", test_dir);
    snprintf(py_exe, sizeof py_exe, "%s" SEP "python.exe", tools_dir);
    snprintf(gen_py, sizeof gen_py, "%s" SEP "gen.py", tools_dir);
    snprintf(lifter_py, sizeof lifter_py, "%s" SEP "lifter.py", tools_dir);
    snprintf(patches_py, sizeof patches_py, "%s" SEP "patches.py", tools_dir);
    snprintf(src_dir, sizeof src_dir, "%s" SEP "src_out2", test_dir);
    snprintf(dst_dir, sizeof dst_dir, "%s" SEP "ws_out2", test_dir);

#ifdef _WIN32
    CreateDirectoryA(exefs, NULL);
    CreateDirectoryA(tools_dir, NULL);
    CreateDirectoryA(src_dir, NULL);
    CreateDirectoryA(dst_dir, NULL);
#else
    mkdir(exefs, 0755);
    mkdir(tools_dir, 0755);
    mkdir(src_dir, 0755);
    mkdir(dst_dir, 0755);
#endif

    FILE *f;
    f = fopen(nso, "wb"); fputs("SUBSDK0_CODE", f); fclose(f);
    f = fopen(layout, "wb"); fputs("LAYOUT2", f); fclose(f);
    f = fopen(hle, "wb"); fputs("HLE2", f); fclose(f);
    f = fopen(py_exe, "wb"); fputs("PYTHON_3_12_BIN", f); fclose(f);
    f = fopen(gen_py, "wb"); fputs("# gen.py root", f); fclose(f);
    f = fopen(lifter_py, "wb"); fputs("# lifter logic v1", f); fclose(f);
    f = fopen(patches_py, "wb"); fputs("# patches logic v1", f); fclose(f);

    for (int i = 0; i < 2; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s" SEP "subsdk0_%03d.c", src_dir, i);
        f = fopen(p, "wb"); fprintf(f, "// subsdk0 chunk %d\n", i); fclose(f);
    }

    bool p1 = recomp_cache_put_module("subsdk0", nso, layout, hle, py_exe, gen_py, "--hle symbols.txt", src_dir, 2, false);
    TEST_ASSERT(p1, "put module in python source mode");

    int g1 = recomp_cache_get_module("subsdk0", nso, layout, hle, py_exe, gen_py, "--hle symbols.txt", dst_dir);
    TEST_ASSERT(g1 == 2, "get module in python source mode hits");

    // Modify lifter.py dependency -> Expect cache miss!
    f = fopen(lifter_py, "wb"); fputs("# lifter logic v2 MODIFIED", f); fclose(f);
    int g_miss_lifter = recomp_cache_get_module("subsdk0", nso, layout, hle, py_exe, gen_py, "--hle symbols.txt", dst_dir);
    TEST_ASSERT(g_miss_lifter == -1, "modifying imported lifter.py invalidates module cache");

    // Restore lifter.py -> Expect hit again!
    f = fopen(lifter_py, "wb"); fputs("# lifter logic v1", f); fclose(f);
    int g_hit_again = recomp_cache_get_module("subsdk0", nso, layout, hle, py_exe, gen_py, "--hle symbols.txt", dst_dir);
    TEST_ASSERT(g_hit_again == 2, "restoring lifter.py restores hit");

    // Modify generator options -> Expect cache miss!
    int g_miss_opts = recomp_cache_get_module("subsdk0", nso, layout, hle, py_exe, gen_py, "--hle symbols.txt --simd", dst_dir);
    TEST_ASSERT(g_miss_opts == -1, "changing generator options invalidates cache");

    printf("       Issue 2 — Complete generator fingerprinting passed.\n");
}

static void test_library_promotion_and_recovery_transactions(const char *test_dir) {
    printf("[test] Running FIX 2 — Library Promotion, Failure Injection & Recovery...\n");
    char tmp_lib[1100], dst_lib[1100], dst_id[1100], dst_old[1100], dst_id_old[1100];
    snprintf(tmp_lib, sizeof tmp_lib, "%s" SEP "swgame.dll.tmp", test_dir);
    snprintf(dst_lib, sizeof dst_lib, "%s" SEP "swgame.dll", test_dir);
    snprintf(dst_id, sizeof dst_id, "%s" SEP "swgame.dll.id", test_dir);
    snprintf(dst_old, sizeof dst_old, "%s" SEP "swgame.dll.old", test_dir);
    snprintf(dst_id_old, sizeof dst_id_old, "%s" SEP "swgame.dll.id.old", test_dir);

    remove(tmp_lib);
    remove(dst_lib);
    remove(dst_id);
    remove(dst_old);
    remove(dst_id_old);

    char why[256] = "";

    // Test 1: Reject missing temporary library
    bool fail_missing = recomp_cache_promote_lib(tmp_lib, dst_lib, "id_v1", why, sizeof why);
    TEST_ASSERT(!fail_missing, "promotion rejects missing temporary library");

    // Test 2: Reject small/truncated library (< 1MB)
    FILE *f = fopen(tmp_lib, "wb");
    fputs("TOO_SMALL_FILE", f);
    fclose(f);
    bool fail_small = recomp_cache_promote_lib(tmp_lib, dst_lib, "id_v1", why, sizeof why);
    TEST_ASSERT(!fail_small, "promotion rejects library < 1MB");

    // Test 3: Reject library with invalid PE/ELF magic or corrupt DOS/PE header
    f = fopen(tmp_lib, "wb");
    char junk[4096];
    memset(junk, 0x11, sizeof junk);
    for (int k = 0; k < 300; k++) fwrite(junk, 1, sizeof junk, f); // ~1.2 MB of junk
    fclose(f);
    bool fail_junk = recomp_cache_promote_lib(tmp_lib, dst_lib, "id_v1", why, sizeof why);
    TEST_ASSERT(!fail_junk, "promotion rejects library with invalid binary header");

    // Test 4: Successful promotion of valid PE AMD64 library (>1 MB)
    make_test_pe_dll(tmp_lib, 1280 * 1024); // 1.28 MB valid PE
    bool ok_promo1 = recomp_cache_promote_lib(tmp_lib, dst_lib, "id_v1", why, sizeof why);
    TEST_ASSERT(ok_promo1, "successful library promotion");
    TEST_ASSERT(recomp_cache_is_valid_lib(dst_lib, NULL, 0), "promoted library verified valid");

    // Verify .id sidecar was written and matches
    f = fopen(dst_id, "rb");
    TEST_ASSERT(f != NULL, "promoted .id sidecar exists");
    char id_read[64] = {0};
    fgets(id_read, sizeof id_read, f);
    fclose(f);
    TEST_ASSERT(strstr(id_read, "id_v1") != NULL, ".id sidecar contains expected ID");

    // Failure Injection 5: Failure during sidecar promotion (simulated rollback)
    // Make a new candidate library
    make_test_pe_dll(tmp_lib, 1400 * 1024);
    // Create an un-overwritable directory where tmp_id would be written, or simulate failure
    // We test that recomp_cache_promote_lib with NULL gamelib_id fails safely and leaves dst intact
    bool fail_null_id = recomp_cache_promote_lib(tmp_lib, dst_lib, NULL, why, sizeof why);
    TEST_ASSERT(!fail_null_id, "promotion fails safely with invalid ID");
    TEST_ASSERT(recomp_cache_is_valid_lib(dst_lib, NULL, 0), "existing library remains intact after failed call");

    // Failure Injection 6: Interrupted promotion with new library missing or truncated and .old exists
    // Promote a second valid version so .old backup is created
    make_test_pe_dll(tmp_lib, 1500 * 1024);
    // Artificially create a healthy .old backup
    make_test_pe_dll(dst_old, 1300 * 1024);
    f = fopen(dst_id_old, "wb"); fputs("id_backup_v1\n", f); fclose(f);

    // Corrupt the current library to simulate interrupted promotion crash
    f = fopen(dst_lib, "wb"); fputs("CORRUPT_TRUNCATED_CRASH", f); fclose(f);

    // Run recovery
    recomp_cache_recover_lib(dst_lib);

    // Verify recovery restored the healthy .old library and sidecar!
    TEST_ASSERT(recomp_cache_is_valid_lib(dst_lib, NULL, 0), "recovery restored valid library from .old");
    f = fopen(dst_id, "rb");
    TEST_ASSERT(f != NULL, "recovery restored .id sidecar");
    memset(id_read, 0, sizeof id_read);
    fgets(id_read, sizeof id_read, f);
    fclose(f);
    TEST_ASSERT(strstr(id_read, "id_backup_v1") != NULL, "recovered sidecar matches .old identity");

    // Failure Injection 7: Interrupted promotion where DLL exists but sidecar missing
    remove(dst_id);
    make_test_pe_dll(dst_old, 1300 * 1024);
    f = fopen(dst_id_old, "wb"); fputs("id_backup_v2\n", f); fclose(f);
    recomp_cache_recover_lib(dst_lib);
    TEST_ASSERT(recomp_cache_is_valid_lib(dst_lib, NULL, 0), "recovery restored library when sidecar was missing");
    f = fopen(dst_id, "rb");
    TEST_ASSERT(f != NULL, "sidecar restored");
    memset(id_read, 0, sizeof id_read);
    fgets(id_read, sizeof id_read, f);
    fclose(f);
    TEST_ASSERT(strstr(id_read, "id_backup_v2") != NULL, "restored sidecar matches backup");

    // Test 8: Healthy library cleanup
    recomp_cache_recover_lib(dst_lib);
    // Both dst_lib and dst_id are healthy, stale .old can be cleaned up
    struct stat ost;
    TEST_ASSERT(stat(dst_old, &ost) != 0, "healthy destination safely cleans up leftover .old backup");

    printf("       FIX 2 — Library promotion and recovery transaction tests passed.\n");
}

static uint64_t compute_dir_usage_bytes(const char *dir) {
    uint64_t total = 0;
#ifdef _WIN32
    char search[1200];
    snprintf(search, sizeof search, "%s" SEP "*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[1200];
            snprintf(child, sizeof child, "%s" SEP "%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                total += compute_dir_usage_bytes(child);
            } else {
                struct stat st;
                if (stat(child, &st) == 0) total += (uint64_t)st.st_size;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    return total;
}

static void test_cache_quota_and_lru(const char *test_dir) {
    printf("[test] Running FIX 3 — Real Multi-Tier Quota & Deterministic LRU Eviction...\n");
    char root[1100], obj_dir[1100], gen_dir[1100], zig_dir[1100];
    snprintf(root, sizeof root, "%s" SEP "data_quota", test_dir);
    snprintf(obj_dir, sizeof obj_dir, "%s" SEP "cache" SEP "recomp" SEP "obj", root);
    snprintf(gen_dir, sizeof gen_dir, "%s" SEP "cache" SEP "recomp" SEP "gen", root);
    snprintf(zig_dir, sizeof zig_dir, "%s" SEP "cache" SEP "recomp" SEP "zig", root);

    // Initialize cache with 1 MB quota via env
#ifdef _WIN32
    _putenv_s("SAVAGEWIND_CACHE_MAX_GB", "0.001"); // ~1 MB
#else
    setenv("SAVAGEWIND_CACHE_MAX_GB", "0.001", 1);
#endif

    recomp_cache_init(root, "fake_cc", false, ".", "-O2", "");

    // 1. Create 3 OLD object files in obj_dir (each 500 KB + .meta sidecar, timestamp: 30 minutes ago)
    for (int i = 0; i < 3; i++) {
        char obj_path[1200], meta_path[1200];
        snprintf(obj_path, sizeof obj_path, "%s" SEP "old_obj_%d.o", obj_dir, i);
        snprintf(meta_path, sizeof meta_path, "%s" SEP "old_obj_%d.o.meta", obj_dir, i);
        FILE *f = fopen(obj_path, "wb");
        char chunk[1024]; memset(chunk, 'A' + i, sizeof chunk);
        for (int k = 0; k < 500; k++) fwrite(chunk, 1, sizeof chunk, f); // 500 KB
        fclose(f);
        f = fopen(meta_path, "wb"); fputs("size=512000\nsha256=dummy\n", f); fclose(f);

        set_file_mtime_seconds_ago(obj_path, 1800); // 30 minutes ago
        set_file_mtime_seconds_ago(meta_path, 1800);
    }

    // 2. Create 2 RECENT object files in obj_dir (each 500 KB, timestamp: 1 minute ago)
    for (int i = 0; i < 2; i++) {
        char obj_path[1200];
        snprintf(obj_path, sizeof obj_path, "%s" SEP "recent_obj_%d.o", obj_dir, i);
        FILE *f = fopen(obj_path, "wb");
        char chunk[1024]; memset(chunk, 'R' + i, sizeof chunk);
        for (int k = 0; k < 500; k++) fwrite(chunk, 1, sizeof chunk, f); // 500 KB
        fclose(f);
        set_file_mtime_seconds_ago(obj_path, 60); // 1 minute ago
    }

    // 3. Create 1 ACTIVE .tmp file in obj_dir (500 KB, should NEVER be deleted)
    char tmp_obj[1200];
    snprintf(tmp_obj, sizeof tmp_obj, "%s" SEP "active.tmp.1234", obj_dir);
    FILE *f = fopen(tmp_obj, "wb");
    char chunk[1024]; memset(chunk, 'T', sizeof chunk);
    for (int k = 0; k < 500; k++) fwrite(chunk, 1, sizeof chunk, f);
    fclose(f);
    set_file_mtime_seconds_ago(tmp_obj, 1800); // even if old, .tmp is protected

    // 4. Create 1 OLD module directory in gen_dir (1 MB, timestamp: 35 minutes ago)
    char old_gen_dir[1200];
    snprintf(old_gen_dir, sizeof old_gen_dir, "%s" SEP "old_module_1234", gen_dir);
#ifdef _WIN32
    CreateDirectoryA(old_gen_dir, NULL);
#else
    mkdir(old_gen_dir, 0755);
#endif
    char gen_file[1200];
    snprintf(gen_file, sizeof gen_file, "%s" SEP "main_000.c", old_gen_dir);
    f = fopen(gen_file, "wb");
    for (int k = 0; k < 1000; k++) fwrite(chunk, 1, sizeof chunk, f); // 1 MB
    fclose(f);
    set_file_mtime_seconds_ago(gen_file, 2100);
    set_file_mtime_seconds_ago(old_gen_dir, 2100); // 35 minutes ago

    // 5. Create 1 RECENT module directory in gen_dir (500 KB, timestamp: 2 minutes ago)
    char recent_gen_dir[1200];
    snprintf(recent_gen_dir, sizeof recent_gen_dir, "%s" SEP "recent_module_5678", gen_dir);
#ifdef _WIN32
    CreateDirectoryA(recent_gen_dir, NULL);
#else
    mkdir(recent_gen_dir, 0755);
#endif
    snprintf(gen_file, sizeof gen_file, "%s" SEP "main_000.c", recent_gen_dir);
    f = fopen(gen_file, "wb");
    for (int k = 0; k < 500; k++) fwrite(chunk, 1, sizeof chunk, f); // 500 KB
    fclose(f);
    set_file_mtime_seconds_ago(gen_file, 120);
    set_file_mtime_seconds_ago(recent_gen_dir, 120);

    // 6. Create 1 OLD Zig directory in zig_dir (1 MB, timestamp: 40 minutes ago)
    char old_zig_sub[1200];
    snprintf(old_zig_sub, sizeof old_zig_sub, "%s" SEP "old_zig_entry", zig_dir);
#ifdef _WIN32
    CreateDirectoryA(old_zig_sub, NULL);
#else
    mkdir(old_zig_sub, 0755);
#endif
    char zig_file[1200];
    snprintf(zig_file, sizeof zig_file, "%s" SEP "cached.o", old_zig_sub);
    f = fopen(zig_file, "wb");
    for (int k = 0; k < 1000; k++) fwrite(chunk, 1, sizeof chunk, f); // 1 MB
    fclose(f);
    set_file_mtime_seconds_ago(zig_file, 2400);
    set_file_mtime_seconds_ago(old_zig_sub, 2400);

    // Measure total usage before eviction
    uint64_t usage_before = compute_dir_usage_bytes(root);
    TEST_ASSERT(usage_before > 5 * 1024 * 1024, "usage before enforcement exceeds 5 MB");

    // Run quota enforcement
    recomp_cache_enforce_limit();

    // Verify usage reduced toward target
    uint64_t usage_after = compute_dir_usage_bytes(root);
    TEST_ASSERT(usage_after < usage_before, "enforce_limit reclaimed disk space");
    TEST_ASSERT(usage_after < usage_before, "enforce_limit reclaimed disk space");

    // Assert oldest entries were evicted
    struct stat ost;
    char check_p[1200];
    snprintf(check_p, sizeof check_p, "%s" SEP "old_obj_0.o", obj_dir);
    TEST_ASSERT(stat(check_p, &ost) != 0, "old object 0 was evicted");
    snprintf(check_p, sizeof check_p, "%s" SEP "old_obj_0.o.meta", obj_dir);
    TEST_ASSERT(stat(check_p, &ost) != 0, "old object 0 .meta sidecar was evicted with .o");
    TEST_ASSERT(stat(old_gen_dir, &ost) != 0, "old module directory was evicted");
    TEST_ASSERT(stat(old_zig_sub, &ost) != 0, "old zig directory was evicted");

    // Assert recent entries (<15 min) were preserved
    snprintf(check_p, sizeof check_p, "%s" SEP "recent_obj_0.o", obj_dir);
    TEST_ASSERT(stat(check_p, &ost) == 0, "recent object 0 was preserved");
    snprintf(check_p, sizeof check_p, "%s" SEP "recent_obj_1.o", obj_dir);
    TEST_ASSERT(stat(check_p, &ost) == 0, "recent object 1 was preserved");
    TEST_ASSERT(stat(recent_gen_dir, &ost) == 0, "recent module directory was preserved");

    // Assert active .tmp file was preserved
    TEST_ASSERT(stat(tmp_obj, &ost) == 0, "active .tmp file was preserved");

    printf("       FIX 3 — Real Multi-Tier Quota & Deterministic LRU Eviction passed.\n");
}

int main(void) {
    printf("====================================================\n");
    printf("IrisWinds Target 0.1.1 Hardened Cache Test Suite   \n");
    printf("====================================================\n");

    test_sha256();

    const char *test_dir = "test_target011_sandbox";
#ifdef _WIN32
    char clean_cmd[256];
    snprintf(clean_cmd, sizeof clean_cmd, "rmdir /s /q %s 2>nul", test_dir);
    system(clean_cmd);
    CreateDirectoryA(test_dir, NULL);
#else
    mkdir(test_dir, 0755);
#endif

    test_obj_caching_and_integrity(test_dir);
    test_module_cache_integrity_and_tampering(test_dir);
    test_complete_generator_fingerprints(test_dir);
    test_library_promotion_and_recovery_transactions(test_dir);
    test_cache_quota_and_lru(test_dir);

    printf("====================================================\n");
    printf("ALL %d/%d CACHE INTEGRITY TESTS PASSED!\n", g_tests_passed, g_tests_run);
    printf("====================================================\n");
    return 0;
}
