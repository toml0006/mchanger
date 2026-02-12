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

/*
 * Bulk status
 *
 * Read element status once and fill the provided slot array (and optional drive status).
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
 * Operations
 */

/* Load a disc from slot into drive. Automatically unloads any disc currently in drive. */
int mchanger_load_slot(MChangerHandle *changer, int slot, int drive);

/* Load with verbose callback for mounted disc info */
int mchanger_load_slot_verbose(MChangerHandle *changer, int slot, int drive,
                           MChangerMountCallback callback, void *context);

/* Unload the drive to a specific slot */
int mchanger_unload_drive(MChangerHandle *changer, int slot, int drive);

/* Eject a disc to the import/export slot for physical removal */
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

#ifdef __cplusplus
}
#endif

#endif /* MCHANGER_H */
