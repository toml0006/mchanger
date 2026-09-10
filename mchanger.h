/*
 * mchanger - SCSI Media Changer Library
 *
 * A library to control SCSI media changer devices (jukeboxes/autoloaders) on macOS.
 *
 * MIT License - Copyright (c) 2026 Jackson
 */

#ifndef MCHANGER_H
#define MCHANGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a changer device */
typedef struct MChangerHandle MChangerHandle;

/* Information about a discovered changer */
typedef struct {
    char vendor[64];
    char product[64];
    char path[512];
} MChangerHandleInfo;

/* Status of a slot or drive element */
typedef struct {
    uint16_t address;       /* Element address */
    bool full;              /* Contains media */
    bool except;            /* Exception condition */
    bool valid_source;      /* Source address is valid */
    uint16_t source_addr;   /* Where the media came from */
} MChangerElementStatus;

/* Element map showing all slots, drives, etc. */
typedef struct {
    uint16_t *slot_addrs;
    size_t slot_count;
    uint16_t *drive_addrs;
    size_t drive_count;
    uint16_t *transport_addrs;
    size_t transport_count;
    uint16_t *ie_addrs;
    size_t ie_count;
} MChangerElementMap;

/* Callback for mounted disc info (used with verbose operations) */
typedef void (*MChangerMountCallback)(const char *name, const char *size, void *context);

/* Error codes */
#define MCHANGER_OK              0
#define MCHANGER_ERR_NOT_FOUND  -1
#define MCHANGER_ERR_OPEN       -2
#define MCHANGER_ERR_SCSI       -3
#define MCHANGER_ERR_INVALID    -4
#define MCHANGER_ERR_BUSY       -5
#define MCHANGER_ERR_EMPTY      -6
#define MCHANGER_ERR_TIMEOUT    -7

/* Classified reason for the most recent failed mchanger_open() call. */
#define MCHANGER_CONNECT_ERROR_NONE               0
#define MCHANGER_CONNECT_ERROR_NOT_FOUND          1
#define MCHANGER_CONNECT_ERROR_OWNED_ELSEWHERE    2
#define MCHANGER_CONNECT_ERROR_NOT_RESPONDING     3
#define MCHANGER_CONNECT_ERROR_OPEN_FAILED        4

/*
 * Discovery
 */

/* List available changer devices. Caller must free with mchanger_free_changer_list(). */
int mchanger_list_changers(MChangerHandleInfo **out_list, size_t *out_count);

/* Free a changer list returned by mchanger_list_changers() */
void mchanger_free_changer_list(MChangerHandleInfo *list);

/*
 * Connection
 */

/* Open a changer device. Pass NULL for device_name to open the first found. */
MChangerHandle *mchanger_open(const char *device_name);

/* Open with additional options */
MChangerHandle *mchanger_open_ex(const char *device_name, bool force, bool skip_tur);

/* Returns one of MCHANGER_CONNECT_ERROR_* after mchanger_open() returns NULL. */
int mchanger_last_connect_error(void);

/* True when the most recent command exhausted retries without a usable response. */
bool mchanger_last_command_was_not_responding(void);

/*
 * Return the device identity already published by IORegistry. Unlike
 * mchanger_inquiry(), this does not send a SCSI command to the changer.
 */
int mchanger_get_registry_identity(MChangerHandle *changer,
                                   char *vendor, size_t vendor_len,
                                   char *product, size_t product_len,
                                   char *revision, size_t revision_len);

/* Close a changer handle */
void mchanger_close(MChangerHandle *changer);

/*
 * Element Map
 */

/* Get the element map (slots, drives, transports, I/E). Caller must free with mchanger_free_element_map(). */
int mchanger_get_element_map(MChangerHandle *changer, MChangerElementMap *out_map);

/* Free an element map */
void mchanger_free_element_map(MChangerElementMap *map);

/*
 * Status
 */

/* Get status of a specific slot (1-based index) */
int mchanger_get_slot_status(MChangerHandle *changer, int slot, MChangerElementStatus *out_status);

/* Get status of a specific drive (1-based index) */
int mchanger_get_drive_status(MChangerHandle *changer, int drive, MChangerElementStatus *out_status);

/* Get status of a specific import/export element (1-based index). */
int mchanger_get_ie_status(MChangerHandle *changer, int ie, MChangerElementStatus *out_status);

/*
 * Bulk status
 *
 * Read element status, following paginated reports when required, and fill the
 * provided slot array (and optional drive status).
 *
 * - slot_addrs/slot_count should come from a previously fetched element map.
 * - drive_addr should be an element address from the map (pass 0 to skip drive status).
 * - out_slots must have at least slot_count entries.
 * - out_drive_supported, when non-NULL, is set to true iff a drive element status page was present.
 */
int mchanger_get_bulk_status(MChangerHandle *changer,
                             const uint16_t *slot_addrs,
                             size_t slot_count,
                             uint16_t drive_addr,
                             MChangerElementStatus *out_drive,
                             MChangerElementStatus *out_slots,
                             bool *out_drive_supported);

/*
 * Ask the changer to physically rescan its element inventory. This is much
 * slower than READ ELEMENT STATUS and is intended for recovery after a device
 * reset or when the reported drive/slot state is stale.
 */
int mchanger_initialize_element_status(MChangerHandle *changer);

/*
 * Ask the changer to open or close one of its operator-accessible
 * import/export elements (1-based index).
 *
 * This is the optional SMC-3 OPEN/CLOSE IMPORT/EXPORT ELEMENT command. Older
 * changers may reject it with MCHANGER_ERR_SCSI. The command is submitted
 * exactly once: it is never automatically replayed after a transport error.
 */
int mchanger_set_import_export_access(MChangerHandle *changer, int ie,
                                      bool open);

/*
 * Accept one disc through the first import/export element and store it in the
 * selected empty slot. On PowerFile/Sony changers, issuing this MOVE MEDIUM
 * while the I/E element is empty opens the front gate and waits for insertion.
 */
int mchanger_import_slot(MChangerHandle *changer, int slot);

/*
 * Present one disc from the selected storage slot at the first import/export
 * element for physical removal. The destination I/E element must be empty.
 */
int mchanger_export_slot(MChangerHandle *changer, int slot);

/*
 * Operations
 */

/*
 * Load a disc from slot into drive. If another disc is already in the drive,
 * it is returned robotically; the caller must release that optical device
 * from the operating system first.
 */
int mchanger_load_slot(MChangerHandle *changer, int slot, int drive);

/* Load with verbose callback for mounted disc info */
int mchanger_load_slot_verbose(MChangerHandle *changer, int slot, int drive,
                           MChangerMountCallback callback, void *context);

/*
 * Unload the drive to a specific slot using changer robotics only.
 *
 * The caller must first unmount and release the matching optical device from
 * the operating system. This function never discovers or ejects media on its
 * own, so it cannot accidentally target another drive or duplicate a release.
 */
int mchanger_unload_drive(MChangerHandle *changer, int slot, int drive);

/*
 * Eject a disc to the import/export slot using changer robotics only. If the
 * disc is currently in the drive, the caller must release its optical device
 * from the operating system first.
 */
int mchanger_eject(MChangerHandle *changer, int slot, int drive);

/*
 * Low-level operations (for advanced use)
 */

/* Move medium between any two element addresses */
int mchanger_move_medium(MChangerHandle *changer, uint16_t transport, uint16_t source, uint16_t dest);

/* Eject optical media from macOS before physical move */
int mchanger_eject_from_macos(void);

/* Wait for disc to mount and get info */
int mchanger_wait_for_mount(char *out_name, size_t name_len, char *out_size, size_t size_len, int timeout_secs);

/*
 * Device info
 */

/* Send INQUIRY command and get device info */
int mchanger_inquiry(MChangerHandle *changer, char *vendor, size_t vendor_len,
                 char *product, size_t product_len, char *revision, size_t revision_len);

/* Send TEST UNIT READY */
int mchanger_test_unit_ready(MChangerHandle *changer);

#ifdef MCHANGER_TESTING
/* Pure command helpers exposed only to the software test build. */
int mchanger_test_build_open_close_ie_cdb(uint16_t element_address,
                                          bool open,
                                          uint8_t out_cdb[6]);
int mchanger_test_build_move_medium_cdb(uint16_t transport,
                                        uint16_t source,
                                        uint16_t dest,
                                        uint8_t out_cdb[12]);
int mchanger_test_build_read_element_status_cdb(uint8_t element_type,
                                                uint16_t start,
                                                uint16_t count,
                                                uint32_t allocation_length,
                                                uint8_t out_cdb[12]);
int mchanger_test_parse_bulk_status_report(
    const uint8_t *buffer,
    size_t buffer_length,
    uint16_t request_start,
    const uint16_t *slot_addrs,
    size_t slot_count,
    uint16_t drive_addr,
    MChangerElementStatus *out_drive,
    MChangerElementStatus *out_slots,
    bool *io_drive_supported,
    uint16_t *out_next_start);
bool mchanger_test_cdb_is_retryable(const uint8_t *cdb, uint8_t cdb_len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MCHANGER_H */
