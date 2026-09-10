/*
 * test_mchanger - Software tests and safe hardware qualification for libmchanger
 *
 * Default:              make test
 * Read-only hardware:   make qualification
 * Mechanical roundtrip: make motion-test SLOT=<known-occupied-slot>
 *
 * The motion test refuses to run unless its selected slot is full and drive 1
 * is empty. It loads that slot, verifies the element state, and returns the
 * same disc to the same slot. No import/export operation is tested here.
 */

#include "mchanger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_skipped = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-50s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
} while (0)

#define PASS() do { printf("[PASS]\n"); tests_passed++; return; } while (0)
#define FAIL(msg) do { printf("[FAIL] %s\n", (msg)); return; } while (0)
#define SKIP(msg) do { printf("[SKIP] %s\n", (msg)); tests_skipped++; return; } while (0)

#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); } while (0)
#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_NULL(p, msg) ASSERT((p) == NULL, msg)
#define ASSERT_NOT_NULL(p, msg) ASSERT((p) != NULL, msg)

static MChangerHandle *g_changer = NULL;
static bool g_has_hardware = false;
static bool g_require_hardware = false;
static int g_expected_slots = 0;
static int g_motion_slot = 0;

#define REQUIRE_HARDWARE() do { \
    if (!g_has_hardware) { \
        if (g_require_hardware) FAIL("connected changer required but none was found"); \
        SKIP("no hardware"); \
    } \
} while (0)

static bool parse_positive_int(const char *value, int *out) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!value[0] || !end || *end != '\0' || parsed < 1 || parsed > 65535) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [--require-hardware] [--expect-slots N] [--motion-slot N]\n"
            "\n"
            "  --require-hardware  Fail instead of skipping when no changer is found.\n"
            "  --expect-slots N    Require the reported storage-slot count to equal N.\n"
            "  --motion-slot N     Load known occupied slot N into drive 1 and return it.\n",
            program);
}

static bool list_has_duplicates(const uint16_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (values[i] == values[j]) return true;
        }
    }
    return false;
}

static bool lists_overlap(const uint16_t *left, size_t left_count,
                          const uint16_t *right, size_t right_count) {
    for (size_t i = 0; i < left_count; i++) {
        for (size_t j = 0; j < right_count; j++) {
            if (left[i] == right[j]) return true;
        }
    }
    return false;
}

static bool wait_for_slot_and_drive(int slot,
                                    bool expected_slot_full,
                                    bool expected_drive_full,
                                    int timeout_seconds,
                                    MChangerElementStatus *out_slot,
                                    MChangerElementStatus *out_drive) {
    for (int elapsed = 0; elapsed <= timeout_seconds; elapsed++) {
        MChangerElementStatus slot_status = {0};
        MChangerElementStatus drive_status = {0};
        if (mchanger_get_slot_status(g_changer, slot, &slot_status) == MCHANGER_OK &&
            mchanger_get_drive_status(g_changer, 1, &drive_status) == MCHANGER_OK &&
            slot_status.full == expected_slot_full &&
            drive_status.full == expected_drive_full) {
            if (out_slot) *out_slot = slot_status;
            if (out_drive) *out_drive = drive_status;
            return true;
        }
        if (elapsed < timeout_seconds) sleep(1);
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Software/API tests                                                        */
/* ------------------------------------------------------------------------- */

TEST(list_changers_returns_valid) {
    MChangerHandleInfo *list = (MChangerHandleInfo *)(uintptr_t)1;
    size_t count = 999;

    int rc = mchanger_list_changers(&list, &count);
    ASSERT_EQ(rc, MCHANGER_OK, "device discovery should complete successfully");
    ASSERT(count != 999, "count should be initialized");

    if (count == 0) {
        ASSERT_NULL(list, "empty discovery should return a NULL list");
    } else {
        ASSERT_NOT_NULL(list, "non-empty discovery should return a list");
        ASSERT(strlen(list[0].vendor) > 0 || strlen(list[0].product) > 0,
               "first entry should have a vendor or product");
    }

    mchanger_free_changer_list(list);
    PASS();
}

TEST(list_changers_null_params) {
    MChangerHandleInfo *list = NULL;
    size_t count = 0;
    ASSERT_EQ(mchanger_list_changers(NULL, &count), MCHANGER_ERR_INVALID,
              "NULL list should be invalid");
    ASSERT_EQ(mchanger_list_changers(&list, NULL), MCHANGER_ERR_INVALID,
              "NULL count should be invalid");
    PASS();
}

TEST(null_safe_cleanup) {
    mchanger_free_changer_list(NULL);
    mchanger_free_element_map(NULL);
    mchanger_close(NULL);
    PASS();
}

TEST(open_null_safe) {
    /* The qualification harness already owns the changer exclusively. Opening
       a second handle can contend with the active SCSI task user client. */
    if (g_has_hardware) PASS();
    MChangerHandle *changer = mchanger_open(NULL);
    if (changer) mchanger_close(changer);
    PASS();
}

TEST(api_null_changer_returns_invalid) {
    MChangerElementMap map = {0};
    MChangerElementStatus status = {0};

    ASSERT_EQ(mchanger_get_element_map(NULL, &map), MCHANGER_ERR_INVALID, "get_element_map");
    ASSERT_EQ(mchanger_get_slot_status(NULL, 1, &status), MCHANGER_ERR_INVALID, "get_slot_status");
    ASSERT_EQ(mchanger_get_drive_status(NULL, 1, &status), MCHANGER_ERR_INVALID, "get_drive_status");
    ASSERT_EQ(mchanger_get_bulk_status(NULL, NULL, 0, 0, NULL, &status, NULL),
              MCHANGER_ERR_INVALID, "get_bulk_status");
    ASSERT_EQ(mchanger_load_slot(NULL, 1, 1), MCHANGER_ERR_INVALID, "load_slot");
    ASSERT_EQ(mchanger_unload_drive(NULL, 1, 1), MCHANGER_ERR_INVALID, "unload_drive");
    ASSERT_EQ(mchanger_eject(NULL, 1, 1), MCHANGER_ERR_INVALID, "eject");
    ASSERT_EQ(mchanger_import_slot(NULL, 1), MCHANGER_ERR_INVALID, "import_slot");
    MChangerElementStatus ie_status = {0};
    ASSERT_EQ(mchanger_get_ie_status(NULL, 1, &ie_status), MCHANGER_ERR_INVALID,
              "get_ie_status");
    ASSERT_EQ(mchanger_export_slot(NULL, 1), MCHANGER_ERR_INVALID, "export_slot");
    ASSERT_EQ(mchanger_move_medium(NULL, 0, 0, 0), MCHANGER_ERR_INVALID, "move_medium");
    ASSERT_EQ(mchanger_set_import_export_access(NULL, 1, true), MCHANGER_ERR_INVALID,
              "set_import_export_access");
    ASSERT_EQ(mchanger_test_unit_ready(NULL), MCHANGER_ERR_INVALID, "test_unit_ready");
    ASSERT_EQ(mchanger_get_registry_identity(NULL, NULL, 0, NULL, 0, NULL, 0),
              MCHANGER_ERR_INVALID, "registry_identity");
    PASS();
}

TEST(open_close_ie_cdb_layout) {
    uint8_t cdb[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_EQ(mchanger_test_build_open_close_ie_cdb(0x1234, true, cdb),
              MCHANGER_OK, "build open CDB");
    const uint8_t expected_open[6] = {0x1B, 0x00, 0x12, 0x34, 0x00, 0x00};
    ASSERT(memcmp(cdb, expected_open, sizeof(cdb)) == 0,
           "OPEN I/E CDB does not match SMC-3 layout");

    ASSERT_EQ(mchanger_test_build_open_close_ie_cdb(0x00FE, false, cdb),
              MCHANGER_OK, "build close CDB");
    const uint8_t expected_close[6] = {0x1B, 0x00, 0x00, 0xFE, 0x01, 0x00};
    ASSERT(memcmp(cdb, expected_close, sizeof(cdb)) == 0,
           "CLOSE I/E CDB does not match SMC-3 layout");
    PASS();
}

TEST(open_close_ie_is_never_retried) {
    const uint8_t open_ie[6] = {0x1B, 0, 0, 1, 0, 0};
    const uint8_t read_status[12] = {0xB8};
    ASSERT(!mchanger_test_cdb_is_retryable(open_ie, sizeof(open_ie)),
           "state-changing I/E command must not be retryable");
    ASSERT(mchanger_test_cdb_is_retryable(read_status, sizeof(read_status)),
           "read-only status command should remain retryable");
    PASS();
}

TEST(move_medium_cdb_layout) {
    uint8_t cdb[12];
    memset(cdb, 0xFF, sizeof(cdb));
    ASSERT_EQ(mchanger_test_build_move_medium_cdb(0x0102, 0x0304, 0x0506, cdb),
              MCHANGER_OK, "build MOVE MEDIUM CDB");
    const uint8_t expected[12] = {
        0xA5, 0x00, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT(memcmp(cdb, expected, sizeof(cdb)) == 0,
           "MOVE MEDIUM CDB does not match SMC layout");
    ASSERT_EQ(mchanger_test_build_move_medium_cdb(0, 0, 0, NULL),
              MCHANGER_ERR_INVALID, "NULL MOVE MEDIUM CDB output");
    PASS();
}

TEST(read_element_status_cdb_layout) {
    uint8_t cdb[12];
    memset(cdb, 0xFF, sizeof(cdb));
    ASSERT_EQ(mchanger_test_build_read_element_status_cdb(
                  0x02, 0x1234, 0x00C8, 4096, cdb),
              MCHANGER_OK, "build READ ELEMENT STATUS CDB");
    const uint8_t expected[12] = {
        0xB8, 0x02, 0x12, 0x34, 0x00, 0xC8,
        0x00, 0x00, 0x10, 0x00, 0x00, 0x00
    };
    ASSERT(memcmp(cdb, expected, sizeof(cdb)) == 0,
           "READ ELEMENT STATUS CDB does not match SMC layout");
    ASSERT_EQ(mchanger_test_build_read_element_status_cdb(
                  0, 0, 0, 0x1000000, cdb),
              MCHANGER_ERR_INVALID, "reject oversized allocation length");
    PASS();
}

static size_t append_status_page(uint8_t *buffer, size_t offset, uint8_t type,
                                 const uint16_t *addresses,
                                 const uint8_t *flags, size_t count) {
    const uint16_t descriptor_length = 16;
    size_t page_bytes = count * descriptor_length;
    buffer[offset] = type;
    buffer[offset + 2] = (uint8_t)(descriptor_length >> 8);
    buffer[offset + 3] = (uint8_t)descriptor_length;
    buffer[offset + 5] = (uint8_t)(page_bytes >> 16);
    buffer[offset + 6] = (uint8_t)(page_bytes >> 8);
    buffer[offset + 7] = (uint8_t)page_bytes;
    offset += 8;
    for (size_t i = 0; i < count; i++) {
        buffer[offset] = (uint8_t)(addresses[i] >> 8);
        buffer[offset + 1] = (uint8_t)addresses[i];
        buffer[offset + 2] = flags[i];
        offset += descriptor_length;
    }
    return offset;
}

static size_t finish_status_report(uint8_t *buffer, size_t length,
                                   uint16_t element_count) {
    size_t report_bytes = length - 8;
    buffer[2] = (uint8_t)(element_count >> 8);
    buffer[3] = (uint8_t)element_count;
    buffer[5] = (uint8_t)(report_bytes >> 16);
    buffer[6] = (uint8_t)(report_bytes >> 8);
    buffer[7] = (uint8_t)report_bytes;
    return length;
}

TEST(paginated_bulk_status_parser) {
    const uint16_t slot_addresses[] = {4, 39, 40, 41};
    MChangerElementStatus slots[4] = {
        {.address = 4, .except = true},
        {.address = 39, .except = true},
        {.address = 40, .except = true},
        {.address = 41, .except = true}
    };
    MChangerElementStatus drive = {.address = 2};
    bool drive_supported = false;

    uint8_t first[128] = {0};
    size_t first_length = 8;
    const uint16_t drive_address[] = {2};
    const uint8_t drive_flags[] = {1};
    first_length = append_status_page(first, first_length, 0x04,
                                      drive_address, drive_flags, 1);
    const uint16_t first_slots[] = {4, 39, 0};
    const uint8_t first_flags[] = {1, 1, 0};
    first_length = append_status_page(first, first_length, 0x02,
                                      first_slots, first_flags, 3);
    finish_status_report(first, first_length, 4);

    uint16_t next = 0;
    ASSERT_EQ(mchanger_test_parse_bulk_status_report(
                  first, first_length, 0, slot_addresses, 4, 2,
                  &drive, slots, &drive_supported, &next),
              MCHANGER_OK, "parse first XL1B status chunk");
    ASSERT_EQ(next, 40, "next request should start after the last real descriptor");
    ASSERT(drive_supported && drive.full, "drive page should be retained");
    ASSERT(slots[0].full && !slots[0].except, "slot 4 should be occupied");
    ASSERT(slots[1].full && !slots[1].except, "slot 39 should be occupied");
    ASSERT(slots[2].except && slots[3].except,
           "unreported slots must remain unknown after the first chunk");

    uint8_t second[96] = {0};
    size_t second_length = 8;
    const uint16_t second_slots[] = {40, 41};
    const uint8_t second_flags[] = {0, 1};
    second_length = append_status_page(second, second_length, 0x02,
                                       second_slots, second_flags, 2);
    finish_status_report(second, second_length, 2);

    ASSERT_EQ(mchanger_test_parse_bulk_status_report(
                  second, second_length, 40, slot_addresses, 4, 2,
                  &drive, slots, &drive_supported, &next),
              MCHANGER_OK, "parse second XL1B status chunk");
    ASSERT_EQ(next, 42, "pagination should advance across the second chunk");
    ASSERT(!slots[2].full && !slots[2].except, "slot 40 should be verified empty");
    ASSERT(slots[3].full && !slots[3].except, "slot 41 should be occupied");
    PASS();
}

TEST(api_invalid_indices_return_invalid) {
    REQUIRE_HARDWARE();

    MChangerElementStatus status = {0};
    ASSERT_EQ(mchanger_get_slot_status(g_changer, 0, &status), MCHANGER_ERR_INVALID, "slot 0");
    ASSERT_EQ(mchanger_get_slot_status(g_changer, -1, &status), MCHANGER_ERR_INVALID, "slot -1");
    ASSERT_EQ(mchanger_get_drive_status(g_changer, 0, &status), MCHANGER_ERR_INVALID, "drive 0");
    ASSERT_EQ(mchanger_load_slot(g_changer, 0, 1), MCHANGER_ERR_INVALID, "load slot 0");
    ASSERT_EQ(mchanger_unload_drive(g_changer, 0, 1), MCHANGER_ERR_INVALID, "unload slot 0");
    PASS();
}

/* ------------------------------------------------------------------------- */
/* Read-only hardware qualification                                           */
/* ------------------------------------------------------------------------- */

TEST(open_without_optional_scsi_probes) {
    REQUIRE_HARDWARE();
    ASSERT_NOT_NULL(g_changer, "changer handle should be open");
    PASS();
}

TEST(registry_identity) {
    REQUIRE_HARDWARE();

    char vendor[64] = {0};
    char product[64] = {0};
    char revision[16] = {0};
    int rc = mchanger_get_registry_identity(g_changer, vendor, sizeof(vendor),
                                            product, sizeof(product),
                                            revision, sizeof(revision));
    ASSERT_EQ(rc, MCHANGER_OK, "IORegistry identity should be available");
    ASSERT(vendor[0] != '\0', "vendor should be populated");
    ASSERT(product[0] != '\0', "product should be populated");
    printf("\n    Device: %s %s (firmware %s)\n    ", vendor, product,
           revision[0] ? revision : "unknown");
    PASS();
}

TEST(element_map_integrity) {
    REQUIRE_HARDWARE();

    MChangerElementMap map = {0};
    ASSERT_EQ(mchanger_get_element_map(g_changer, &map), MCHANGER_OK,
              "READ ELEMENT STATUS should produce an element map");
    ASSERT(map.slot_count > 0, "element map should contain storage slots");
    ASSERT(map.drive_count > 0, "element map should contain a drive");
    ASSERT(map.transport_count > 0, "element map should contain a transport");
    ASSERT_NOT_NULL(map.slot_addrs, "slot address list should be allocated");
    ASSERT_NOT_NULL(map.drive_addrs, "drive address list should be allocated");
    ASSERT_NOT_NULL(map.transport_addrs, "transport address list should be allocated");

    if (g_expected_slots > 0) {
        ASSERT_EQ(map.slot_count, (size_t)g_expected_slots,
                  "reported slot count does not match --expect-slots");
    }
    ASSERT(!list_has_duplicates(map.slot_addrs, map.slot_count), "duplicate slot addresses");
    ASSERT(!list_has_duplicates(map.drive_addrs, map.drive_count), "duplicate drive addresses");
    ASSERT(!list_has_duplicates(map.transport_addrs, map.transport_count),
           "duplicate transport addresses");
    ASSERT(!list_has_duplicates(map.ie_addrs, map.ie_count), "duplicate I/E addresses");
    ASSERT(!lists_overlap(map.slot_addrs, map.slot_count, map.drive_addrs, map.drive_count),
           "slot and drive address ranges overlap");
    ASSERT(!lists_overlap(map.slot_addrs, map.slot_count,
                          map.transport_addrs, map.transport_count),
           "slot and transport address ranges overlap");
    ASSERT(!lists_overlap(map.slot_addrs, map.slot_count, map.ie_addrs, map.ie_count),
           "slot and I/E address ranges overlap");

    printf("\n    Elements: %zu slots, %zu drive(s), %zu transport(s), %zu I/E\n    ",
           map.slot_count, map.drive_count, map.transport_count, map.ie_count);
    mchanger_free_element_map(&map);
    ASSERT_NULL(map.slot_addrs, "free should clear slot addresses");
    ASSERT_EQ(map.slot_count, 0, "free should clear slot count");
    PASS();
}

TEST(bulk_inventory_is_repeatable) {
    REQUIRE_HARDWARE();

    MChangerElementMap map = {0};
    ASSERT_EQ(mchanger_get_element_map(g_changer, &map), MCHANGER_OK, "get element map");
    ASSERT(map.slot_count > 0 && map.slot_addrs != NULL,
           "bulk inventory requires at least one slot");
    ASSERT(map.drive_count > 0 && map.drive_addrs != NULL,
           "bulk inventory requires at least one drive");
    MChangerElementStatus *first = calloc(map.slot_count, sizeof(*first));
    MChangerElementStatus *second = calloc(map.slot_count, sizeof(*second));
    ASSERT_NOT_NULL(first, "allocate first inventory");
    ASSERT_NOT_NULL(second, "allocate second inventory");

    MChangerElementStatus first_drive = {0};
    MChangerElementStatus second_drive = {0};
    bool first_drive_supported = false;
    bool second_drive_supported = false;
    int first_rc = mchanger_get_bulk_status(
        g_changer, map.slot_addrs, map.slot_count, map.drive_addrs[0],
        &first_drive, first, &first_drive_supported);
    int second_rc = mchanger_get_bulk_status(
        g_changer, map.slot_addrs, map.slot_count, map.drive_addrs[0],
        &second_drive, second, &second_drive_supported);
    ASSERT_EQ(first_rc, MCHANGER_OK, "first bulk inventory should succeed");
    ASSERT_EQ(second_rc, MCHANGER_OK, "second bulk inventory should succeed");
    ASSERT(first_drive_supported, "bulk response should include the drive page");
    ASSERT(second_drive_supported, "repeated response should include the drive page");

    size_t occupied = 0;
    for (size_t i = 0; i < map.slot_count; i++) {
        ASSERT_EQ(first[i].address, map.slot_addrs[i], "first inventory address mismatch");
        ASSERT_EQ(second[i].address, map.slot_addrs[i], "second inventory address mismatch");
        ASSERT_EQ(first[i].full, second[i].full, "slot occupancy changed between reads");
        if (first[i].full) occupied++;
    }
    ASSERT_EQ(first_drive.full, second_drive.full, "drive occupancy changed between reads");
    printf("\n    Inventory: %zu occupied, %zu empty; drive is %s\n    ",
           occupied, map.slot_count - occupied, first_drive.full ? "full" : "empty");
    if (occupied > 0) {
        printf("Occupied slots:");
        for (size_t i = 0; i < map.slot_count; i++) {
            if (first[i].full) printf(" %zu", i + 1);
        }
        printf("\n    ");
    }

    free(first);
    free(second);
    mchanger_free_element_map(&map);
    PASS();
}

TEST(sampled_status_matches_bulk_inventory) {
    REQUIRE_HARDWARE();

    MChangerElementMap map = {0};
    ASSERT_EQ(mchanger_get_element_map(g_changer, &map), MCHANGER_OK, "get element map");
    ASSERT(map.slot_count > 0 && map.slot_addrs != NULL,
           "sampled inventory requires at least one slot");
    ASSERT(map.drive_count > 0 && map.drive_addrs != NULL,
           "sampled inventory requires at least one drive");
    MChangerElementStatus *slots = calloc(map.slot_count, sizeof(*slots));
    ASSERT_NOT_NULL(slots, "allocate inventory");
    MChangerElementStatus drive = {0};
    bool drive_supported = false;
    ASSERT_EQ(mchanger_get_bulk_status(
                  g_changer, map.slot_addrs, map.slot_count, map.drive_addrs[0],
                  &drive, slots, &drive_supported),
              MCHANGER_OK, "bulk inventory should succeed");

    size_t sample_indices[4] = {0, map.slot_count / 2, map.slot_count - 1, 0};
    for (size_t i = 0; i < map.slot_count; i++) {
        if (slots[i].full) {
            sample_indices[3] = i;
            break;
        }
    }
    for (size_t sample = 0; sample < 4; sample++) {
        size_t index = sample_indices[sample];
        MChangerElementStatus individual = {0};
        ASSERT_EQ(mchanger_get_slot_status(g_changer, (int)index + 1, &individual),
                  MCHANGER_OK, "individual slot status should succeed");
        ASSERT_EQ(individual.address, slots[index].address, "individual address mismatch");
        ASSERT_EQ(individual.full, slots[index].full, "individual occupancy mismatch");
    }
    MChangerElementStatus individual_drive = {0};
    ASSERT_EQ(mchanger_get_drive_status(g_changer, 1, &individual_drive),
              MCHANGER_OK, "individual drive status should succeed");
    ASSERT_EQ(individual_drive.address, drive.address, "individual drive address mismatch");
    ASSERT_EQ(individual_drive.full, drive.full, "individual drive occupancy mismatch");

    free(slots);
    mchanger_free_element_map(&map);
    PASS();
}

/* ------------------------------------------------------------------------- */
/* Explicit opt-in mechanical qualification                                  */
/* ------------------------------------------------------------------------- */

TEST(motion_round_trip) {
    if (g_motion_slot == 0) SKIP("not requested (use --motion-slot N)");
    REQUIRE_HARDWARE();

    MChangerElementStatus before_slot = {0};
    MChangerElementStatus before_drive = {0};
    ASSERT_EQ(mchanger_get_slot_status(g_changer, g_motion_slot, &before_slot),
              MCHANGER_OK, "could not read selected slot");
    ASSERT_EQ(mchanger_get_drive_status(g_changer, 1, &before_drive),
              MCHANGER_OK, "could not read drive 1");
    ASSERT(before_slot.full, "selected motion-test slot is empty");
    ASSERT(!before_drive.full, "drive 1 must be empty before the motion test");

    const char *failure = NULL;
    bool load_succeeded = false;
    int rc = mchanger_load_slot(g_changer, g_motion_slot, 1);
    if (rc != MCHANGER_OK) {
        failure = "load operation failed";
    } else {
        load_succeeded = true;
        MChangerElementStatus loaded_slot = {0};
        MChangerElementStatus loaded_drive = {0};
        if (!wait_for_slot_and_drive(g_motion_slot, false, true, 60,
                                     &loaded_slot, &loaded_drive)) {
            failure = "load completed but element state did not settle";
        } else if (!loaded_drive.valid_source ||
                   loaded_drive.source_addr != before_slot.address) {
            failure = "drive did not report the selected slot as its source";
        }
    }

    /* Always attempt to restore the disc after a successful load, including
       when an assertion above detects an unexpected post-load state. Optical
       release is intentionally explicit: mchanger_unload_drive owns robotics
       only and must never issue a second, hidden operating-system eject. */
    if (load_succeeded) {
        (void)mchanger_eject_from_macos();
        rc = mchanger_unload_drive(g_changer, g_motion_slot, 1);
        if (rc != MCHANGER_OK) {
            failure = "return-to-slot operation failed after load; manual recovery required";
        } else if (rc == MCHANGER_OK &&
                   !wait_for_slot_and_drive(g_motion_slot, true, false, 60, NULL, NULL)) {
            failure = "return command completed but original state was not restored; inspect changer";
        }
    }

    if (failure) FAIL(failure);
    PASS();
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--require-hardware") == 0) {
            g_require_hardware = true;
        } else if (strcmp(argv[i], "--expect-slots") == 0 && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], &g_expected_slots)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--motion-slot") == 0 && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], &g_motion_slot)) {
                print_usage(argv[0]);
                return 2;
            }
            g_require_hardware = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    printf("mchanger library qualification\n");
    printf("==============================\n\n");

    MChangerHandleInfo *list = NULL;
    size_t count = 0;
    int discovery_rc = mchanger_list_changers(&list, &count);
    if (discovery_rc == MCHANGER_OK && count > 0) {
        printf("Found %zu changer(s):\n", count);
        for (size_t i = 0; i < count; i++) {
            printf("  [%zu] %s %s\n      %s\n", i + 1, list[i].vendor,
                   list[i].product, list[i].path);
        }
        g_changer = mchanger_open_ex(NULL, true, true);
        g_has_hardware = (g_changer != NULL);
        if (!g_has_hardware) {
            printf("Changer discovery succeeded, but opening the device failed.\n");
        }
    } else if (discovery_rc != MCHANGER_OK) {
        printf("Changer discovery failed with library error %d.\n", discovery_rc);
    } else {
        printf("No changer hardware found.\n");
    }
    mchanger_free_changer_list(list);

    if (g_motion_slot > 0) {
        printf("Motion test requested for slot %d. The disc will be returned to that slot.\n",
               g_motion_slot);
    } else {
        printf("Mechanical movement is disabled.\n");
    }

    printf("\nSoftware/API tests:\n");
    RUN_TEST(list_changers_returns_valid);
    RUN_TEST(list_changers_null_params);
    RUN_TEST(null_safe_cleanup);
    RUN_TEST(open_null_safe);
    RUN_TEST(api_null_changer_returns_invalid);
    RUN_TEST(open_close_ie_cdb_layout);
    RUN_TEST(open_close_ie_is_never_retried);
    RUN_TEST(move_medium_cdb_layout);
    RUN_TEST(read_element_status_cdb_layout);
    RUN_TEST(paginated_bulk_status_parser);
    RUN_TEST(api_invalid_indices_return_invalid);

    printf("\nRead-only hardware qualification:\n");
    RUN_TEST(open_without_optional_scsi_probes);
    if (g_has_hardware && tests_passed != tests_run - tests_skipped) {
        printf("  Remaining hardware checks aborted after readiness failure.\n");
        goto hardware_done;
    }
    RUN_TEST(registry_identity);
    if (g_has_hardware && tests_passed != tests_run - tests_skipped) {
        printf("  Remaining hardware checks aborted after identity lookup failure.\n");
        goto hardware_done;
    }
    RUN_TEST(element_map_integrity);
    if (g_has_hardware && tests_passed != tests_run - tests_skipped) {
        printf("  Remaining hardware checks aborted after element-map failure.\n");
        goto hardware_done;
    }
    RUN_TEST(bulk_inventory_is_repeatable);
    if (g_has_hardware && tests_passed != tests_run - tests_skipped) {
        printf("  Remaining hardware checks aborted after inventory failure.\n");
        goto hardware_done;
    }
    RUN_TEST(sampled_status_matches_bulk_inventory);

hardware_done:
    printf("\nMechanical qualification:\n");
    RUN_TEST(motion_round_trip);

    if (g_changer) mchanger_close(g_changer);

    int failures = tests_run - tests_passed - tests_skipped;
    printf("\n==============================\n");
    printf("Tests: %d | Passed: %d | Failed: %d | Skipped: %d\n",
           tests_run, tests_passed, failures, tests_skipped);

    if (g_require_hardware && !g_has_hardware) {
        printf("Qualification failed: connected hardware was required.\n");
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
