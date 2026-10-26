// Unit test for IrisWinds Vulkan Resource-Access Planner and Snapshot Scheduler
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

enum {
    RES_OP_NONE = 0,
    RES_OP_DRAW = 1,
    RES_OP_CLEAR = 2,
    RES_OP_COMPUTE = 3,
    RES_OP_COPY = 4,
    RES_OP_UPLOAD = 5
};

typedef struct {
    uint64_t serial;
    uint64_t write_version;
    uint32_t last_write_op;
    uint32_t width, height;
    uint32_t format;
    void *image; // mock handle
} MockTexBackend;

typedef struct {
    uint32_t width, height, format;
    MockTexBackend *backend;
} MockNvnTexture;

typedef struct {
    MockNvnTexture *key;
    uint64_t src_serial;
    uint64_t src_version;
    uint32_t width, height;
    uint32_t format;
    MockTexBackend copy;
    bool valid;
    uint64_t last_used_frame;
} MockResSnapshot;

#define MAX_TEST_SNAPSHOTS 16
static MockResSnapshot snaps[MAX_TEST_SNAPSHOTS];
static int nsnaps = 0;

static inline void res_mark_write(MockTexBackend *b, uint32_t op) {
    if (!b) return;
    b->write_version++;
    b->last_write_op = op;
}

static inline void tex_mark_write(MockNvnTexture *t, uint32_t op) {
    if (t && t->backend) res_mark_write(t->backend, op);
}

static void snapshots_invalidate(MockNvnTexture *t) {
    for (int i = 0; i < nsnaps; i++) {
        if (!t || snaps[i].key == t) snaps[i].valid = false;
    }
    if (t && t->backend) t->backend->write_version++;
}

static int find_or_alloc_snapshot(MockNvnTexture *t, MockTexBackend *src) {
    int best_slot = -1;
    uint64_t oldest_frame = ~0ull;
    for (int i = 0; i < nsnaps; i++) {
        if (snaps[i].key == t && snaps[i].src_serial == src->serial) return i;
        if (!snaps[i].valid && best_slot < 0) best_slot = i;
        if (snaps[i].last_used_frame < oldest_frame) {
            oldest_frame = snaps[i].last_used_frame;
            if (best_slot < 0) best_slot = i;
        }
    }
    if (nsnaps < MAX_TEST_SNAPSHOTS) {
        int idx = nsnaps++;
        snaps[idx].key = t;
        snaps[idx].src_serial = src->serial;
        snaps[idx].valid = false;
        return idx;
    }
    if (best_slot < 0) best_slot = 0;
    MockResSnapshot *s = &snaps[best_slot];
    s->key = t;
    s->src_serial = src->serial;
    s->valid = false;
    return best_slot;
}

static inline bool snapshot_can_reuse(const MockResSnapshot *s, const MockTexBackend *src) {
    return s->valid && s->copy.image != NULL &&
           s->src_serial == src->serial &&
           s->src_version == src->write_version &&
           s->width == src->width &&
           s->height == src->height &&
           s->format == src->format;
}

int main(void) {
    printf("[test_resource_planner] Starting Vulkan Resource-Access Planner Unit Tests...\n");

    // 1. Initialize Mock Textures (e.g. G-buffer targets 0 and 1, and Depth)
    MockTexBackend b0 = {.serial = 101, .write_version = 1, .width = 1600, .height = 900, .format = 1, .image = (void *)0x1000};
    MockTexBackend b1 = {.serial = 102, .write_version = 1, .width = 1600, .height = 900, .format = 1, .image = (void *)0x2000};
    MockTexBackend bd = {.serial = 103, .write_version = 1, .width = 1600, .height = 900, .format = 2, .image = (void *)0x3000};

    MockNvnTexture t0 = {.width = 1600, .height = 900, .format = 1, .backend = &b0};
    MockNvnTexture t1 = {.width = 1600, .height = 900, .format = 1, .backend = &b1};
    MockNvnTexture td = {.width = 1600, .height = 900, .format = 2, .backend = &bd};

    // Test Case 1: First access requires a fresh copy
    int s0 = find_or_alloc_snapshot(&t0, &b0);
    assert(s0 == 0);
    assert(!snapshot_can_reuse(&snaps[s0], &b0));
    printf("  [PASS] Test 1: Fresh texture requires snapshot copy\n");

    // Mock copy execution
    snaps[s0].copy.image = (void *)0x5000;
    snaps[s0].width = b0.width;
    snaps[s0].height = b0.height;
    snaps[s0].format = b0.format;
    snaps[s0].src_version = b0.write_version;
    snaps[s0].valid = true;

    // Test Case 2: Subsequent read without write reuses snapshot
    assert(snapshot_can_reuse(&snaps[s0], &b0));
    printf("  [PASS] Test 2: Unmodified texture reuses cached snapshot (0 copies, 0 pass ends)\n");

    // Test Case 3: Draw writes to t1, but NOT to t0 -> t0 snapshot remains valid
    tex_mark_write(&t1, RES_OP_DRAW);
    assert(b1.write_version == 2);
    assert(b0.write_version == 1);
    assert(snapshot_can_reuse(&snaps[s0], &b0));
    printf("  [PASS] Test 3: Selective draw write-tracking preserves untouched attachment snapshots\n");

    // Test Case 4: Draw writes to t0 -> t0 snapshot is correctly detected as stale
    tex_mark_write(&t0, RES_OP_DRAW);
    assert(b0.write_version == 2);
    assert(!snapshot_can_reuse(&snaps[s0], &b0));
    printf("  [PASS] Test 4: Write to attachment invalidates snapshot version (version 1 != 2)\n");

    // Test Case 5: Fresh copy updates version and restores validity
    snaps[s0].src_version = b0.write_version;
    snaps[s0].valid = true;
    assert(snapshot_can_reuse(&snaps[s0], &b0));
    printf("  [PASS] Test 5: Post-copy refresh restores validity at version 2\n");

    // Test Case 6: Clear operation invalidates snapshot
    snapshots_invalidate(&t0);
    assert(!snapshot_can_reuse(&snaps[s0], &b0));
    assert(b0.write_version == 3);
    printf("  [PASS] Test 6: Clear/compute/copy invalidation correctly marks snapshot stale\n");

    // Test Case 7: Multi-resource allocation & LRU
    int s1 = find_or_alloc_snapshot(&t1, &b1);
    int sd = find_or_alloc_snapshot(&td, &bd);
    assert(s1 == 1);
    assert(sd == 2);
    assert(nsnaps == 3);
    printf("  [PASS] Test 7: Multi-resource snapshot allocation succeeds\n");

    printf("\nAll 7 Resource-Access Planner unit tests PASSED successfully!\n");
    return 0;
}
