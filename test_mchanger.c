/*
 * test_mchanger - Tests for mchanger library
 *
 * Run with: make test
 *
 * Note: Most tests require a physical changer device to be connected.
 * Tests that require hardware will be skipped if no device is found.
 */

#include "mchanger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_skipped = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-50s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
} while(0)

#define PASS() do { printf("[PASS]\n"); tests_passed++; return; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); return; } while(0)
#define SKIP(msg) do { printf("[SKIP] %s\n", msg); tests_skipped++; return; } while(0)

#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); } while(0)
#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_NE(a, b, msg) ASSERT((a) != (b), msg)
#define ASSERT_NULL(p, msg) ASSERT((p) == NULL, msg)
#define ASSERT_NOT_NULL(p, msg) ASSERT((p) != NULL, msg)

/* Global changer handle for tests that need hardware */
static MChangerHandle *g_changer = NULL;
static bool g_has_hardware = false;

/*
 * =============================================================================
 * Basic API Tests (no hardware required)
 * =============================================================================
 */

TEST(list_changers_returns_valid) {
    MChangerHandleInfo *list = NULL;
    size_t count = 999;

    int rc = mchanger_list_changers(&list, &count);
    ASSERT_EQ(rc, MCHANGER_OK, "mchanger_list_changers should return OK");
    /* count should be set to actual number (may be 0) */
    ASSERT(count != 999, "count should be modified");

    if (count > 0) {
        ASSERT_NOT_NULL(list, "list should be non-NULL when count > 0");
        /* Check first entry has some data */
        ASSERT(strlen(list[0].vendor) > 0 || strlen(list[0].product) > 0,
               "first entry should have vendor or product");
    }

    mchanger_free_changer_list(list);
    PASS();
}

TEST(list_changers_null_params) {
    int rc = mchanger_list_changers(NULL, NULL);
    ASSERT_EQ(rc, MCHANGER_ERR_INVALID, "should return INVALID for NULL params");
    PASS();
}

TEST(free_changer_list_null_safe) {
    /* Should not crash */
    mchanger_free_changer_list(NULL);
    PASS();
}

TEST(open_null_safe) {
    /* Opening with no device should return NULL or valid handle */
    /* This test just ensures it doesn't crash */
    MChangerHandle *ch = mchanger_open(NULL);
    if (ch) {
        mchanger_close(ch);
    }
    PASS();
}

TEST(close_null_safe) {
    /* Should not crash */
    mchanger_close(NULL);
    PASS();
}

TEST(free_element_map_null_safe) {
    mchanger_free_element_map(NULL);
    PASS();
}

TEST(api_null_changer_returns_invalid) {
    MChangerElementMap map;
    MChangerElementStatus status;

    ASSERT_EQ(mchanger_get_element_map(NULL, &map), MCHANGER_ERR_INVALID, "get_element_map");
    ASSERT_EQ(mchanger_get_slot_status(NULL, 1, &status), MCHANGER_ERR_INVALID, "get_slot_status");
    ASSERT_EQ(mchanger_get_drive_status(NULL, 1, &status), MCHANGER_ERR_INVALID, "get_drive_status");
    ASSERT_EQ(mchanger_load_slot(NULL, 1, 1), MCHANGER_ERR_INVALID, "load_slot");
    ASSERT_EQ(mchanger_unload_drive(NULL, 1, 1), MCHANGER_ERR_INVALID, "unload_drive");
    ASSERT_EQ(mchanger_eject(NULL, 1, 1), MCHANGER_ERR_INVALID, "eject");
    ASSERT_EQ(mchanger_move_medium(NULL, 0, 0, 0), MCHANGER_ERR_INVALID, "move_medium");
    ASSERT_EQ(mchanger_test_unit_ready(NULL), MCHANGER_ERR_INVALID, "test_unit_ready");

    PASS();
}

TEST(api_invalid_slot_returns_invalid) {
    if (!g_has_hardware) SKIP("no hardware");

    MChangerElementStatus status;
    ASSERT_EQ(mchanger_get_slot_status(g_changer, 0, &status), MCHANGER_ERR_INVALID, "slot 0");
    ASSERT_EQ(mchanger_get_slot_status(g_changer, -1, &status), MCHANGER_ERR_INVALID, "slot -1");
    ASSERT_EQ(mchanger_load_slot(g_changer, 0, 1), MCHANGER_ERR_INVALID, "load slot 0");

    PASS();
}

/*
 * =============================================================================
 * Hardware Tests (require connected changer)
 * =============================================================================
 */

TEST(open_and_close) {
    if (!g_has_hardware) SKIP("no hardware");

    /* Just verify the global handle is valid - we already opened it in main() */
    ASSERT_NOT_NULL(g_changer, "global changer should be open");
    PASS();
}

TEST(test_unit_ready) {
    if (!g_has_hardware) SKIP("no hardware");

    int rc = mchanger_test_unit_ready(g_changer);
    ASSERT_EQ(rc, MCHANGER_OK, "device should be ready");
    PASS();
}

TEST(inquiry) {
    if (!g_has_hardware) SKIP("no hardware");

    char vendor[64] = {0};
    char product[64] = {0};
    char revision[16] = {0};

    int rc = mchanger_inquiry(g_changer, vendor, sizeof(vendor),
                          product, sizeof(product),
                          revision, sizeof(revision));
    ASSERT_EQ(rc, MCHANGER_OK, "inquiry should succeed");
    ASSERT(strlen(vendor) > 0, "vendor should be set");
    ASSERT(strlen(product) > 0, "product should be set");

    PASS();
}

TEST(get_element_map) {
    if (!g_has_hardware) SKIP("no hardware");

    MChangerElementMap map = {0};
    int rc = mchanger_get_element_map(g_changer, &map);
    ASSERT_EQ(rc, MCHANGER_OK, "should get element map");
    ASSERT(map.slot_count > 0, "should have slots");
    ASSERT(map.drive_count > 0, "should have drives");
    ASSERT(map.transport_count > 0, "should have transports");
    ASSERT_NOT_NULL(map.slot_addrs, "slot_addrs should be allocated");
    ASSERT_NOT_NULL(map.drive_addrs, "drive_addrs should be allocated");

    mchanger_free_element_map(&map);

    /* Verify map was cleared */
    ASSERT_NULL(map.slot_addrs, "slot_addrs should be NULL after free");
    ASSERT_EQ(map.slot_count, 0, "slot_count should be 0 after free");

    PASS();
}

TEST(get_slot_status) {
    if (!g_has_hardware) SKIP("no hardware");

    MChangerElementStatus status = {0};
    int rc = mchanger_get_slot_status(g_changer, 1, &status);
    ASSERT_EQ(rc, MCHANGER_OK, "should get slot 1 status");
    ASSERT(status.address != 0, "address should be set");
    /* full can be true or false, just check it's a valid response */

    PASS();
}

TEST(get_drive_status) {
    if (!g_has_hardware) SKIP("no hardware");

    MChangerElementStatus status = {0};
    int rc = mchanger_get_drive_status(g_changer, 1, &status);
    ASSERT_EQ(rc, MCHANGER_OK, "should get drive 1 status");
    ASSERT(status.address != 0, "address should be set");

    PASS();
}

TEST(load_same_slot_is_noop) {
    if (!g_has_hardware) SKIP("no hardware");

    /* Get current drive status */
    MChangerElementStatus drive_st = {0};
    int rc = mchanger_get_drive_status(g_changer, 1, &drive_st);
    ASSERT_EQ(rc, MCHANGER_OK, "should get drive status");

    if (!drive_st.full || !drive_st.valid_source) {
        SKIP("drive empty or no source info");
    }

    /* Find which slot the disc came from */
    MChangerElementMap map = {0};
    rc = mchanger_get_element_map(g_changer, &map);
    ASSERT_EQ(rc, MCHANGER_OK, "should get map");

    int source_slot = 0;
    for (size_t i = 0; i < map.slot_count; i++) {
        if (map.slot_addrs[i] == drive_st.source_addr) {
            source_slot = (int)(i + 1);
            break;
        }
    }
    mchanger_free_element_map(&map);

    if (source_slot == 0) {
        SKIP("couldn't find source slot");
    }

    /* Loading the same slot should be a no-op */
    rc = mchanger_load_slot(g_changer, source_slot, 1);
    ASSERT_EQ(rc, MCHANGER_OK, "loading same slot should succeed (no-op)");

    PASS();
}

/*
 * =============================================================================
 * Main
 * =============================================================================
 */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("mchanger library tests\n");
    printf("==========================\n\n");

    /* Check if hardware is available */
    MChangerHandleInfo *list = NULL;
    size_t count = 0;
    mchanger_list_changers(&list, &count);
    g_has_hardware = (count > 0);

    if (g_has_hardware) {
        printf("Found %zu changer(s): %s %s\n", count, list[0].vendor, list[0].product);
        g_changer = mchanger_open_ex(NULL, true, true); /* force, skip TUR for faster tests */
        if (!g_changer) {
            printf("Warning: Could not open changer, hardware tests will be skipped\n");
            g_has_hardware = false;
        }
    } else {
        printf("No changer hardware found, hardware tests will be skipped\n");
    }
    mchanger_free_changer_list(list);

    printf("\nRunning tests...\n\n");

    /* Basic API tests */
    printf("Basic API tests:\n");
    RUN_TEST(list_changers_returns_valid);
    RUN_TEST(list_changers_null_params);
    RUN_TEST(free_changer_list_null_safe);
    RUN_TEST(open_null_safe);
    RUN_TEST(close_null_safe);
    RUN_TEST(free_element_map_null_safe);
    RUN_TEST(api_null_changer_returns_invalid);
    RUN_TEST(api_invalid_slot_returns_invalid);

    /* Hardware tests */
    printf("\nHardware tests:\n");
    RUN_TEST(open_and_close);
    RUN_TEST(test_unit_ready);
    RUN_TEST(inquiry);
    RUN_TEST(get_element_map);
    RUN_TEST(get_slot_status);
    RUN_TEST(get_drive_status);
    RUN_TEST(load_same_slot_is_noop);

    /* Cleanup */
    if (g_changer) {
        mchanger_close(g_changer);
    }

    /* Summary */
    printf("\n==========================\n");
    printf("Tests: %d | Passed: %d | Failed: %d | Skipped: %d\n",
           tests_run, tests_passed, tests_run - tests_passed - tests_skipped, tests_skipped);

    return (tests_passed + tests_skipped == tests_run) ? 0 : 1;
}
