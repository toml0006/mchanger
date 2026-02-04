/*
 * xl1b_changer - SCSI Media Changer Library
 *
 * A library to control SCSI media changer devices (jukeboxes/autoloaders) on macOS.
 *
 * MIT License - Copyright (c) 2026 Jackson
 */

#ifndef XL1B_CHANGER_H
#define XL1B_CHANGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a changer device */
typedef struct XL1BChanger XL1BChanger;

/* Information about a discovered changer */
typedef struct {
    char vendor[64];
    char product[64];
    char path[512];
} XL1BChangerInfo;

/* Status of a slot or drive element */
typedef struct {
    uint16_t address;       /* Element address */
    bool full;              /* Contains media */
    bool except;            /* Exception condition */
    bool valid_source;      /* Source address is valid */
    uint16_t source_addr;   /* Where the media came from */
} XL1BElementStatus;

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
} XL1BElementMap;

/* Callback for mounted disc info (used with verbose operations) */
typedef void (*XL1BMountCallback)(const char *name, const char *size, void *context);

/* Error codes */
#define XL1B_OK              0
#define XL1B_ERR_NOT_FOUND  -1
#define XL1B_ERR_OPEN       -2
#define XL1B_ERR_SCSI       -3
#define XL1B_ERR_INVALID    -4
#define XL1B_ERR_BUSY       -5
#define XL1B_ERR_EMPTY      -6

/*
 * Discovery
 */

/* List available changer devices. Caller must free with xl1b_free_changer_list(). */
int xl1b_list_changers(XL1BChangerInfo **out_list, size_t *out_count);

/* Free a changer list returned by xl1b_list_changers() */
void xl1b_free_changer_list(XL1BChangerInfo *list);

/*
 * Connection
 */

/* Open a changer device. Pass NULL for device_name to open the first found. */
XL1BChanger *xl1b_open(const char *device_name);

/* Open with additional options */
XL1BChanger *xl1b_open_ex(const char *device_name, bool force, bool skip_tur);

/* Close a changer handle */
void xl1b_close(XL1BChanger *changer);

/*
 * Element Map
 */

/* Get the element map (slots, drives, transports, I/E). Caller must free with xl1b_free_element_map(). */
int xl1b_get_element_map(XL1BChanger *changer, XL1BElementMap *out_map);

/* Free an element map */
void xl1b_free_element_map(XL1BElementMap *map);

/*
 * Status
 */

/* Get status of a specific slot (1-based index) */
int xl1b_get_slot_status(XL1BChanger *changer, int slot, XL1BElementStatus *out_status);

/* Get status of a specific drive (1-based index) */
int xl1b_get_drive_status(XL1BChanger *changer, int drive, XL1BElementStatus *out_status);

/*
 * Operations
 */

/* Load a disc from slot into drive. Automatically unloads any disc currently in drive. */
int xl1b_load_slot(XL1BChanger *changer, int slot, int drive);

/* Load with verbose callback for mounted disc info */
int xl1b_load_slot_verbose(XL1BChanger *changer, int slot, int drive,
                           XL1BMountCallback callback, void *context);

/* Unload the drive to a specific slot */
int xl1b_unload_drive(XL1BChanger *changer, int slot, int drive);

/* Eject a disc to the import/export slot for physical removal */
int xl1b_eject(XL1BChanger *changer, int slot, int drive);

/*
 * Low-level operations (for advanced use)
 */

/* Move medium between any two element addresses */
int xl1b_move_medium(XL1BChanger *changer, uint16_t transport, uint16_t source, uint16_t dest);

/* Eject optical media from macOS before physical move */
int xl1b_eject_from_macos(void);

/* Wait for disc to mount and get info */
int xl1b_wait_for_mount(char *out_name, size_t name_len, char *out_size, size_t size_len, int timeout_secs);

/*
 * Device info
 */

/* Send INQUIRY command and get device info */
int xl1b_inquiry(XL1BChanger *changer, char *vendor, size_t vendor_len,
                 char *product, size_t product_len, char *revision, size_t revision_len);

/* Send TEST UNIT READY */
int xl1b_test_unit_ready(XL1BChanger *changer);

#ifdef __cplusplus
}
#endif

#endif /* XL1B_CHANGER_H */
