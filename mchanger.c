/*
 * mchanger - SCSI Media Changer Library and CLI
 *
 * A library/tool to control SCSI media changer devices on macOS.
 *
 * MIT License - Copyright (c) 2026 Jackson
 */

#include "mchanger.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOTypes.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSITask.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>
#include <IOKit/sbp2/IOFireWireSBP2Lib.h>
#include <DiskArbitration/DiskArbitration.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VENDOR_KEY CFSTR("Vendor Identification")
#define PRODUCT_KEY CFSTR("Product Identification")

typedef enum {
    BACKEND_SCSITASK = 0,
    BACKEND_SBP2 = 1
} BackendType;

typedef struct {
    BackendType backend;
    io_service_t service;
    SCSITaskDeviceInterface **scsi_device;
    bool has_exclusive;
    IOFireWireSBP2LibLUNInterface **sbp2_lun;
    IOFireWireSBP2LibLoginInterface **sbp2_login;
} ChangerHandle;

typedef struct {
    uint16_t *addrs;
    size_t count;
    size_t cap;
} ElementList;

typedef struct {
    ElementList transports;
    ElementList slots;
    ElementList drives;
    ElementList ie;
} ElementMap;

typedef struct {
    uint16_t first_transport;
    uint16_t num_transport;
    uint16_t first_storage;
    uint16_t num_storage;
    uint16_t first_ie;
    uint16_t num_ie;
    uint16_t first_drive;
    uint16_t num_drive;
} ElementAddrAssignment;

static ChangerHandle open_changer_scsitask(io_service_t service, const char *vendor_c, const char *product_c);
static ChangerHandle open_sbp2_lun_from_service(io_service_t service);
static void close_changer(ChangerHandle *handle);
static void element_map_free(ElementMap *map);
static int fetch_element_map(ChangerHandle *handle, ElementMap *map);
static int cmd_mode_sense_element(ChangerHandle *handle);
static int cmd_probe_storage(ChangerHandle *handle);
static void parse_element_status(const uint8_t *buf, uint32_t len);
static int cmd_inquiry_vpd(ChangerHandle *handle, uint8_t page);
static int cmd_report_luns(ChangerHandle *handle);
static int cmd_log_sense(ChangerHandle *handle, uint8_t page);

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s list\n"
        "  %s list-all\n"
        "  %s scan-changers\n"
        "  %s list-sbp2\n"
        "  %s scan-sbp2\n"
        "  %s test-unit-ready\n"
        "  %s inquiry\n"
        "  %s inquiry-vpd --page <hex>\n"
        "  %s report-luns\n"
        "  %s log-sense --page <hex>\n"
        "  %s mode-sense-element\n"
        "  %s probe-storage\n"
        "  %s init-status\n"
        "  %s read-element-status --element-type <all|transport|storage|ie|drive>\n"
        "                           --start <addr> --count <n> --alloc <bytes> [--raw]\n"
        "  %s list-map\n"
        "  %s sanity-check\n"
        "  %s insert --slot <n> [--transport <addr>]     (IE port -> slot)\n"
        "  %s retrieve --slot <n> [--transport <addr>]   (slot -> IE port)\n"
        "  %s load --slot <n> [--drive <n>] [--transport <addr>]   (slot -> drive)\n"
        "  %s unload --slot <n> [--drive <n>] [--transport <addr>] (drive -> slot)\n"
        "  %s eject --slot <n> [--drive <n>] [--transport <addr>]  (load, eject, unload)\n"
        "  %s move --transport <addr> --source <addr> --dest <addr> (low-level)\n"
        "\n"
        "Notes:\n"
        "- Addresses are element addresses from READ ELEMENT STATUS.\n"
        "- Use --force to bypass device ID and TUR checks.\n"
        "- Use --no-tur to skip the automatic TEST UNIT READY check.\n"
        "- Use --dry-run to show resolved element addresses without moving media.\n"
        "- Use --confirm to require interactive confirmation before moving media.\n"
        "- Use --debug to print IORegistry details for troubleshooting.\n"
        "- Use --verbose or -v to show mounted disc info during load/unload.\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0
    );
}

static bool g_debug = false;
static bool g_verbose = false;

static bool cfstring_equals(CFTypeRef value, const char *expected) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        return false;
    }
    char buffer[256];
    if (!CFStringGetCString((CFStringRef)value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return false;
    }
    return strcmp(buffer, expected) == 0;
}

static void cfstring_to_c(CFTypeRef value, char *out, size_t out_len) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        snprintf(out, out_len, "unknown");
        return;
    }
    if (!CFStringGetCString((CFStringRef)value, out, out_len, kCFStringEncodingUTF8)) {
        snprintf(out, out_len, "unknown");
    }
}

static void get_vendor_product(io_service_t service,
                               char *vendor_c, size_t vendor_len,
                               char *product_c, size_t product_len) {
    CFTypeRef vendor = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
    CFTypeRef product = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);
    CFTypeRef parent_vendor = NULL;
    CFTypeRef parent_product = NULL;

    bool have_vendor = (vendor && CFGetTypeID(vendor) == CFStringGetTypeID());
    bool have_product = (product && CFGetTypeID(product) == CFStringGetTypeID());
    if (!have_vendor || !have_product) {
        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(service, kIOServicePlane, &parent) == KERN_SUCCESS) {
            parent_vendor = IORegistryEntryCreateCFProperty(parent, VENDOR_KEY, kCFAllocatorDefault, 0);
            parent_product = IORegistryEntryCreateCFProperty(parent, PRODUCT_KEY, kCFAllocatorDefault, 0);
            IOObjectRelease(parent);
        }
    }

    CFTypeRef use_vendor = have_vendor ? vendor : parent_vendor;
    CFTypeRef use_product = have_product ? product : parent_product;
    cfstring_to_c(use_vendor, vendor_c, vendor_len);
    cfstring_to_c(use_product, product_c, product_len);

    if (vendor) CFRelease(vendor);
    if (product) CFRelease(product);
    if (parent_vendor) CFRelease(parent_vendor);
    if (parent_product) CFRelease(parent_product);
}

static bool is_changer_device(io_service_t service) {
    CFTypeRef type = IORegistryEntryCreateCFProperty(service, CFSTR("Peripheral Device Type"), kCFAllocatorDefault, 0);
    bool is_type8 = false;
    if (type && CFGetTypeID(type) == CFNumberGetTypeID()) {
        int value = 0;
        CFNumberGetValue((CFNumberRef)type, kCFNumberIntType, &value);
        is_type8 = (value == 8);
    }
    if (type) CFRelease(type);
    return is_type8;
}

static io_service_t find_scsi_task_device(io_service_t changer_nub) {
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(changer_nub, kIOServicePlane, &iter) != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }
    io_service_t match = IO_OBJECT_NULL;
    io_registry_entry_t child;
    while ((child = IOIteratorNext(iter))) {
        CFTypeRef category = IORegistryEntryCreateCFProperty(child, CFSTR("SCSITaskDeviceCategory"), kCFAllocatorDefault, 0);
        if (category) {
            CFRelease(category);
            match = child;
            break;
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(iter);
    return match;
}

static io_service_t find_scsi_task_device_global(const char *vendor, const char *product) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("IOSCSIPeripheralDeviceNub");
    if (!match) {
        fprintf(stderr, "Failed to create IOSCSIPeripheralDeviceNub match dictionary.\n");
        return IO_OBJECT_NULL;
    }
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%x\n", kr);
        return IO_OBJECT_NULL;
    }

    io_service_t fallback = IO_OBJECT_NULL;
    io_service_t service;
    CFStringRef scsi_uuid_str = CFUUIDCreateString(kCFAllocatorDefault, kIOSCSITaskDeviceUserClientTypeID);
    while ((service = IOIteratorNext(iter))) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
        CFTypeRef p = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);
        CFTypeRef cat = IORegistryEntryCreateCFProperty(service, CFSTR("SCSITaskDeviceCategory"), kCFAllocatorDefault, 0);
        CFTypeRef plugins = IORegistryEntryCreateCFProperty(service, CFSTR("IOCFPlugInTypes"), kCFAllocatorDefault, 0);
        bool v_ok = cfstring_equals(v, vendor);
        bool p_ok = cfstring_equals(p, product);
        bool cat_ok = cfstring_equals(cat, "SCSITaskUserClientDevice");
        bool plugin_ok = false;
        if (plugins && CFGetTypeID(plugins) == CFDictionaryGetTypeID() && scsi_uuid_str) {
            plugin_ok = CFDictionaryContainsKey((CFDictionaryRef)plugins, scsi_uuid_str);
        }
        if (v) CFRelease(v);
        if (p) CFRelease(p);
        if (cat) CFRelease(cat);
        if (plugins) CFRelease(plugins);

        if (v_ok && p_ok && cat_ok && plugin_ok) {
            if (scsi_uuid_str) CFRelease(scsi_uuid_str);
            IOObjectRelease(iter);
            return service;
        }
        if (!cat_ok || !plugin_ok) {
            IOObjectRelease(service);
            continue;
        }
        if (fallback == IO_OBJECT_NULL) {
            fallback = service;
        } else {
            IOObjectRelease(service);
        }
    }
    if (scsi_uuid_str) CFRelease(scsi_uuid_str);
    IOObjectRelease(iter);
    return fallback;
}

static io_iterator_t match_scsi_devices(void) {
    CFMutableDictionaryRef match = IOServiceMatching("IOSCSIPeripheralDeviceNub");
    if (!match) {
        fprintf(stderr, "Failed to create IOService match dictionary.\n");
        return IO_OBJECT_NULL;
    }
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%x\n", kr);
        return IO_OBJECT_NULL;
    }
    return iter;
}

static void list_changers(void) {
    io_iterator_t iter = IO_OBJECT_NULL;
    iter = match_scsi_devices();
    if (iter == IO_OBJECT_NULL) return;

    io_service_t service;
    int count = 0;
    while ((service = IOIteratorNext(iter))) {
        if (!is_changer_device(service)) {
            IOObjectRelease(service);
            continue;
        }
        CFTypeRef vendor = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
        CFTypeRef product = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);
        char vendor_c[128];
        char product_c[128];
        cfstring_to_c(vendor, vendor_c, sizeof(vendor_c));
        cfstring_to_c(product, product_c, sizeof(product_c));

        char path[512];
        path[0] = '\0';
        IORegistryEntryGetPath(service, kIOServicePlane, path);

        printf("Changer %d:\n", ++count);
        printf("  Vendor:  %s\n", vendor_c);
        printf("  Product: %s\n", product_c);
        printf("  Path:    %s\n", path[0] ? path : "(unknown)");

        if (vendor) CFRelease(vendor);
        if (product) CFRelease(product);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);

    if (count == 0) {
        printf("No SCSI changer devices (device type 8) found.\n");
    }
}

static void list_all_scsi_devices(void) {
    io_iterator_t iter = match_scsi_devices();
    if (iter == IO_OBJECT_NULL) return;

    io_service_t service;
    int count = 0;
    while ((service = IOIteratorNext(iter))) {
        CFTypeRef vendor = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
        CFTypeRef product = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);
        char vendor_c[128];
        char product_c[128];
        cfstring_to_c(vendor, vendor_c, sizeof(vendor_c));
        cfstring_to_c(product, product_c, sizeof(product_c));

        char path[512];
        path[0] = '\0';
        IORegistryEntryGetPath(service, kIOServicePlane, path);

        printf("SCSI Device %d:\n", ++count);
        printf("  Vendor:  %s\n", vendor_c);
        printf("  Product: %s\n", product_c);
        printf("  Type8:   %s\n", is_changer_device(service) ? "yes" : "no");
        printf("  Path:    %s\n", path[0] ? path : "(unknown)");

        if (vendor) CFRelease(vendor);
        if (product) CFRelease(product);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);

    if (count == 0) {
        printf("No SCSI peripheral devices found.\n");
    }
}

static uint64_t get_cfnumber_u64(CFTypeRef value, bool *ok_out) {
    if (ok_out) *ok_out = false;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return 0;
    uint64_t out = 0;
    if (CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &out)) {
        if (ok_out) *ok_out = true;
        return out;
    }
    return 0;
}

static void list_sbp2_luns(void) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("IOFireWireSBP2LUN");
    if (!match) {
        fprintf(stderr, "Failed to create IOFireWireSBP2LUN match dictionary.\n");
        return;
    }
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%x\n", kr);
        return;
    }

    io_service_t service;
    int count = 0;
    while ((service = IOIteratorNext(iter))) {
        CFTypeRef lun_prop = IORegistryEntryCreateCFProperty(service, CFSTR("LUN"), kCFAllocatorDefault, 0);
        CFTypeRef sbp2_lun_prop = IORegistryEntryCreateCFProperty(service, CFSTR("SBP2LUN"), kCFAllocatorDefault, 0);
        char vendor_c[128];
        char product_c[128];
        get_vendor_product(service, vendor_c, sizeof(vendor_c), product_c, sizeof(product_c));

        char path[512];
        path[0] = '\0';
        IORegistryEntryGetPath(service, kIOServicePlane, path);

        uint64_t entry_id = 0;
        IORegistryEntryGetRegistryEntryID(service, &entry_id);

        printf("SBP2 LUN %d:\n", ++count);
        printf("  Vendor:  %s\n", vendor_c);
        printf("  Product: %s\n", product_c);
        printf("  EntryID: 0x%llx\n", (unsigned long long)entry_id);
        bool lun_ok = false;
        uint64_t lun = get_cfnumber_u64(lun_prop, &lun_ok);
        bool sbp2_ok = false;
        uint64_t sbp2_lun = get_cfnumber_u64(sbp2_lun_prop, &sbp2_ok);
        if (lun_ok) {
            printf("  LUN:     %llu\n", (unsigned long long)lun);
        }
        if (sbp2_ok) {
            printf("  SBP2LUN: %llu\n", (unsigned long long)sbp2_lun);
        }
        printf("  Path:    %s\n", path[0] ? path : "(unknown)");

        if (lun_prop) CFRelease(lun_prop);
        if (sbp2_lun_prop) CFRelease(sbp2_lun_prop);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);

    if (count == 0) {
        printf("No SBP2 LUN services found.\n");
    }
}

static void scan_sbp2_luns(void) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("IOFireWireSBP2LUN");
    if (!match) {
        fprintf(stderr, "Failed to create IOFireWireSBP2LUN match dictionary.\n");
        return;
    }
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%x\n", kr);
        return;
    }

    io_service_t service;
    int count = 0;
    while ((service = IOIteratorNext(iter))) {
        char vendor_c[128];
        char product_c[128];
        get_vendor_product(service, vendor_c, sizeof(vendor_c), product_c, sizeof(product_c));

        char path[512];
        path[0] = '\0';
        IORegistryEntryGetPath(service, kIOServicePlane, path);

        uint64_t entry_id = 0;
        IORegistryEntryGetRegistryEntryID(service, &entry_id);

        printf("SBP2 LUN %d:\n", ++count);
        printf("  Vendor:  %s\n", vendor_c);
        printf("  Product: %s\n", product_c);
        printf("  EntryID: 0x%llx\n", (unsigned long long)entry_id);
        printf("  Path:    %s\n", path[0] ? path : "(unknown)");

        ChangerHandle handle = open_sbp2_lun_from_service(service);
        if (handle.sbp2_login) {
            ElementMap map = {0};
            int rc = fetch_element_map(&handle, &map);
            if (rc == 0) {
                printf("  Elements: transports=%zu slots=%zu drives=%zu ie=%zu\n",
                       map.transports.count, map.slots.count, map.drives.count, map.ie.count);
            } else {
                printf("  Elements: failed to read element map\n");
            }
            element_map_free(&map);
            close_changer(&handle);
        } else {
            printf("  Elements: unable to open SBP2 login\n");
            IOObjectRelease(service);
        }

    }
    IOObjectRelease(iter);

    if (count == 0) {
        printf("No SBP2 LUN services found.\n");
    }
}

static void scan_changers(void) {
    io_iterator_t iter = match_scsi_devices();
    if (iter == IO_OBJECT_NULL) return;

    io_service_t service;
    int count = 0;
    while ((service = IOIteratorNext(iter))) {
        if (!is_changer_device(service)) {
            IOObjectRelease(service);
            continue;
        }
        CFTypeRef vendor = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
        CFTypeRef product = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);
        char vendor_c[128];
        char product_c[128];
        cfstring_to_c(vendor, vendor_c, sizeof(vendor_c));
        cfstring_to_c(product, product_c, sizeof(product_c));

        char path[512];
        path[0] = '\0';
        IORegistryEntryGetPath(service, kIOServicePlane, path);

        printf("Changer %d:\n", ++count);
        printf("  Vendor:  %s\n", vendor_c);
        printf("  Product: %s\n", product_c);
        printf("  Path:    %s\n", path[0] ? path : "(unknown)");

        ChangerHandle handle = open_changer_scsitask(service, vendor_c, product_c);
        if (handle.scsi_device) {
            ElementMap map = {0};
            int rc = fetch_element_map(&handle, &map);
            if (rc == 0) {
                printf("  Elements: transports=%zu slots=%zu drives=%zu ie=%zu\n",
                       map.transports.count, map.slots.count, map.drives.count, map.ie.count);
            } else {
                printf("  Elements: failed to read element map\n");
            }
            element_map_free(&map);
            close_changer(&handle);
        } else {
            printf("  Elements: unable to open SCSITask user client\n");
            if (handle.service) {
                IOObjectRelease(handle.service);
            }
        }

        if (vendor) CFRelease(vendor);
        if (product) CFRelease(product);
    }
    IOObjectRelease(iter);

    if (count == 0) {
        printf("No SCSI changer devices (device type 8) found.\n");
    }
}

static io_service_t find_changer_service(bool require_sony) {
    io_iterator_t iter = match_scsi_devices();
    if (iter == IO_OBJECT_NULL) {
        return IO_OBJECT_NULL;
    }

    io_service_t fallback = IO_OBJECT_NULL;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        if (!is_changer_device(service)) {
            IOObjectRelease(service);
            continue;
        }
        CFTypeRef vendor = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
        CFTypeRef product = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);

        bool is_sony = cfstring_equals(vendor, "Sony");
        bool is_vgp = cfstring_equals(product, "VAIOChanger1");

        if (vendor) CFRelease(vendor);
        if (product) CFRelease(product);

        if (is_sony && is_vgp) {
            IOObjectRelease(iter);
            return service;
        }
        if (!require_sony && fallback == IO_OBJECT_NULL) {
            fallback = service;
        } else {
            IOObjectRelease(service);
        }
    }

    IOObjectRelease(iter);
    return fallback;
}

static io_service_t find_sbp2_lun_service(const char *vendor, const char *product) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("IOFireWireSBP2LUN");
    if (!match) {
        fprintf(stderr, "Failed to create IOFireWireSBP2LUN match dictionary.\n");
        return IO_OBJECT_NULL;
    }
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%x\n", kr);
        return IO_OBJECT_NULL;
    }

    io_service_t fallback = IO_OBJECT_NULL;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(service, VENDOR_KEY, kCFAllocatorDefault, 0);
        CFTypeRef p = IORegistryEntryCreateCFProperty(service, PRODUCT_KEY, kCFAllocatorDefault, 0);
        bool v_ok = cfstring_equals(v, vendor);
        bool p_ok = cfstring_equals(p, product);
        if (v) CFRelease(v);
        if (p) CFRelease(p);

        if (v_ok && p_ok) {
            IOObjectRelease(iter);
            return service;
        }
        if (fallback == IO_OBJECT_NULL) {
            fallback = service;
        } else {
            IOObjectRelease(service);
        }
    }
    IOObjectRelease(iter);
    return fallback;
}

static ChangerHandle open_changer_scsitask(io_service_t service, const char *vendor_c, const char *product_c) {
    ChangerHandle handle = {0};
    handle.backend = BACKEND_SCSITASK;
    handle.service = service;

    io_service_t task_service = find_scsi_task_device(handle.service);
    if (task_service == IO_OBJECT_NULL) {
        task_service = find_scsi_task_device_global(vendor_c, product_c);
    }
    if (task_service == IO_OBJECT_NULL) {
        fprintf(stderr, "Failed to locate SCSITask device for changer.\n");
        IOObjectRelease(handle.service);
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    if (g_debug) {
        char path[512] = {0};
        IORegistryEntryGetPath(task_service, kIOServicePlane, path);
        printf("Task service path: %s\n", path[0] ? path : "(unknown)");

        CFTypeRef plugins = IORegistryEntryCreateCFProperty(task_service, CFSTR("IOCFPlugInTypes"), kCFAllocatorDefault, 0);
        if (plugins && CFGetTypeID(plugins) == CFDictionaryGetTypeID()) {
            printf("IOCFPlugInTypes keys:\n");
            CFDictionaryRef dict = (CFDictionaryRef)plugins;
            CFIndex count = CFDictionaryGetCount(dict);
            const void **keys = calloc((size_t)count, sizeof(void *));
            if (keys) {
                CFDictionaryGetKeysAndValues(dict, keys, NULL);
                for (CFIndex i = 0; i < count; i++) {
                    CFTypeRef key = keys[i];
                    if (key && CFGetTypeID(key) == CFStringGetTypeID()) {
                        char kbuf[128];
                        if (CFStringGetCString((CFStringRef)key, kbuf, sizeof(kbuf), kCFStringEncodingUTF8)) {
                            printf("  %s\n", kbuf);
                        }
                    }
                }
                free(keys);
            }
        }
        if (plugins) CFRelease(plugins);
    }

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        task_service,
        kIOSCSITaskDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID,
        &plugin,
        &score
    );
    IOObjectRelease(task_service);
    if (kr != KERN_SUCCESS || !plugin) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService failed: 0x%x\n", kr);
        IOObjectRelease(handle.service);
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    HRESULT result = (*plugin)->QueryInterface(
        plugin,
        CFUUIDGetUUIDBytes(kIOSCSITaskDeviceInterfaceID),
        (LPVOID *)&handle.scsi_device
    );
    (*plugin)->Release(plugin);

    if (result || !handle.scsi_device) {
        fprintf(stderr, "QueryInterface for SCSITaskDeviceInterface failed.\n");
        IOObjectRelease(handle.service);
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    kern_return_t ex = (*handle.scsi_device)->ObtainExclusiveAccess(handle.scsi_device);
    if (ex == kIOReturnSuccess) {
        handle.has_exclusive = true;
    } else {
        fprintf(stderr, "Warning: Could not obtain exclusive access (0x%x). Proceeding.\n", ex);
    }

    return handle;
}

typedef struct {
    bool done;
    IOReturn status;
    FWSBP2LoginCompleteParams login_params;
} SBP2LoginWait;

static void sbp2_login_callback(void *refCon, FWSBP2LoginCompleteParams *params) {
    SBP2LoginWait *waiter = (SBP2LoginWait *)refCon;
    if (!waiter) return;
    waiter->status = params->status;
    waiter->done = true;
}

typedef struct {
    bool done;
    UInt32 notificationEvent;
    const void *message;
    UInt32 length;
} SBP2StatusWait;

static void sbp2_status_callback(void *refCon, FWSBP2NotifyParams *params) {
    SBP2StatusWait *waiter = (SBP2StatusWait *)refCon;
    if (!waiter) return;
    waiter->notificationEvent = params->notificationEvent;
    waiter->message = params->message;
    waiter->length = params->length;
    waiter->done = true;
}

static bool runloop_wait(bool *done_flag, double timeout_seconds) {
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + timeout_seconds;
    while (!*done_flag) {
        CFTimeInterval remaining = end - CFAbsoluteTimeGetCurrent();
        if (remaining <= 0) {
            return false;
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, remaining > 0.1 ? 0.1 : remaining, true);
    }
    return true;
}

static ChangerHandle open_sbp2_lun_from_service(io_service_t service) {
    ChangerHandle handle = {0};
    handle.backend = BACKEND_SBP2;
    handle.service = service;
    if (handle.service == IO_OBJECT_NULL) {
        fprintf(stderr, "No SBP2 LUN service provided.\n");
        return handle;
    }

    if (g_debug) {
        char path[512] = {0};
        IORegistryEntryGetPath(handle.service, kIOServicePlane, path);
        printf("SBP2 LUN path: %s\n", path[0] ? path : "(unknown)");
        CFTypeRef plugins = IORegistryEntryCreateCFProperty(handle.service, CFSTR("IOCFPlugInTypes"), kCFAllocatorDefault, 0);
        if (plugins && CFGetTypeID(plugins) == CFDictionaryGetTypeID()) {
            printf("SBP2 IOCFPlugInTypes keys:\n");
            CFDictionaryRef dict = (CFDictionaryRef)plugins;
            CFIndex count = CFDictionaryGetCount(dict);
            const void **keys = calloc((size_t)count, sizeof(void *));
            if (keys) {
                CFDictionaryGetKeysAndValues(dict, keys, NULL);
                for (CFIndex i = 0; i < count; i++) {
                    CFTypeRef key = keys[i];
                    if (key && CFGetTypeID(key) == CFStringGetTypeID()) {
                        char kbuf[128];
                        if (CFStringGetCString((CFStringRef)key, kbuf, sizeof(kbuf), kCFStringEncodingUTF8)) {
                            printf("  %s\n", kbuf);
                        }
                    }
                }
                free(keys);
            }
        }
        if (plugins) CFRelease(plugins);
    }

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        handle.service,
        kIOFireWireSBP2LibTypeID,
        kIOCFPlugInInterfaceID,
        &plugin,
        &score
    );
    if (kr != KERN_SUCCESS || !plugin) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService(SBP2) failed: 0x%x\n", kr);
        IOObjectRelease(handle.service);
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    HRESULT result = (*plugin)->QueryInterface(
        plugin,
        CFUUIDGetUUIDBytes(kIOFireWireSBP2LibLUNInterfaceID),
        (LPVOID *)&handle.sbp2_lun
    );
    (*plugin)->Release(plugin);
    if (result || !handle.sbp2_lun) {
        fprintf(stderr, "QueryInterface for SBP2 LUN failed.\n");
        IOObjectRelease(handle.service);
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    kr = (*handle.sbp2_lun)->open(handle.sbp2_lun);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "SBP2 LUN open failed: 0x%x\n", kr);
        (*handle.sbp2_lun)->Release(handle.sbp2_lun);
        IOObjectRelease(handle.service);
        handle.sbp2_lun = NULL;
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    (*handle.sbp2_lun)->addCallbackDispatcherToRunLoop(handle.sbp2_lun, CFRunLoopGetCurrent());

    IUnknownVTbl **login_unknown = (*handle.sbp2_lun)->createLogin(
        handle.sbp2_lun,
        CFUUIDGetUUIDBytes(kIOFireWireSBP2LibLoginInterfaceID)
    );
    if (!login_unknown) {
        fprintf(stderr, "Failed to create SBP2 login.\n");
        (*handle.sbp2_lun)->close(handle.sbp2_lun);
        (*handle.sbp2_lun)->Release(handle.sbp2_lun);
        IOObjectRelease(handle.service);
        handle.sbp2_lun = NULL;
        handle.service = IO_OBJECT_NULL;
        return handle;
    }
    handle.sbp2_login = (IOFireWireSBP2LibLoginInterface **)login_unknown;
    (*handle.sbp2_login)->setLoginFlags(handle.sbp2_login, kFWSBP2ExclusiveLogin);

    SBP2LoginWait waiter = {0};
    (*handle.sbp2_login)->setLoginCallback(handle.sbp2_login, &waiter, sbp2_login_callback);
    kr = (*handle.sbp2_login)->submitLogin(handle.sbp2_login);
    if (kr != kIOReturnSuccess || !runloop_wait(&waiter.done, 5.0) || waiter.status != kIOReturnSuccess) {
        fprintf(stderr, "SBP2 login failed: 0x%x\n", kr);
        (*handle.sbp2_login)->Release(handle.sbp2_login);
        (*handle.sbp2_lun)->close(handle.sbp2_lun);
        (*handle.sbp2_lun)->Release(handle.sbp2_lun);
        IOObjectRelease(handle.service);
        handle.sbp2_login = NULL;
        handle.sbp2_lun = NULL;
        handle.service = IO_OBJECT_NULL;
        return handle;
    }

    return handle;
}

static ChangerHandle open_changer_sbp2(const char *vendor_c, const char *product_c) {
    io_service_t service = find_sbp2_lun_service(vendor_c, product_c);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "No SBP2 LUN service found for %s %s.\n", vendor_c, product_c);
        ChangerHandle empty = {0};
        return empty;
    }
    return open_sbp2_lun_from_service(service);
}

static ChangerHandle open_changer(bool require_sony) {
    ChangerHandle handle = {0};
    handle.service = find_changer_service(require_sony);
    if (handle.service == IO_OBJECT_NULL) {
        fprintf(stderr, "No changer device found.\n");
        return handle;
    }

    CFTypeRef vendor = IORegistryEntryCreateCFProperty(handle.service, VENDOR_KEY, kCFAllocatorDefault, 0);
    CFTypeRef product = IORegistryEntryCreateCFProperty(handle.service, PRODUCT_KEY, kCFAllocatorDefault, 0);
    char vendor_c[128];
    char product_c[128];
    cfstring_to_c(vendor, vendor_c, sizeof(vendor_c));
    cfstring_to_c(product, product_c, sizeof(product_c));
    if (vendor) CFRelease(vendor);
    if (product) CFRelease(product);

    printf("Using changer device: %s %s\n", vendor_c, product_c);
    if (require_sony) {
        if (!(strcmp(vendor_c, "Sony") == 0 && strcmp(product_c, "VAIOChanger1") == 0)) {
            fprintf(stderr, "Device ID mismatch. Use --force to override.\n");
            IOObjectRelease(handle.service);
            handle.service = IO_OBJECT_NULL;
            return handle;
        }
    }

    ChangerHandle scsi = open_changer_scsitask(handle.service, vendor_c, product_c);
    if (scsi.scsi_device) {
        return scsi;
    }

    IOObjectRelease(handle.service);
    handle.service = IO_OBJECT_NULL;
    return open_changer_sbp2(vendor_c, product_c);
}

static void close_changer(ChangerHandle *handle) {
    if (!handle) return;
    if (handle->backend == BACKEND_SCSITASK && handle->scsi_device) {
        if (handle->has_exclusive) {
            (*handle->scsi_device)->ReleaseExclusiveAccess(handle->scsi_device);
        }
        (*handle->scsi_device)->Release(handle->scsi_device);
        handle->scsi_device = NULL;
    }
    if (handle->backend == BACKEND_SBP2 && handle->sbp2_login && handle->sbp2_lun) {
        (*handle->sbp2_login)->submitLogout(handle->sbp2_login);
        (*handle->sbp2_login)->Release(handle->sbp2_login);
        handle->sbp2_login = NULL;
        (*handle->sbp2_lun)->removeCallbackDispatcherFromRunLoop(handle->sbp2_lun);
        (*handle->sbp2_lun)->close(handle->sbp2_lun);
        (*handle->sbp2_lun)->Release(handle->sbp2_lun);
        handle->sbp2_lun = NULL;
    }
    if (handle->service) {
        IOObjectRelease(handle->service);
        handle->service = IO_OBJECT_NULL;
    }
}

static void dump_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            printf("\n%04zx: ", i);
        }
        printf("%02x ", buf[i]);
    }
    printf("\n");
}

static void element_list_free(ElementList *list) {
    if (!list) return;
    free(list->addrs);
    list->addrs = NULL;
    list->count = 0;
    list->cap = 0;
}

static void element_map_free(ElementMap *map) {
    if (!map) return;
    element_list_free(&map->transports);
    element_list_free(&map->slots);
    element_list_free(&map->drives);
    element_list_free(&map->ie);
}

static void element_list_push(ElementList *list, uint16_t addr) {
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 32;
        uint16_t *next = realloc(list->addrs, new_cap * sizeof(uint16_t));
        if (!next) {
            return;
        }
        list->addrs = next;
        list->cap = new_cap;
    }
    list->addrs[list->count++] = addr;
}

static const char *sense_key_name(uint8_t sense_key) {
    switch (sense_key & 0x0F) {
        case kSENSE_KEY_NO_SENSE: return "NO_SENSE";
        case kSENSE_KEY_RECOVERED_ERROR: return "RECOVERED_ERROR";
        case kSENSE_KEY_NOT_READY: return "NOT_READY";
        case kSENSE_KEY_MEDIUM_ERROR: return "MEDIUM_ERROR";
        case kSENSE_KEY_HARDWARE_ERROR: return "HARDWARE_ERROR";
        case kSENSE_KEY_ILLEGAL_REQUEST: return "ILLEGAL_REQUEST";
        case kSENSE_KEY_UNIT_ATTENTION: return "UNIT_ATTENTION";
        case kSENSE_KEY_DATA_PROTECT: return "DATA_PROTECT";
        case kSENSE_KEY_BLANK_CHECK: return "BLANK_CHECK";
        case kSENSE_KEY_VENDOR_SPECIFIC: return "VENDOR_SPECIFIC";
        case kSENSE_KEY_COPY_ABORTED: return "COPY_ABORTED";
        case kSENSE_KEY_ABORTED_COMMAND: return "ABORTED_COMMAND";
        case kSENSE_KEY_VOLUME_OVERFLOW: return "VOLUME_OVERFLOW";
        case kSENSE_KEY_MISCOMPARE: return "MISCOMPARE";
        case 0x0C: return "RESERVED_0C";
        case 0x0F: return "RESERVED_0F";
        default: return "UNKNOWN";
    }
}

static void print_sense(const SCSI_Sense_Data *sense) {
    uint8_t valid = sense->VALID_RESPONSE_CODE & kSENSE_DATA_VALID_Mask;
    uint8_t response = sense->VALID_RESPONSE_CODE & kSENSE_RESPONSE_CODE_Mask;
    uint8_t key = sense->SENSE_KEY & 0x0F;
    uint8_t asc = sense->ADDITIONAL_SENSE_CODE;
    uint8_t ascq = sense->ADDITIONAL_SENSE_CODE_QUALIFIER;
    printf("Sense: valid=%u response=0x%02x key=%s(0x%02x) asc=0x%02x ascq=0x%02x\n",
           valid ? 1 : 0, response, sense_key_name(key), key, asc, ascq);
}

static int execute_cdb_scsitask(
    ChangerHandle *handle,
    const uint8_t *cdb,
    uint8_t cdb_len,
    void *buffer,
    uint32_t buffer_len,
    uint8_t direction,
    uint32_t timeout_ms
) {
    if (!handle || !handle->scsi_device) return 1;

    SCSITaskInterface **task = (*handle->scsi_device)->CreateSCSITask(handle->scsi_device);
    if (!task) {
        fprintf(stderr, "CreateSCSITask failed.\n");
        return 1;
    }

    (*task)->SetTaskAttribute(task, kSCSITask_SIMPLE);
    (*task)->SetCommandDescriptorBlock(task, (UInt8 *)cdb, cdb_len);
    (*task)->SetTimeoutDuration(task, timeout_ms);

    if (direction == kSCSIDataTransfer_NoDataTransfer) {
        (*task)->SetScatterGatherEntries(task, NULL, 0, 0, direction);
    } else {
        SCSITaskSGElement sg;
#if defined(__LP64__)
        sg.address = (mach_vm_address_t)buffer;
        sg.length = buffer_len;
#else
        sg.address = (UInt32)buffer;
        sg.length = buffer_len;
#endif
        (*task)->SetScatterGatherEntries(task, &sg, 1, buffer_len, direction);
    }

    SCSI_Sense_Data sense;
    memset(&sense, 0, sizeof(sense));
    SCSITaskStatus status = 0;
    UInt64 transferred = 0;

    IOReturn kr = (*task)->ExecuteTaskSync(task, &sense, &status, &transferred);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "ExecuteTaskSync failed: 0x%x\n", kr);
    }

    if (status != kSCSITaskStatus_GOOD) {
        fprintf(stderr, "SCSI task status: 0x%x\n", status);
        print_sense(&sense);
        fprintf(stderr, "Sense data:");
        dump_hex((uint8_t *)&sense, sizeof(sense));
    } else {
        if (buffer && buffer_len > 0) {
            printf("Transferred %llu bytes.\n", (unsigned long long)transferred);
        }
    }

    (*task)->Release(task);
    return (kr == kIOReturnSuccess && status == kSCSITaskStatus_GOOD) ? 0 : 1;
}

static int execute_cdb_sbp2(
    ChangerHandle *handle,
    const uint8_t *cdb,
    uint8_t cdb_len,
    void *buffer,
    uint32_t buffer_len,
    uint8_t direction,
    uint32_t timeout_ms
) {
    if (!handle || !handle->sbp2_login) return 1;

    IUnknownVTbl **orb_unknown = (*handle->sbp2_login)->createORB(
        handle->sbp2_login,
        CFUUIDGetUUIDBytes(kIOFireWireSBP2LibORBInterfaceID)
    );
    if (!orb_unknown) {
        fprintf(stderr, "Failed to create SBP2 ORB.\n");
        return 1;
    }
    IOFireWireSBP2LibORBInterface **orb = (IOFireWireSBP2LibORBInterface **)orb_unknown;

    SBP2StatusWait waiter = {0};
    (*orb)->setRefCon(orb, &waiter);
    (*handle->sbp2_login)->setStatusNotify(handle->sbp2_login, &waiter, sbp2_status_callback);

    UInt32 flags = kFWSBP2CommandCompleteNotify | kFWSBP2CommandNormalORB;
    if (direction == kSCSIDataTransfer_FromTargetToInitiator) {
        flags |= kFWSBP2CommandTransferDataFromTarget;
    }
    (*orb)->setCommandFlags(orb, flags);
    (*orb)->setCommandTimeout(orb, timeout_ms);
    (*orb)->setCommandBlock(orb, (void *)cdb, cdb_len);

    if (direction != kSCSIDataTransfer_NoDataTransfer && buffer && buffer_len > 0) {
        FWSBP2VirtualRange range;
        range.address = buffer;
        range.length = buffer_len;
        UInt32 io_dir = (direction == kSCSIDataTransfer_FromTargetToInitiator) ? kIODirectionIn : kIODirectionOut;
        IOReturn r = (*orb)->setCommandBuffersAsRanges(orb, &range, 1, io_dir, 0, buffer_len);
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "setCommandBuffersAsRanges failed: 0x%x\n", r);
            (*orb)->Release(orb);
            return 1;
        }
    }

    IOReturn kr = (*handle->sbp2_login)->submitORB(handle->sbp2_login, orb);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "submitORB failed: 0x%x\n", kr);
        (*orb)->Release(orb);
        return 1;
    }
    kr = (*handle->sbp2_login)->ringDoorbell(handle->sbp2_login);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "ringDoorbell failed: 0x%x\n", kr);
        (*orb)->Release(orb);
        return 1;
    }

    if (!runloop_wait(&waiter.done, (double)timeout_ms / 1000.0 + 1.0)) {
        fprintf(stderr, "SBP2 command timed out.\n");
        (*orb)->Release(orb);
        return 1;
    }

    if (direction != kSCSIDataTransfer_NoDataTransfer && buffer && buffer_len > 0) {
        (*orb)->releaseCommandBuffers(orb);
    }

    (*orb)->Release(orb);

    if (waiter.notificationEvent != kFWSBP2NormalCommandStatus) {
        fprintf(stderr, "SBP2 notification event: %u\n", waiter.notificationEvent);
        if (waiter.message && waiter.length >= sizeof(FWSBP2StatusBlock)) {
            const FWSBP2StatusBlock *sb = (const FWSBP2StatusBlock *)waiter.message;
            fprintf(stderr, "SBP2 status: 0x%02x details: 0x%02x\n", sb->sbpStatus, sb->details);
        }
        return 1;
    }

    if (buffer && buffer_len > 0) {
        printf("Transferred %u bytes (SBP2).\n", buffer_len);
    }

    return 0;
}

static int execute_cdb(
    ChangerHandle *handle,
    const uint8_t *cdb,
    uint8_t cdb_len,
    void *buffer,
    uint32_t buffer_len,
    uint8_t direction,
    uint32_t timeout_ms
) {
    if (!handle) return 1;
    if (handle->backend == BACKEND_SCSITASK) {
        return execute_cdb_scsitask(handle, cdb, cdb_len, buffer, buffer_len, direction, timeout_ms);
    }
    return execute_cdb_sbp2(handle, cdb, cdb_len, buffer, buffer_len, direction, timeout_ms);
}

static int cmd_inquiry(ChangerHandle *handle) {
    uint8_t cdb[6] = {0};
    cdb[0] = 0x12; // INQUIRY
    cdb[4] = 96;
    uint8_t buf[96];
    memset(buf, 0, sizeof(buf));
    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, sizeof(buf), kSCSIDataTransfer_FromTargetToInitiator, 10000);
    if (rc == 0) {
        printf("INQUIRY data:");
        dump_hex(buf, sizeof(buf));
    }
    return rc;
}

static int cmd_inquiry_vpd(ChangerHandle *handle, uint8_t page) {
    uint8_t cdb[6] = {0};
    cdb[0] = 0x12; // INQUIRY
    cdb[1] = 0x01; // EVPD
    cdb[2] = page;
    uint16_t alloc = 512;
    cdb[4] = (uint8_t)alloc;

    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 10000);
    if (rc == 0) {
        uint16_t page_len = (buf[2] << 8) | buf[3];
        printf("INQUIRY VPD page 0x%02x length=%u\n", page, page_len);
        dump_hex(buf, (page_len + 4 <= alloc) ? page_len + 4 : alloc);
    }
    return rc;
}

static int cmd_report_luns(ChangerHandle *handle) {
    uint8_t cdb[12] = {0};
    cdb[0] = 0xA0; // REPORT LUNS
    uint32_t alloc = 512;
    cdb[6] = (alloc >> 24) & 0xFF;
    cdb[7] = (alloc >> 16) & 0xFF;
    cdb[8] = (alloc >> 8) & 0xFF;
    cdb[9] = alloc & 0xFF;

    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 10000);
    if (rc == 0) {
        uint32_t list_len = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
        printf("REPORT LUNS length=%u\n", list_len);
        dump_hex(buf, (list_len + 8 <= alloc) ? list_len + 8 : alloc);
    }
    return rc;
}

static int cmd_log_sense(ChangerHandle *handle, uint8_t page) {
    uint8_t cdb[10] = {0};
    cdb[0] = 0x4D; // LOG SENSE(10)
    cdb[1] = 0x00;
    cdb[2] = page & 0x3F;
    uint16_t alloc = 512;
    cdb[7] = (alloc >> 8) & 0xFF;
    cdb[8] = alloc & 0xFF;

    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 10000);
    if (rc == 0) {
        uint16_t page_len = (buf[2] << 8) | buf[3];
        printf("LOG SENSE page 0x%02x length=%u\n", page, page_len);
        dump_hex(buf, (page_len + 4 <= alloc) ? page_len + 4 : alloc);
    }
    return rc;
}

static int read_mode_sense_element(ChangerHandle *handle, ElementAddrAssignment *out, bool print) {
    uint8_t cdb[10] = {0};
    cdb[0] = 0x5A; // MODE SENSE(10)
    cdb[1] = 0x08; // DBD=1 (disable block descriptors)
    cdb[2] = 0x1D; // Element Address Assignment page
    cdb[3] = 0x00; // subpage
    uint16_t alloc = 256;
    cdb[7] = (alloc >> 8) & 0xFF;
    cdb[8] = alloc & 0xFF;

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 10000);
    if (rc != 0) {
        return rc;
    }

    if (alloc < 8) {
        fprintf(stderr, "MODE SENSE response too short.\n");
        return 1;
    }

    uint16_t mode_data_length = (buf[0] << 8) | buf[1];
    uint16_t block_desc_len = (buf[6] << 8) | buf[7];
    uint32_t page_offset = 8 + block_desc_len;
    if (page_offset + 2 > alloc) {
        fprintf(stderr, "MODE SENSE page offset out of range.\n");
        return 1;
    }

    uint8_t page_code = buf[page_offset] & 0x3F;
    uint8_t page_len = buf[page_offset + 1];
    if (print) {
        printf("Mode Sense (Element Address Assignment):\n");
        printf("  Mode Data Length: %u\n", mode_data_length);
        printf("  Page Code: 0x%02x Length: %u\n", page_code, page_len);
    }

    if (page_code != 0x1D || page_len < 16) {
        if (print) {
            printf("  Unexpected page code/length; dumping raw page bytes.\n");
            dump_hex(&buf[page_offset], (size_t)page_len + 2);
        }
        return 0;
    }

    const uint8_t *p = &buf[page_offset + 2];
    if (out) {
        out->first_transport = (p[0] << 8) | p[1];
        out->num_transport = (p[2] << 8) | p[3];
        out->first_storage = (p[4] << 8) | p[5];
        out->num_storage = (p[6] << 8) | p[7];
        out->first_ie = (p[8] << 8) | p[9];
        out->num_ie = (p[10] << 8) | p[11];
        out->first_drive = (p[12] << 8) | p[13];
        out->num_drive = (p[14] << 8) | p[15];
    }

    if (print && out) {
        printf("  Transport: first=0x%04x count=%u\n", out->first_transport, out->num_transport);
        printf("  Storage:   first=0x%04x count=%u\n", out->first_storage, out->num_storage);
        printf("  IE:        first=0x%04x count=%u\n", out->first_ie, out->num_ie);
        printf("  Drive:     first=0x%04x count=%u\n", out->first_drive, out->num_drive);
    }
    return 0;
}

static int cmd_mode_sense_element(ChangerHandle *handle) {
    ElementAddrAssignment assign = {0};
    return read_mode_sense_element(handle, &assign, true);
}

static int cmd_probe_storage(ChangerHandle *handle) {
    ElementAddrAssignment assign = {0};
    int rc = read_mode_sense_element(handle, &assign, true);
    if (rc != 0) return rc;

    if (assign.num_storage == 0) {
        fprintf(stderr, "No storage elements reported by MODE SENSE.\n");
        return 1;
    }

    uint16_t step = (assign.num_storage > 40) ? 40 : assign.num_storage;
    uint32_t alloc = 4096;
    uint8_t *buf = calloc(1, alloc);
    if (!buf) {
        fprintf(stderr, "Allocation failed.\n");
        return 1;
    }

    printf("\nProbing READ ELEMENT STATUS ranges (storage):\n");
    for (uint16_t offset = 0; offset < assign.num_storage; offset += step) {
        uint16_t start = assign.first_storage + offset;
        uint16_t count = assign.num_storage - offset;
        if (count > step) count = step;

        uint8_t cdb[12] = {0};
        cdb[0] = 0xB8; // READ ELEMENT STATUS
        cdb[1] = 0x02; // storage
        cdb[2] = (start >> 8) & 0xFF;
        cdb[3] = start & 0xFF;
        cdb[4] = (count >> 8) & 0xFF;
        cdb[5] = count & 0xFF;
        cdb[6] = (alloc >> 16) & 0xFF;
        cdb[7] = (alloc >> 8) & 0xFF;
        cdb[8] = alloc & 0xFF;

        memset(buf, 0, alloc);
        rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 30000);
        if (rc != 0) {
            printf("  start=0x%04x count=%u -> error\n", start, count);
            continue;
        }
        uint16_t first_elem = (buf[0] << 8) | buf[1];
        uint16_t num_elem = (buf[2] << 8) | buf[3];
        uint32_t report_bytes = (buf[5] << 16) | (buf[6] << 8) | buf[7];
        printf("  start=0x%04x count=%u -> header first=0x%04x num=%u bytes=%u\n",
               start, count, first_elem, num_elem, report_bytes);
        if (report_bytes > 0) {
            parse_element_status(buf, alloc);
        }
    }

    free(buf);
    return 0;
}

static int cmd_test_unit_ready(ChangerHandle *handle) {
    uint8_t cdb[6] = {0};
    cdb[0] = 0x00; // TEST UNIT READY
    return execute_cdb(handle, cdb, sizeof(cdb), NULL, 0, kSCSIDataTransfer_NoDataTransfer, 10000);
}

static int cmd_init_status(ChangerHandle *handle) {
    uint8_t cdb[6] = {0};
    cdb[0] = 0x07; // INITIALIZE ELEMENT STATUS
    return execute_cdb(handle, cdb, sizeof(cdb), NULL, 0, kSCSIDataTransfer_NoDataTransfer, 60000);
}

static const char *element_type_name(uint8_t type) {
    switch (type) {
        case 0x01: return "transport";
        case 0x02: return "storage";
        case 0x03: return "import/export";
        case 0x04: return "drive";
        default: return "unknown";
    }
}

static void print_flags(uint8_t flags) {
    bool except = (flags & 0x80) != 0;
    bool full = (flags & 0x01) != 0;
    printf(" except=%s full=%s flags=0x%02x", except ? "1" : "0", full ? "1" : "0", flags);
}

static void parse_element_status(const uint8_t *buf, uint32_t len) {
    if (len < 8) {
        printf("Element status buffer too short.\n");
        return;
    }

    uint16_t first_elem = (buf[0] << 8) | buf[1];
    uint16_t num_elem = (buf[2] << 8) | buf[3];
    uint32_t report_bytes = (buf[5] << 16) | (buf[6] << 8) | buf[7];

    printf("Element Status Header:\n");
    printf("  First Element Address Reported: 0x%04x\n", first_elem);
    printf("  Number of Elements Available:   %u\n", num_elem);
    printf("  Byte Count of Report Available: %u\n", report_bytes);

    uint32_t offset = 8;
    unsigned slot_index = 0;
    unsigned drive_index = 0;
    unsigned ie_index = 0;
    unsigned transport_index = 0;

    while (offset + 8 <= len) {
        uint8_t type = buf[offset] & 0x0F;
        uint8_t flags = buf[offset + 1];
        bool pvol = (flags & 0x80) != 0;
        bool avol = (flags & 0x40) != 0;
        uint16_t desc_len = (buf[offset + 2] << 8) | buf[offset + 3];
        uint32_t page_bytes = (buf[offset + 5] << 16) | (buf[offset + 6] << 8) | buf[offset + 7];
        offset += 8;

        if (desc_len == 0 || page_bytes == 0) {
            break;
        }

        printf("\nElement Status Page: %s (type=0x%02x)\n", element_type_name(type), type);
        printf("  Descriptor Length: %u\n", desc_len);
        printf("  Descriptor Bytes:  %u\n", page_bytes);
        printf("  PVolTag: %u  AVolTag: %u\n", pvol ? 1 : 0, avol ? 1 : 0);

        uint32_t page_end = offset + page_bytes;
        if (page_end > len) {
            page_end = len;
        }

        while (offset + desc_len <= page_end) {
            uint16_t elem_addr = (buf[offset] << 8) | buf[offset + 1];
            uint8_t elem_flags = buf[offset + 2];
            printf("  Element 0x%04x", elem_addr);

            if (type == 0x02) {
                slot_index++;
                printf(" (slot %u)", slot_index);
            } else if (type == 0x04) {
                drive_index++;
                printf(" (drive %u)", drive_index);
            } else if (type == 0x03) {
                ie_index++;
                printf(" (ie %u)", ie_index);
            } else if (type == 0x01) {
                transport_index++;
                printf(" (transport %u)", transport_index);
            }

            print_flags(elem_flags);

            if (desc_len >= 12) {
                uint8_t svalid = buf[offset + 9] & 0x80;
                uint16_t src_addr = (buf[offset + 10] << 8) | buf[offset + 11];
                if (svalid) {
                    printf(" src=0x%04x", src_addr);
                }
            }

            printf("\n");
            offset += desc_len;
        }

        if (offset < page_end) {
            offset = page_end;
        }
    }
}

static bool parse_element_status_map(const uint8_t *buf, uint32_t len, ElementMap *map) {
    if (!map || len < 8) return false;
    uint32_t offset = 8;
    while (offset + 8 <= len) {
        uint8_t type = buf[offset] & 0x0F;
        uint16_t desc_len = (buf[offset + 2] << 8) | buf[offset + 3];
        uint32_t page_bytes = (buf[offset + 5] << 16) | (buf[offset + 6] << 8) | buf[offset + 7];
        offset += 8;

        if (desc_len == 0 || page_bytes == 0) break;

        uint32_t page_end = offset + page_bytes;
        if (page_end > len) page_end = len;

        while (offset + desc_len <= page_end) {
            if (desc_len < 2) {
                offset = page_end;
                break;
            }
            uint16_t elem_addr = (buf[offset] << 8) | buf[offset + 1];
            if (type == 0x02 && elem_addr == 0x0000) {
                bool all_zero = true;
                for (uint16_t i = 0; i < desc_len; i++) {
                    if (buf[offset + i] != 0x00) {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) {
                    offset += desc_len;
                    continue;
                }
            }
            if (type == 0x01) {
                element_list_push(&map->transports, elem_addr);
            } else if (type == 0x02) {
                element_list_push(&map->slots, elem_addr);
            } else if (type == 0x03) {
                element_list_push(&map->ie, elem_addr);
            } else if (type == 0x04) {
                element_list_push(&map->drives, elem_addr);
            }
            offset += desc_len;
        }
        if (offset < page_end) offset = page_end;
    }
    return (map->transports.count + map->slots.count + map->drives.count + map->ie.count) > 0;
}

static int cmd_read_element_status(ChangerHandle *handle, uint8_t element_type, uint16_t start, uint16_t count, uint32_t alloc, bool dump_raw) {
    uint8_t cdb[12] = {0};
    cdb[0] = 0xB8; // READ ELEMENT STATUS
    cdb[1] = (element_type & 0x0F);
    cdb[2] = (start >> 8) & 0xFF;
    cdb[3] = start & 0xFF;
    cdb[4] = (count >> 8) & 0xFF;
    cdb[5] = count & 0xFF;
    cdb[6] = (alloc >> 16) & 0xFF;
    cdb[7] = (alloc >> 8) & 0xFF;
    cdb[8] = alloc & 0xFF;
    cdb[9] = 0;
    cdb[10] = 0;
    cdb[11] = 0;

    uint8_t *buf = calloc(1, alloc);
    if (!buf) {
        fprintf(stderr, "Allocation failed.\n");
        return 1;
    }

    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 30000);
    if (rc != 0 && element_type != 0x00) {
        fprintf(stderr, "READ ELEMENT STATUS failed for type '%s'; retrying with element-type=all.\n",
                element_type_name(element_type));
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0xB8; // READ ELEMENT STATUS
        cdb[1] = 0x00; // all element types
        cdb[4] = 0xFF;
        cdb[5] = 0xFF;
        cdb[6] = (alloc >> 16) & 0xFF;
        cdb[7] = (alloc >> 8) & 0xFF;
        cdb[8] = alloc & 0xFF;
        memset(buf, 0, alloc);
        rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 30000);
    }

    if (rc == 0) {
        parse_element_status(buf, alloc);
        if (dump_raw) {
            printf("\nRAW READ ELEMENT STATUS data:");
            dump_hex(buf, alloc);
        }
    }
    free(buf);
    return rc;
}

// Eject any mounted optical media before unloading from drive.
// Returns 0 on success (or no optical media found), non-zero on failure.
static int eject_optical_media(void) {
    // Use popen to run diskutil and find optical drives
    FILE *fp = popen("diskutil list external 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "Warning: Could not run diskutil to check for optical media.\n");
        return 0; // Continue anyway
    }

    char line[512];
    char disk_to_eject[64] = {0};

    while (fgets(line, sizeof(line), fp)) {
        // Look for lines like "/dev/disk4 (external, physical):"
        if (strncmp(line, "/dev/disk", 9) == 0) {
            // Extract disk identifier (e.g., "disk4")
            char *start = line + 5; // Skip "/dev/"
            char *end = strchr(start, ' ');
            if (end && (end - start) < (int)sizeof(disk_to_eject)) {
                strncpy(disk_to_eject, start, end - start);
                disk_to_eject[end - start] = '\0';
            }
        }
        // Check if this is an optical disc (CD/DVD partition scheme)
        if (strstr(line, "CD_partition_scheme") || strstr(line, "DVD_partition_scheme") ||
            strstr(line, "CD_DA") || strstr(line, "BD_partition_scheme")) {
            if (disk_to_eject[0] != '\0') {
                break; // Found an optical disc
            }
        }
    }
    pclose(fp);

    if (disk_to_eject[0] == '\0') {
        // No optical media found, nothing to eject
        return 0;
    }

    printf("Ejecting optical media (%s) before unload...\n", disk_to_eject);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "diskutil eject %s 2>&1", disk_to_eject);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "Warning: diskutil eject returned %d\n", ret);
        // Continue anyway - the physical move might still work
    }

    // Give the system a moment to process the eject
    usleep(500000); // 500ms

    return 0;
}

// Get info about mounted optical disc. Returns disc name in out_name (caller provides buffer).
// Returns true if an optical disc is found, false otherwise.
static bool get_mounted_disc_info(char *out_name, size_t name_len, char *out_size, size_t size_len) {
    if (out_name && name_len > 0) out_name[0] = '\0';
    if (out_size && size_len > 0) out_size[0] = '\0';

    FILE *fp = popen("diskutil list external 2>/dev/null", "r");
    if (!fp) return false;

    char line[512];
    bool found_optical = false;
    bool in_optical_disk = false;

    while (fgets(line, sizeof(line), fp)) {
        // Look for external disk header
        if (strncmp(line, "/dev/disk", 9) == 0) {
            in_optical_disk = false;
        }
        // Check if this is an optical disc
        if (strstr(line, "CD_partition_scheme") || strstr(line, "DVD_partition_scheme") ||
            strstr(line, "BD_partition_scheme")) {
            in_optical_disk = true;
            found_optical = true;
            // Parse line like "   0:        CD_partition_scheme You By Me: Vol. 1      *385.6 MB   disk4"
            // Find the disc name - it's between the scheme type and the size (*xxx MB/GB)
            char *scheme_end = strstr(line, "_scheme");
            if (scheme_end) {
                scheme_end += 7; // skip "_scheme"
                while (*scheme_end == ' ') scheme_end++;
                // Find the size marker (starts with *)
                char *size_start = strstr(scheme_end, "*");
                if (size_start && out_name && name_len > 0) {
                    // Name is between scheme_end and size_start
                    char *name_end = size_start;
                    while (name_end > scheme_end && *(name_end-1) == ' ') name_end--;
                    size_t copy_len = name_end - scheme_end;
                    if (copy_len >= name_len) copy_len = name_len - 1;
                    strncpy(out_name, scheme_end, copy_len);
                    out_name[copy_len] = '\0';
                }
                // Get size
                if (size_start && out_size && size_len > 0) {
                    size_start++; // skip *
                    char *size_end = size_start;
                    while (*size_end && *size_end != ' ' && *size_end != '\t') size_end++;
                    // Include unit (MB/GB)
                    while (*size_end == ' ') size_end++;
                    while (*size_end && *size_end != ' ' && *size_end != '\t') size_end++;
                    size_t copy_len = size_end - size_start;
                    if (copy_len >= size_len) copy_len = size_len - 1;
                    strncpy(out_size, size_start, copy_len);
                    out_size[copy_len] = '\0';
                }
            }
            break;
        }
    }
    pclose(fp);
    return found_optical;
}

// DiskArbitration callback context
typedef struct {
    bool found;
    char name[256];
    char size[64];
} DACallbackContext;

// Callback for disk appeared event
static void disk_appeared_callback(DADiskRef disk, void *context) {
    DACallbackContext *ctx = (DACallbackContext *)context;
    if (ctx->found) return; // Already found one

    CFDictionaryRef desc = DADiskCopyDescription(disk);
    if (!desc) return;

    // Check if this is an optical disc (CD/DVD/BD)
    CFStringRef mediaType = CFDictionaryGetValue(desc, kDADiskDescriptionMediaTypeKey);
    CFStringRef mediaKind = CFDictionaryGetValue(desc, kDADiskDescriptionMediaKindKey);

    bool is_optical = false;
    if (mediaKind) {
        char kind[128] = {0};
        CFStringGetCString(mediaKind, kind, sizeof(kind), kCFStringEncodingUTF8);
        if (strstr(kind, "CD") || strstr(kind, "DVD") || strstr(kind, "BD")) {
            is_optical = true;
        }
    }
    if (mediaType) {
        char type[128] = {0};
        CFStringGetCString(mediaType, type, sizeof(type), kCFStringEncodingUTF8);
        if (strstr(type, "CD") || strstr(type, "DVD") || strstr(type, "BD")) {
            is_optical = true;
        }
    }

    if (is_optical) {
        ctx->found = true;

        // Get volume name
        CFStringRef volName = CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey);
        if (volName) {
            CFStringGetCString(volName, ctx->name, sizeof(ctx->name), kCFStringEncodingUTF8);
        } else {
            // Try media name
            CFStringRef mediaName = CFDictionaryGetValue(desc, kDADiskDescriptionMediaNameKey);
            if (mediaName) {
                CFStringGetCString(mediaName, ctx->name, sizeof(ctx->name), kCFStringEncodingUTF8);
            }
        }

        // Get size
        CFNumberRef sizeNum = CFDictionaryGetValue(desc, kDADiskDescriptionMediaSizeKey);
        if (sizeNum) {
            long long size = 0;
            CFNumberGetValue(sizeNum, kCFNumberLongLongType, &size);
            if (size >= 1000000000) {
                snprintf(ctx->size, sizeof(ctx->size), "%.1f GB", size / 1000000000.0);
            } else {
                snprintf(ctx->size, sizeof(ctx->size), "%.1f MB", size / 1000000.0);
            }
        }

        CFRunLoopStop(CFRunLoopGetCurrent());
    }

    CFRelease(desc);
}

// Timer callback for timeout
static void timeout_callback(CFRunLoopTimerRef timer __attribute__((unused)), void *info) {
    bool *timed_out = (bool *)info;
    *timed_out = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

// Wait for disc to be mounted using DiskArbitration and print info
static void wait_and_print_mounted_disc(void) {
    // First check if already mounted
    char name[256] = {0};
    char size[64] = {0};
    if (get_mounted_disc_info(name, sizeof(name), size, sizeof(size))) {
        printf("  Mounted: %s (%s)\n", name[0] ? name : "Unknown", size[0] ? size : "?");
        return;
    }

    // Set up DiskArbitration session
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) {
        printf("  Mounted: (unable to create DA session)\n");
        return;
    }

    DACallbackContext ctx = {0};
    bool timed_out = false;

    // Register for disk appeared events
    DARegisterDiskAppearedCallback(session, NULL, disk_appeared_callback, &ctx);
    DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    // Set up timeout (30 seconds)
    CFRunLoopTimerContext timerCtx = { 0, &timed_out, NULL, NULL, NULL };
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 30.0, 0, 0, 0, timeout_callback, &timerCtx);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    // Run until disc appears or timeout
    CFRunLoopRun();

    // Cleanup
    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
    DAUnregisterCallback(session, disk_appeared_callback, &ctx);
    DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRelease(session);

    if (ctx.found) {
        printf("  Mounted: %s (%s)\n", ctx.name[0] ? ctx.name : "Audio CD", ctx.size[0] ? ctx.size : "?");
    } else if (timed_out) {
        printf("  Mounted: (timed out waiting for disc)\n");
    } else {
        printf("  Mounted: (unknown)\n");
    }
}

// Structure to hold element status info
typedef struct {
    uint16_t addr;
    bool full;
    bool valid_src;
    uint16_t src_addr;
} ElementStatus;

// Read element status and find info for specific elements
// Returns 0 on success, fills in drive_status and slot_status if non-NULL
static int read_element_status_info(ChangerHandle *handle, uint16_t drive_addr, ElementStatus *drive_status,
                                    uint16_t slot_addr, ElementStatus *slot_status) {
    uint32_t alloc = 4096;
    uint8_t cdb[12] = {0};
    cdb[0] = 0xB8; // READ ELEMENT STATUS
    cdb[1] = 0x00; // all element types
    cdb[4] = 0xFF;
    cdb[5] = 0xFF;
    cdb[6] = (alloc >> 16) & 0xFF;
    cdb[7] = (alloc >> 8) & 0xFF;
    cdb[8] = alloc & 0xFF;

    uint8_t *buf = calloc(1, alloc);
    if (!buf) return -1;

    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 30000);
    if (rc != 0) {
        free(buf);
        return rc;
    }

    // Initialize output
    if (drive_status) {
        drive_status->addr = drive_addr;
        drive_status->full = false;
        drive_status->valid_src = false;
        drive_status->src_addr = 0;
    }
    if (slot_status) {
        slot_status->addr = slot_addr;
        slot_status->full = false;
        slot_status->valid_src = false;
        slot_status->src_addr = 0;
    }

    // Parse element status pages
    uint32_t len = alloc;
    if (len < 8) {
        free(buf);
        return 0;
    }

    uint32_t offset = 8;
    while (offset + 8 <= len) {
        uint16_t desc_len = (buf[offset + 2] << 8) | buf[offset + 3];
        uint32_t page_bytes = (buf[offset + 5] << 16) | (buf[offset + 6] << 8) | buf[offset + 7];
        offset += 8;

        if (desc_len == 0 || page_bytes == 0) break;

        uint32_t page_end = offset + page_bytes;
        if (page_end > len) page_end = len;

        while (offset + desc_len <= page_end) {
            uint16_t elem_addr = (buf[offset] << 8) | buf[offset + 1];
            uint8_t elem_flags = buf[offset + 2];
            bool full = (elem_flags & 0x01) != 0;

            bool svalid = false;
            uint16_t src = 0;
            if (desc_len >= 12) {
                svalid = (buf[offset + 9] & 0x80) != 0;
                src = (buf[offset + 10] << 8) | buf[offset + 11];
            }

            if (drive_status && elem_addr == drive_addr) {
                drive_status->full = full;
                drive_status->valid_src = svalid;
                drive_status->src_addr = src;
            }
            if (slot_status && elem_addr == slot_addr) {
                slot_status->full = full;
                slot_status->valid_src = svalid;
                slot_status->src_addr = src;
            }

            offset += desc_len;
        }

        if (offset < page_end) {
            offset = page_end;
        }
    }

    free(buf);
    return 0;
}

static int cmd_move_medium(ChangerHandle *handle, uint16_t transport, uint16_t source, uint16_t dest) {
    uint8_t cdb[12] = {0};
    cdb[0] = 0xA5; // MOVE MEDIUM
    cdb[2] = (transport >> 8) & 0xFF;
    cdb[3] = transport & 0xFF;
    cdb[4] = (source >> 8) & 0xFF;
    cdb[5] = source & 0xFF;
    cdb[6] = (dest >> 8) & 0xFF;
    cdb[7] = dest & 0xFF;
    return execute_cdb(handle, cdb, sizeof(cdb), NULL, 0, kSCSIDataTransfer_NoDataTransfer, 60000);
}

static int fetch_element_map(ChangerHandle *handle, ElementMap *map) {
    uint32_t alloc = 65535;
    uint8_t *buf = calloc(1, alloc);
    if (!buf) return 1;

    // First query "all types" to get transport, IE, and drive elements
    // (Some devices only respond to "all types" for these element types)
    uint8_t cdb[12] = {0};
    cdb[0] = 0xB8; // READ ELEMENT STATUS
    cdb[1] = 0x00; // all element types
    cdb[2] = 0x00; // starting element address (high)
    cdb[3] = 0x00; // starting element address (low)
    cdb[4] = 0xFF; // number of elements (high) - request maximum
    cdb[5] = 0xFF; // number of elements (low)
    cdb[6] = (alloc >> 16) & 0xFF;
    cdb[7] = (alloc >> 8) & 0xFF;
    cdb[8] = alloc & 0xFF;

    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 60000);
    if (rc != 0) {
        free(buf);
        return rc;
    }

    uint32_t report_bytes = (buf[5] << 16) | (buf[6] << 8) | buf[7];
    if (report_bytes == 0) {
        free(buf);
        return 1;
    }

    uint32_t parse_len = (report_bytes + 8 <= alloc) ? report_bytes + 8 : alloc;
    (void)parse_element_status_map(buf, parse_len, map);

    // Now query storage elements separately - some devices return truncated
    // storage lists when using "all types" but return full lists when querying
    // storage specifically. Some devices also paginate results (max ~40 per query).
    ElementAddrAssignment assign = {0};
    if (read_mode_sense_element(handle, &assign, false) == 0 && assign.num_storage > 0) {
        // Clear existing slots - we'll rebuild from paginated queries
        free(map->slots.addrs);
        map->slots.addrs = NULL;
        map->slots.count = 0;
        map->slots.cap = 0;

        // Paginate through storage elements - device may return max ~40 per query
        uint16_t start_addr = assign.first_storage;
        uint16_t remaining = assign.num_storage;

        while (remaining > 0) {
            memset(cdb, 0, sizeof(cdb));
            cdb[0] = 0xB8; // READ ELEMENT STATUS
            cdb[1] = 0x02; // storage elements only
            cdb[2] = (start_addr >> 8) & 0xFF;
            cdb[3] = start_addr & 0xFF;
            cdb[4] = (remaining >> 8) & 0xFF;
            cdb[5] = remaining & 0xFF;
            cdb[6] = (alloc >> 16) & 0xFF;
            cdb[7] = (alloc >> 8) & 0xFF;
            cdb[8] = alloc & 0xFF;

            memset(buf, 0, alloc);
            rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 60000);
            if (rc != 0) break;

            report_bytes = (buf[5] << 16) | (buf[6] << 8) | buf[7];
            if (report_bytes == 0) break;

            // Parse this page of results
            parse_len = (report_bytes + 8 <= alloc) ? report_bytes + 8 : alloc;
            size_t before = map->slots.count;
            (void)parse_element_status_map(buf, parse_len, map);
            size_t added = map->slots.count - before;

            if (added == 0) break; // No new elements, stop pagination

            // Move to next page
            start_addr += (uint16_t)added;
            if (added >= remaining) break;
            remaining -= (uint16_t)added;
        }

        // Fill in missing slots from MODE SENSE if device didn't return all
        // Some devices (like VGP-XL1B) have firmware quirks where READ ELEMENT STATUS
        // doesn't return all slots even though they physically exist
        if (map->slots.count < assign.num_storage) {
            uint16_t expected_end = assign.first_storage + assign.num_storage;
            uint16_t actual_end = assign.first_storage + (uint16_t)map->slots.count;
            for (uint16_t addr = actual_end; addr < expected_end; addr++) {
                element_list_push(&map->slots, addr);
            }
        }
    }

    free(buf);
    return (map->transports.count + map->slots.count + map->drives.count + map->ie.count) > 0 ? 0 : 1;
}

static void print_element_map(const ElementMap *map) {
    printf("Element Map:\n");
    printf("  Transports: %zu\n", map->transports.count);
    for (size_t i = 0; i < map->transports.count; i++) {
        printf("    transport %zu -> 0x%04x\n", i + 1, map->transports.addrs[i]);
    }
    printf("  Slots: %zu\n", map->slots.count);
    for (size_t i = 0; i < map->slots.count; i++) {
        printf("    slot %zu -> 0x%04x\n", i + 1, map->slots.addrs[i]);
    }
    printf("  Drives: %zu\n", map->drives.count);
    for (size_t i = 0; i < map->drives.count; i++) {
        printf("    drive %zu -> 0x%04x\n", i + 1, map->drives.addrs[i]);
    }
    printf("  Import/Export: %zu\n", map->ie.count);
    for (size_t i = 0; i < map->ie.count; i++) {
        printf("    ie %zu -> 0x%04x\n", i + 1, map->ie.addrs[i]);
    }
}

static void warn_if_slot_mismatch(ChangerHandle *handle, const ElementMap *map) {
    if (!handle || !map) return;
    ElementAddrAssignment assign = {0};
    if (read_mode_sense_element(handle, &assign, false) != 0) {
        return;
    }
    if (assign.num_storage > 0 && map->slots.count > 0 &&
        assign.num_storage != map->slots.count) {
        if (map->slots.count < assign.num_storage / 2) {
            // Significant mismatch - likely missing magazines
            fprintf(stderr,
                    "Warning: Device capacity is %u slots but only %zu are responding.\n"
                    "         Check that all magazines are properly installed.\n",
                    assign.num_storage, map->slots.count);
        } else {
            fprintf(stderr,
                    "Note: MODE SENSE reports max capacity of %u slots, "
                    "device reports %zu installed.\n",
                    assign.num_storage, map->slots.count);
        }
    }
}

static bool parse_u16(const char *s, uint16_t *out) {
    if (!s || !out) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out) {
    if (!s || !out) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFFFFFFFFUL) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u8(const char *s, uint8_t *out) {
    if (!s || !out) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

static bool parse_element_type(const char *s, uint8_t *out) {
    if (!s || !out) return false;
    if (strcmp(s, "all") == 0) { *out = 0x00; return true; }
    if (strcmp(s, "transport") == 0) { *out = 0x01; return true; }
    if (strcmp(s, "storage") == 0) { *out = 0x02; return true; }
    if (strcmp(s, "ie") == 0) { *out = 0x03; return true; }
    if (strcmp(s, "drive") == 0) { *out = 0x04; return true; }
    return false;
}

static bool parse_index(const char *s, size_t *out) {
    if (!s || !out) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!end || *end != '\0' || v == 0 || v > 65535) return false;
    *out = (size_t)v;
    return true;
}

static bool confirm_move(void) {
    fprintf(stderr, "Confirm move? Type 'yes' to proceed: ");
    char buf[16] = {0};
    if (!fgets(buf, sizeof(buf), stdin)) {
        return false;
    }
    return (strncmp(buf, "yes", 3) == 0);
}

/*
 * =============================================================================
 * CLI Main (excluded when building as library with -DMCHANGER_NO_MAIN)
 * =============================================================================
 */

#ifndef MCHANGER_NO_MAIN

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    bool force = false;
    bool skip_tur = false;
    bool dry_run = false;
    bool confirm = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) force = true;
        if (strcmp(argv[i], "--no-tur") == 0) skip_tur = true;
        if (strcmp(argv[i], "--dry-run") == 0) dry_run = true;
        if (strcmp(argv[i], "--confirm") == 0) confirm = true;
        if (strcmp(argv[i], "--debug") == 0) g_debug = true;
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_verbose = true;
    }

    if (strcmp(argv[1], "list") == 0) {
        list_changers();
        return 0;
    }
    if (strcmp(argv[1], "list-all") == 0) {
        list_all_scsi_devices();
        return 0;
    }
    if (strcmp(argv[1], "scan-changers") == 0) {
        scan_changers();
        return 0;
    }
    if (strcmp(argv[1], "list-sbp2") == 0) {
        list_sbp2_luns();
        return 0;
    }
    if (strcmp(argv[1], "scan-sbp2") == 0) {
        scan_sbp2_luns();
        return 0;
    }

    ChangerHandle handle = open_changer(!force);
    if ((handle.backend == BACKEND_SCSITASK && !handle.scsi_device) ||
        (handle.backend == BACKEND_SBP2 && !handle.sbp2_login)) {
        return 1;
    }

    int rc = 0;
    if (strcmp(argv[1], "sanity-check") == 0) {
        if (handle.backend == BACKEND_SCSITASK) {
            printf("Backend: SCSITask\n");
        } else if (handle.backend == BACKEND_SBP2) {
            printf("Backend: SBP2\n");
        } else {
            printf("Backend: unknown\n");
        }
        printf("User client open: OK\n");
        rc = 0;
        goto out;
    }
    if (!skip_tur && strcmp(argv[1], "test-unit-ready") != 0) {
        int tur = cmd_test_unit_ready(&handle);
        if (tur != 0 && !force) {
            fprintf(stderr, "TEST UNIT READY failed. Use --force to continue.\n");
            rc = 1; goto out;
        }
    }

    if (strcmp(argv[1], "test-unit-ready") == 0) {
        rc = cmd_test_unit_ready(&handle);
    } else if (strcmp(argv[1], "inquiry") == 0) {
        rc = cmd_inquiry(&handle);
    } else if (strcmp(argv[1], "inquiry-vpd") == 0) {
        uint8_t page = 0;
        bool have_page = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
                have_page = parse_u8(argv[++i], &page);
            }
        }
        if (!have_page) {
            fprintf(stderr, "Missing or invalid --page.\n");
            rc = 1; goto out;
        }
        rc = cmd_inquiry_vpd(&handle, page);
    } else if (strcmp(argv[1], "report-luns") == 0) {
        rc = cmd_report_luns(&handle);
    } else if (strcmp(argv[1], "log-sense") == 0) {
        uint8_t page = 0;
        bool have_page = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
                have_page = parse_u8(argv[++i], &page);
            }
        }
        if (!have_page) {
            fprintf(stderr, "Missing or invalid --page.\n");
            rc = 1; goto out;
        }
        rc = cmd_log_sense(&handle, page);
    } else if (strcmp(argv[1], "mode-sense-element") == 0) {
        rc = cmd_mode_sense_element(&handle);
    } else if (strcmp(argv[1], "probe-storage") == 0) {
        rc = cmd_probe_storage(&handle);
    } else if (strcmp(argv[1], "init-status") == 0) {
        rc = cmd_init_status(&handle);
    } else if (strcmp(argv[1], "read-element-status") == 0) {
        uint8_t element_type = 0;
        uint16_t start = 0;
        uint16_t count = 0;
        uint32_t alloc = 0;
        bool dump_raw = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--element-type") == 0 && i + 1 < argc) {
                if (!parse_element_type(argv[++i], &element_type)) {
                    fprintf(stderr, "Invalid element type.\n");
                    rc = 1; goto out;
                }
            } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
                if (!parse_u16(argv[++i], &start)) {
                    fprintf(stderr, "Invalid --start.\n");
                    rc = 1; goto out;
                }
            } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
                if (!parse_u16(argv[++i], &count)) {
                    fprintf(stderr, "Invalid --count.\n");
                    rc = 1; goto out;
                }
            } else if (strcmp(argv[i], "--alloc") == 0 && i + 1 < argc) {
                if (!parse_u32(argv[++i], &alloc)) {
                    fprintf(stderr, "Invalid --alloc.\n");
                    rc = 1; goto out;
                }
            } else if (strcmp(argv[i], "--raw") == 0) {
                dump_raw = true;
            }
        }
        if (alloc == 0) {
            fprintf(stderr, "Missing --alloc.\n");
            rc = 1; goto out;
        }
        rc = cmd_read_element_status(&handle, element_type, start, count, alloc, dump_raw);
    } else if (strcmp(argv[1], "move") == 0) {
        uint16_t transport = 0, source = 0, dest = 0;
        bool have_transport = false, have_source = false, have_dest = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
                have_source = parse_u16(argv[++i], &source);
            } else if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
                have_dest = parse_u16(argv[++i], &dest);
            }
        }
        if (!have_transport || !have_source || !have_dest) {
            fprintf(stderr, "Missing --transport, --source, or --dest.\n");
            rc = 1; goto out;
        }
        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, source, dest);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1; goto out;
            }
            rc = cmd_move_medium(&handle, transport, source, dest);
        }
    } else if (strcmp(argv[1], "list-map") == 0) {
        ElementMap map = {0};
        rc = fetch_element_map(&handle, &map);
        if (rc == 0) {
            print_element_map(&map);
            warn_if_slot_mismatch(&handle, &map);
        } else {
            fprintf(stderr, "Failed to read element map.\n");
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "load") == 0 || strcmp(argv[1], "load-slot") == 0) {
        size_t slot_index = 0, drive_index = 1; // default to drive 1
        bool have_slot = false;
        bool have_transport = false;
        uint16_t transport = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
                have_slot = parse_index(argv[++i], &slot_index);
            } else if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc) {
                parse_index(argv[++i], &drive_index);
            } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            }
        }
        if (!have_slot) {
            fprintf(stderr, "Missing --slot.\n");
            rc = 1; goto out;
        }
        ElementMap map = {0};
        rc = fetch_element_map(&handle, &map);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element map.\n");
            element_map_free(&map);
            goto out;
        }
        if (slot_index == 0 || slot_index > map.slots.count ||
            drive_index == 0 || drive_index > map.drives.count) {
            fprintf(stderr, "Slot/drive out of range. Slots: %zu, Drives: %zu\n",
                    map.slots.count, map.drives.count);
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (!have_transport) {
            if (map.transports.count == 0) {
                fprintf(stderr, "No transport element found.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            transport = map.transports.addrs[0];
        }
        uint16_t slot_addr = map.slots.addrs[slot_index - 1];
        uint16_t drive_addr = map.drives.addrs[drive_index - 1];

        // Check if drive already has a disc - if so, unload it first
        ElementStatus drive_st = {0}, target_slot_st = {0};
        rc = read_element_status_info(&handle, drive_addr, &drive_st, slot_addr, &target_slot_st);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element status.\n");
            element_map_free(&map);
            goto out;
        }

        // Check if the target slot's disc is already in the drive
        if (target_slot_st.full) {
            // Slot has disc, we can load it
        } else if (drive_st.full && drive_st.valid_src && drive_st.src_addr == slot_addr) {
            // The disc we want is already in the drive
            printf("LOAD: Disc from slot %u is already in drive %u.\n",
                   (unsigned)slot_index, (unsigned)drive_index);
            element_map_free(&map);
            goto out;
        } else if (!target_slot_st.full) {
            fprintf(stderr, "Slot %u is empty.\n", (unsigned)slot_index);
            rc = 1;
            element_map_free(&map);
            goto out;
        }

        printf("LOAD: transport=0x%04x slot=%u(0x%04x) drive=%u(0x%04x)\n",
               transport, (unsigned)slot_index, slot_addr,
               (unsigned)drive_index, drive_addr);

        // Show current mounted disc in verbose mode
        if (g_verbose && drive_st.full) {
            char name[256] = {0};
            char size[64] = {0};
            if (get_mounted_disc_info(name, sizeof(name), size, sizeof(size))) {
                printf("  Currently mounted: %s (%s)\n", name[0] ? name : "Unknown", size[0] ? size : "?");
            }
        }

        // If drive has a different disc, unload it first
        if (drive_st.full) {
            uint16_t unload_slot_addr = 0;
            size_t unload_slot_index = 0;

            if (drive_st.valid_src) {
                unload_slot_addr = drive_st.src_addr;
                // Find the slot index for this address
                for (size_t i = 0; i < map.slots.count; i++) {
                    if (map.slots.addrs[i] == unload_slot_addr) {
                        unload_slot_index = i + 1;
                        break;
                    }
                }
            }

            if (unload_slot_addr == 0 || unload_slot_index == 0) {
                fprintf(stderr, "Drive has a disc but cannot determine source slot.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }

            printf("  Drive has disc from slot %zu(0x%04x), unloading first...\n",
                   unload_slot_index, unload_slot_addr);

            if (dry_run) {
                printf("DRY RUN: Eject from macOS\n");
                printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x (unload)\n",
                       transport, drive_addr, unload_slot_addr);
                printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x (load)\n",
                       transport, slot_addr, drive_addr);
            } else {
                if (confirm && !confirm_move()) {
                    fprintf(stderr, "Aborted.\n");
                    rc = 1;
                    element_map_free(&map);
                    goto out;
                }
                // Eject from macOS
                eject_optical_media();
                // Unload current disc
                rc = cmd_move_medium(&handle, transport, drive_addr, unload_slot_addr);
                if (rc != 0) {
                    fprintf(stderr, "Failed to unload current disc.\n");
                    element_map_free(&map);
                    goto out;
                }
                // Load requested disc
                printf("  Loading slot %u...\n", (unsigned)slot_index);
                rc = cmd_move_medium(&handle, transport, slot_addr, drive_addr);
            }
        } else {
            // Drive is empty, just load
            if (dry_run) {
                printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                       transport, slot_addr, drive_addr);
            } else {
                if (confirm && !confirm_move()) {
                    fprintf(stderr, "Aborted.\n");
                    rc = 1;
                    element_map_free(&map);
                    goto out;
                }
                rc = cmd_move_medium(&handle, transport, slot_addr, drive_addr);
            }
        }
        // Show newly mounted disc in verbose mode
        if (g_verbose && rc == 0 && !dry_run) {
            wait_and_print_mounted_disc();
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "unload") == 0 || strcmp(argv[1], "unload-drive") == 0) {
        size_t slot_index = 0, drive_index = 1; // default to drive 1
        bool have_slot = false;
        bool have_transport = false;
        uint16_t transport = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
                have_slot = parse_index(argv[++i], &slot_index);
            } else if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc) {
                parse_index(argv[++i], &drive_index);
            } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            }
        }
        if (!have_slot) {
            fprintf(stderr, "Missing --slot.\n");
            rc = 1; goto out;
        }
        ElementMap map = {0};
        rc = fetch_element_map(&handle, &map);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element map.\n");
            element_map_free(&map);
            goto out;
        }
        if (slot_index == 0 || slot_index > map.slots.count ||
            drive_index == 0 || drive_index > map.drives.count) {
            fprintf(stderr, "Slot/drive out of range. Slots: %zu, Drives: %zu\n",
                    map.slots.count, map.drives.count);
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (!have_transport) {
            if (map.transports.count == 0) {
                fprintf(stderr, "No transport element found.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            transport = map.transports.addrs[0];
        }
        uint16_t slot_addr = map.slots.addrs[slot_index - 1];
        uint16_t drive_addr = map.drives.addrs[drive_index - 1];
        printf("UNLOAD: transport=0x%04x drive=%u(0x%04x) slot=%u(0x%04x)\n",
               transport, (unsigned)drive_index, drive_addr,
               (unsigned)slot_index, slot_addr);
        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, drive_addr, slot_addr);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1; goto out;
            }
            // Eject optical media from macOS before physical unload
            eject_optical_media();
            rc = cmd_move_medium(&handle, transport, drive_addr, slot_addr);
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "eject") == 0) {
        // Eject a disc from the machine via the I/E slot
        // If the disc is currently in the drive, unload it first
        size_t slot_index = 0, drive_index = 1; // default drive 1
        bool have_slot = false, have_drive = false;
        bool have_transport = false;
        uint16_t transport = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
                have_slot = parse_index(argv[++i], &slot_index);
            } else if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc) {
                have_drive = parse_index(argv[++i], &drive_index);
            } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            }
        }
        if (!have_slot) {
            fprintf(stderr, "Missing --slot.\n");
            rc = 1; goto out;
        }
        ElementMap map = {0};
        rc = fetch_element_map(&handle, &map);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element map.\n");
            element_map_free(&map);
            goto out;
        }
        if (slot_index == 0 || slot_index > map.slots.count) {
            fprintf(stderr, "Slot out of range. Slots: %zu\n", map.slots.count);
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (!have_drive) drive_index = 1;
        if (drive_index == 0 || drive_index > map.drives.count) {
            fprintf(stderr, "Drive out of range. Drives: %zu\n", map.drives.count);
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (map.ie.count == 0) {
            fprintf(stderr, "No import/export element found.\n");
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (!have_transport) {
            if (map.transports.count == 0) {
                fprintf(stderr, "No transport element found.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            transport = map.transports.addrs[0];
        }
        uint16_t slot_addr = map.slots.addrs[slot_index - 1];
        uint16_t drive_addr = map.drives.addrs[drive_index - 1];
        uint16_t ie_addr = map.ie.addrs[0];

        // Check element status to see if disc is in slot or in drive
        ElementStatus drive_st = {0}, slot_st = {0};
        rc = read_element_status_info(&handle, drive_addr, &drive_st, slot_addr, &slot_st);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element status.\n");
            element_map_free(&map);
            goto out;
        }

        bool disc_in_drive = false;
        if (!slot_st.full && drive_st.full) {
            // Slot is empty and drive has a disc - check if it came from this slot
            if (drive_st.valid_src && drive_st.src_addr == slot_addr) {
                disc_in_drive = true;
            } else if (!drive_st.valid_src) {
                // No source info, assume disc in drive belongs to this slot if slot is empty
                disc_in_drive = true;
            }
        }

        if (!slot_st.full && !disc_in_drive) {
            fprintf(stderr, "Slot %zu is empty and disc is not in drive.\n", slot_index);
            rc = 1;
            element_map_free(&map);
            goto out;
        }

        printf("EJECT: slot=%zu(0x%04x) via ie(0x%04x)\n", slot_index, slot_addr, ie_addr);

        if (disc_in_drive) {
            printf("  Disc is currently in drive %zu(0x%04x), unloading first...\n",
                   drive_index, drive_addr);
            if (dry_run) {
                printf("DRY RUN: Eject from macOS\n");
                printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x (unload to slot)\n",
                       transport, drive_addr, slot_addr);
                printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x (eject to I/E)\n",
                       transport, slot_addr, ie_addr);
            } else {
                if (confirm && !confirm_move()) {
                    fprintf(stderr, "Aborted.\n");
                    rc = 1;
                    element_map_free(&map);
                    goto out;
                }
                // Step 1: Eject from macOS
                eject_optical_media();
                // Step 2: Unload from drive to slot
                printf("  Moving from drive to slot...\n");
                rc = cmd_move_medium(&handle, transport, drive_addr, slot_addr);
                if (rc != 0) {
                    fprintf(stderr, "Failed to unload from drive.\n");
                    element_map_free(&map);
                    goto out;
                }
                // Step 3: Move from slot to I/E
                printf("  Moving from slot to I/E...\n");
                rc = cmd_move_medium(&handle, transport, slot_addr, ie_addr);
                if (rc != 0) {
                    fprintf(stderr, "Failed to move to I/E slot.\n");
                }
            }
        } else {
            // Disc is in slot, just move to I/E
            if (dry_run) {
                printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x (eject to I/E)\n",
                       transport, slot_addr, ie_addr);
            } else {
                if (confirm && !confirm_move()) {
                    fprintf(stderr, "Aborted.\n");
                    rc = 1;
                    element_map_free(&map);
                    goto out;
                }
                printf("  Moving from slot to I/E...\n");
                rc = cmd_move_medium(&handle, transport, slot_addr, ie_addr);
                if (rc != 0) {
                    fprintf(stderr, "Failed to move to I/E slot.\n");
                }
            }
        }

        if (rc == 0) {
            printf("Disc ejected to I/E slot. You can now remove it from the changer.\n");
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "insert") == 0) {
        // Insert a disc from the IE port into a slot
        size_t slot_index = 0;
        bool have_slot = false;
        bool have_transport = false;
        uint16_t transport = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
                have_slot = parse_index(argv[++i], &slot_index);
            } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            }
        }
        if (!have_slot) {
            fprintf(stderr, "Missing --slot.\n");
            rc = 1; goto out;
        }
        ElementMap map = {0};
        rc = fetch_element_map(&handle, &map);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element map.\n");
            element_map_free(&map);
            goto out;
        }
        if (slot_index == 0 || slot_index > map.slots.count) {
            fprintf(stderr, "Slot out of range. Slots: %zu\n", map.slots.count);
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (map.ie.count == 0) {
            fprintf(stderr, "No import/export element found.\n");
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (!have_transport) {
            if (map.transports.count == 0) {
                fprintf(stderr, "No transport element found.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            transport = map.transports.addrs[0];
        }
        uint16_t slot_addr = map.slots.addrs[slot_index - 1];
        uint16_t ie_addr = map.ie.addrs[0];

        printf("INSERT: IE(0x%04x) -> slot %zu(0x%04x)\n", ie_addr, slot_index, slot_addr);
        printf("Place a disc in the IE port, then press Enter to continue...\n");
        if (!dry_run) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
        }

        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, ie_addr, slot_addr);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            rc = cmd_move_medium(&handle, transport, ie_addr, slot_addr);
            if (rc == 0) {
                printf("Disc inserted into slot %zu.\n", slot_index);
            }
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "retrieve") == 0) {
        // Retrieve a disc from a slot to the IE port
        size_t slot_index = 0;
        bool have_slot = false;
        bool have_transport = false;
        uint16_t transport = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
                have_slot = parse_index(argv[++i], &slot_index);
            } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            }
        }
        if (!have_slot) {
            fprintf(stderr, "Missing --slot.\n");
            rc = 1; goto out;
        }
        ElementMap map = {0};
        rc = fetch_element_map(&handle, &map);
        if (rc != 0) {
            fprintf(stderr, "Failed to read element map.\n");
            element_map_free(&map);
            goto out;
        }
        if (slot_index == 0 || slot_index > map.slots.count) {
            fprintf(stderr, "Slot out of range. Slots: %zu\n", map.slots.count);
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (map.ie.count == 0) {
            fprintf(stderr, "No import/export element found.\n");
            rc = 1;
            element_map_free(&map);
            goto out;
        }
        if (!have_transport) {
            if (map.transports.count == 0) {
                fprintf(stderr, "No transport element found.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            transport = map.transports.addrs[0];
        }
        uint16_t slot_addr = map.slots.addrs[slot_index - 1];
        uint16_t ie_addr = map.ie.addrs[0];

        printf("RETRIEVE: slot %zu(0x%04x) -> IE(0x%04x)\n", slot_index, slot_addr, ie_addr);

        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, slot_addr, ie_addr);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1;
                element_map_free(&map);
                goto out;
            }
            rc = cmd_move_medium(&handle, transport, slot_addr, ie_addr);
            if (rc == 0) {
                printf("Disc from slot %zu is now in the IE port. You can remove it.\n", slot_index);
            }
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "unload") == 0) {
        uint16_t transport = 0, slot = 0, drive = 0;
        bool have_transport = false, have_slot = false, have_drive = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
                have_transport = parse_u16(argv[++i], &transport);
            } else if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
                have_slot = parse_u16(argv[++i], &slot);
            } else if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc) {
                have_drive = parse_u16(argv[++i], &drive);
            }
        }
        if (!have_transport || !have_slot || !have_drive) {
            fprintf(stderr, "Missing --transport, --drive, or --slot.\n");
            rc = 1; goto out;
        }
        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, drive, slot);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1; goto out;
            }
            // Eject optical media from macOS before physical unload
            eject_optical_media();
            rc = cmd_move_medium(&handle, transport, drive, slot);
        }
    } else {
        print_usage(argv[0]);
        rc = 1;
    }

out:
    close_changer(&handle);
    return rc;
}

#endif /* MCHANGER_NO_MAIN */

/*
 * =============================================================================
 * Public API Implementation
 * =============================================================================
 */

/* Internal handle is compatible with public handle */
struct MChangerHandle {
    ChangerHandle internal;
};

/* List available changer devices */
int mchanger_list_changers(MChangerHandleInfo **out_list, size_t *out_count) {
    if (!out_list || !out_count) return MCHANGER_ERR_INVALID;

    *out_list = NULL;
    *out_count = 0;

    io_iterator_t iter = match_scsi_devices();
    if (iter == IO_OBJECT_NULL) return MCHANGER_ERR_NOT_FOUND;

    /* Count changers first */
    size_t count = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        if (is_changer_device(service)) count++;
        IOObjectRelease(service);
    }

    if (count == 0) {
        IOObjectRelease(iter);
        return MCHANGER_OK; /* No changers found, but not an error */
    }

    /* Allocate and fill */
    MChangerHandleInfo *list = calloc(count, sizeof(MChangerHandleInfo));
    if (!list) {
        IOObjectRelease(iter);
        return MCHANGER_ERR_INVALID;
    }

    IOIteratorReset(iter);
    size_t idx = 0;
    while ((service = IOIteratorNext(iter)) && idx < count) {
        if (is_changer_device(service)) {
            get_vendor_product(service, list[idx].vendor, sizeof(list[idx].vendor),
                              list[idx].product, sizeof(list[idx].product));
            io_string_t path;
            if (IORegistryEntryGetPath(service, kIOServicePlane, path) == KERN_SUCCESS) {
                strncpy(list[idx].path, path, sizeof(list[idx].path) - 1);
            }
            idx++;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);

    *out_list = list;
    *out_count = idx;
    return MCHANGER_OK;
}

void mchanger_free_changer_list(MChangerHandleInfo *list) {
    free(list);
}

/* Open a changer device */
MChangerHandle *mchanger_open(const char *device_name) {
    return mchanger_open_ex(device_name, false, false);
}

MChangerHandle *mchanger_open_ex(const char *device_name, bool force, bool skip_tur) {
    (void)device_name; /* TODO: support opening specific device by name */

    MChangerHandle *changer = calloc(1, sizeof(MChangerHandle));
    if (!changer) return NULL;

    changer->internal = open_changer(!force);
    if (!changer->internal.service && !changer->internal.sbp2_lun) {
        free(changer);
        return NULL;
    }

    if (!skip_tur && !force) {
        if (cmd_test_unit_ready(&changer->internal) != 0) {
            close_changer(&changer->internal);
            free(changer);
            return NULL;
        }
    }

    return changer;
}

void mchanger_close(MChangerHandle *changer) {
    if (!changer) return;
    close_changer(&changer->internal);
    free(changer);
}

/* Get element map */
int mchanger_get_element_map(MChangerHandle *changer, MChangerElementMap *out_map) {
    if (!changer || !out_map) return MCHANGER_ERR_INVALID;

    memset(out_map, 0, sizeof(*out_map));

    ElementMap internal_map = {0};
    int rc = fetch_element_map(&changer->internal, &internal_map);
    if (rc != 0) return MCHANGER_ERR_SCSI;

    /* Copy to public structure */
    if (internal_map.slots.count > 0) {
        out_map->slot_addrs = malloc(internal_map.slots.count * sizeof(uint16_t));
        if (out_map->slot_addrs) {
            memcpy(out_map->slot_addrs, internal_map.slots.addrs,
                   internal_map.slots.count * sizeof(uint16_t));
            out_map->slot_count = internal_map.slots.count;
        }
    }

    if (internal_map.drives.count > 0) {
        out_map->drive_addrs = malloc(internal_map.drives.count * sizeof(uint16_t));
        if (out_map->drive_addrs) {
            memcpy(out_map->drive_addrs, internal_map.drives.addrs,
                   internal_map.drives.count * sizeof(uint16_t));
            out_map->drive_count = internal_map.drives.count;
        }
    }

    if (internal_map.transports.count > 0) {
        out_map->transport_addrs = malloc(internal_map.transports.count * sizeof(uint16_t));
        if (out_map->transport_addrs) {
            memcpy(out_map->transport_addrs, internal_map.transports.addrs,
                   internal_map.transports.count * sizeof(uint16_t));
            out_map->transport_count = internal_map.transports.count;
        }
    }

    if (internal_map.ie.count > 0) {
        out_map->ie_addrs = malloc(internal_map.ie.count * sizeof(uint16_t));
        if (out_map->ie_addrs) {
            memcpy(out_map->ie_addrs, internal_map.ie.addrs,
                   internal_map.ie.count * sizeof(uint16_t));
            out_map->ie_count = internal_map.ie.count;
        }
    }

    element_map_free(&internal_map);
    return MCHANGER_OK;
}

void mchanger_free_element_map(MChangerElementMap *map) {
    if (!map) return;
    free(map->slot_addrs);
    free(map->drive_addrs);
    free(map->transport_addrs);
    free(map->ie_addrs);
    memset(map, 0, sizeof(*map));
}

/* Get element status */
int mchanger_get_slot_status(MChangerHandle *changer, int slot, MChangerElementStatus *out_status) {
    if (!changer || !out_status || slot < 1) return MCHANGER_ERR_INVALID;

    ElementMap map = {0};
    if (fetch_element_map(&changer->internal, &map) != 0) return MCHANGER_ERR_SCSI;

    if ((size_t)slot > map.slots.count) {
        element_map_free(&map);
        return MCHANGER_ERR_INVALID;
    }

    uint16_t slot_addr = map.slots.addrs[slot - 1];
    uint16_t drive_addr = map.drives.count > 0 ? map.drives.addrs[0] : 0;

    ElementStatus internal_st = {0};
    int rc = read_element_status_info(&changer->internal, drive_addr, NULL, slot_addr, &internal_st);
    element_map_free(&map);

    if (rc != 0) return MCHANGER_ERR_SCSI;

    out_status->address = internal_st.addr;
    out_status->full = internal_st.full;
    out_status->except = false; /* TODO: extract from flags */
    out_status->valid_source = internal_st.valid_src;
    out_status->source_addr = internal_st.src_addr;

    return MCHANGER_OK;
}

int mchanger_get_drive_status(MChangerHandle *changer, int drive, MChangerElementStatus *out_status) {
    if (!changer || !out_status || drive < 1) return MCHANGER_ERR_INVALID;

    ElementMap map = {0};
    if (fetch_element_map(&changer->internal, &map) != 0) return MCHANGER_ERR_SCSI;

    if ((size_t)drive > map.drives.count) {
        element_map_free(&map);
        return MCHANGER_ERR_INVALID;
    }

    uint16_t drive_addr = map.drives.addrs[drive - 1];

    ElementStatus internal_st = {0};
    int rc = read_element_status_info(&changer->internal, drive_addr, &internal_st, 0, NULL);
    element_map_free(&map);

    if (rc != 0) return MCHANGER_ERR_SCSI;

    out_status->address = internal_st.addr;
    out_status->full = internal_st.full;
    out_status->except = false;
    out_status->valid_source = internal_st.valid_src;
    out_status->source_addr = internal_st.src_addr;

    return MCHANGER_OK;
}

/* Load a disc from slot into drive */
int mchanger_load_slot(MChangerHandle *changer, int slot, int drive) {
    return mchanger_load_slot_verbose(changer, slot, drive, NULL, NULL);
}

int mchanger_load_slot_verbose(MChangerHandle *changer, int slot, int drive,
                           MChangerMountCallback callback, void *context) {
    if (!changer || slot < 1 || drive < 1) return MCHANGER_ERR_INVALID;

    ElementMap map = {0};
    if (fetch_element_map(&changer->internal, &map) != 0) return MCHANGER_ERR_SCSI;

    if ((size_t)slot > map.slots.count || (size_t)drive > map.drives.count) {
        element_map_free(&map);
        return MCHANGER_ERR_INVALID;
    }

    uint16_t transport = map.transports.count > 0 ? map.transports.addrs[0] : 0;
    uint16_t slot_addr = map.slots.addrs[slot - 1];
    uint16_t drive_addr = map.drives.addrs[drive - 1];

    /* Check current status */
    ElementStatus drive_st = {0}, slot_st = {0};
    if (read_element_status_info(&changer->internal, drive_addr, &drive_st, slot_addr, &slot_st) != 0) {
        element_map_free(&map);
        return MCHANGER_ERR_SCSI;
    }

    /* Already loaded? */
    if (!slot_st.full && drive_st.full && drive_st.valid_src && drive_st.src_addr == slot_addr) {
        element_map_free(&map);
        return MCHANGER_OK;
    }

    /* Slot empty and disc not in drive? */
    if (!slot_st.full && !(drive_st.full && drive_st.valid_src && drive_st.src_addr == slot_addr)) {
        element_map_free(&map);
        return MCHANGER_ERR_EMPTY;
    }

    int rc = 0;

    /* If drive has a different disc, unload it first */
    if (drive_st.full) {
        uint16_t unload_addr = drive_st.valid_src ? drive_st.src_addr : slot_addr;
        eject_optical_media();
        rc = cmd_move_medium(&changer->internal, transport, drive_addr, unload_addr);
        if (rc != 0) {
            element_map_free(&map);
            return MCHANGER_ERR_SCSI;
        }
    }

    /* Load the disc */
    rc = cmd_move_medium(&changer->internal, transport, slot_addr, drive_addr);
    element_map_free(&map);

    if (rc != 0) return MCHANGER_ERR_SCSI;

    /* Notify about mounted disc if callback provided */
    if (callback) {
        char name[256] = {0}, size[64] = {0};
        mchanger_wait_for_mount(name, sizeof(name), size, sizeof(size), 30);
        callback(name[0] ? name : "Unknown", size[0] ? size : "?", context);
    }

    return MCHANGER_OK;
}

/* Unload the drive to a specific slot */
int mchanger_unload_drive(MChangerHandle *changer, int slot, int drive) {
    if (!changer || slot < 1 || drive < 1) return MCHANGER_ERR_INVALID;

    ElementMap map = {0};
    if (fetch_element_map(&changer->internal, &map) != 0) return MCHANGER_ERR_SCSI;

    if ((size_t)slot > map.slots.count || (size_t)drive > map.drives.count) {
        element_map_free(&map);
        return MCHANGER_ERR_INVALID;
    }

    uint16_t transport = map.transports.count > 0 ? map.transports.addrs[0] : 0;
    uint16_t slot_addr = map.slots.addrs[slot - 1];
    uint16_t drive_addr = map.drives.addrs[drive - 1];

    eject_optical_media();
    int rc = cmd_move_medium(&changer->internal, transport, drive_addr, slot_addr);
    element_map_free(&map);

    return rc == 0 ? MCHANGER_OK : MCHANGER_ERR_SCSI;
}

/* Eject a disc to the import/export slot */
int mchanger_eject(MChangerHandle *changer, int slot, int drive) {
    if (!changer || slot < 1 || drive < 1) return MCHANGER_ERR_INVALID;

    ElementMap map = {0};
    if (fetch_element_map(&changer->internal, &map) != 0) return MCHANGER_ERR_SCSI;

    if ((size_t)slot > map.slots.count || (size_t)drive > map.drives.count || map.ie.count == 0) {
        element_map_free(&map);
        return MCHANGER_ERR_INVALID;
    }

    uint16_t transport = map.transports.count > 0 ? map.transports.addrs[0] : 0;
    uint16_t slot_addr = map.slots.addrs[slot - 1];
    uint16_t drive_addr = map.drives.addrs[drive - 1];
    uint16_t ie_addr = map.ie.addrs[0];

    /* Check if disc is in drive */
    ElementStatus drive_st = {0}, slot_st = {0};
    if (read_element_status_info(&changer->internal, drive_addr, &drive_st, slot_addr, &slot_st) != 0) {
        element_map_free(&map);
        return MCHANGER_ERR_SCSI;
    }

    int rc = 0;

    /* If disc is in drive, unload to slot first */
    if (!slot_st.full && drive_st.full) {
        eject_optical_media();
        rc = cmd_move_medium(&changer->internal, transport, drive_addr, slot_addr);
        if (rc != 0) {
            element_map_free(&map);
            return MCHANGER_ERR_SCSI;
        }
    }

    /* Move from slot to I/E */
    rc = cmd_move_medium(&changer->internal, transport, slot_addr, ie_addr);
    element_map_free(&map);

    return rc == 0 ? MCHANGER_OK : MCHANGER_ERR_SCSI;
}

/* Low-level move medium */
int mchanger_move_medium(MChangerHandle *changer, uint16_t transport, uint16_t source, uint16_t dest) {
    if (!changer) return MCHANGER_ERR_INVALID;
    return cmd_move_medium(&changer->internal, transport, source, dest) == 0 ? MCHANGER_OK : MCHANGER_ERR_SCSI;
}

/* Eject from macOS */
int mchanger_eject_from_macos(void) {
    return eject_optical_media();
}

/* Wait for mount */
int mchanger_wait_for_mount(char *out_name, size_t name_len, char *out_size, size_t size_len, int timeout_secs) {
    if (out_name && name_len > 0) out_name[0] = '\0';
    if (out_size && size_len > 0) out_size[0] = '\0';

    /* First check if already mounted */
    if (get_mounted_disc_info(out_name, name_len, out_size, size_len)) {
        return MCHANGER_OK;
    }

    /* Set up DiskArbitration session */
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return MCHANGER_ERR_INVALID;

    DACallbackContext ctx = {0};
    bool timed_out = false;

    DARegisterDiskAppearedCallback(session, NULL, disk_appeared_callback, &ctx);
    DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    CFRunLoopTimerContext timerCtx = { 0, &timed_out, NULL, NULL, NULL };
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + (double)timeout_secs, 0, 0, 0, timeout_callback, &timerCtx);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    CFRunLoopRun();

    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
    DAUnregisterCallback(session, disk_appeared_callback, &ctx);
    DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRelease(session);

    if (ctx.found) {
        if (out_name && name_len > 0) strncpy(out_name, ctx.name, name_len - 1);
        if (out_size && size_len > 0) strncpy(out_size, ctx.size, size_len - 1);
        return MCHANGER_OK;
    }

    return timed_out ? MCHANGER_ERR_BUSY : MCHANGER_ERR_NOT_FOUND;
}

/* Device info */
int mchanger_inquiry(MChangerHandle *changer, char *vendor, size_t vendor_len,
                 char *product, size_t product_len, char *revision, size_t revision_len) {
    if (!changer) return MCHANGER_ERR_INVALID;

    uint8_t cdb[6] = {0x12, 0, 0, 0, 96, 0}; /* INQUIRY */
    uint8_t buf[96] = {0};

    int rc = execute_cdb(&changer->internal, cdb, sizeof(cdb), buf, sizeof(buf),
                         kSCSIDataTransfer_FromTargetToInitiator, 10000);
    if (rc != 0) return MCHANGER_ERR_SCSI;

    if (vendor && vendor_len > 0) {
        size_t len = vendor_len - 1 < 8 ? vendor_len - 1 : 8;
        memcpy(vendor, buf + 8, len);
        vendor[len] = '\0';
        /* Trim trailing spaces */
        while (len > 0 && vendor[len - 1] == ' ') vendor[--len] = '\0';
    }

    if (product && product_len > 0) {
        size_t len = product_len - 1 < 16 ? product_len - 1 : 16;
        memcpy(product, buf + 16, len);
        product[len] = '\0';
        while (len > 0 && product[len - 1] == ' ') product[--len] = '\0';
    }

    if (revision && revision_len > 0) {
        size_t len = revision_len - 1 < 4 ? revision_len - 1 : 4;
        memcpy(revision, buf + 32, len);
        revision[len] = '\0';
        while (len > 0 && revision[len - 1] == ' ') revision[--len] = '\0';
    }

    return MCHANGER_OK;
}

int mchanger_test_unit_ready(MChangerHandle *changer) {
    if (!changer) return MCHANGER_ERR_INVALID;
    return cmd_test_unit_ready(&changer->internal) == 0 ? MCHANGER_OK : MCHANGER_ERR_SCSI;
}
