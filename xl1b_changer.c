#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOTypes.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSITask.h>
#include <IOKit/scsi/SCSICmds_REQUEST_SENSE_Defs.h>
#include <IOKit/sbp2/IOFireWireSBP2Lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        "  %s mode-sense-element\n"
        "  %s probe-storage\n"
        "  %s init-status\n"
        "  %s read-element-status --element-type <all|transport|storage|ie|drive>\n"
        "                           --start <addr> --count <n> --alloc <bytes> [--raw]\n"
        "  %s move --transport <addr> --source <addr> --dest <addr>\n"
        "  %s list-map\n"
        "  %s sanity-check\n"
        "  %s load-slot --slot <n> --drive <n> [--transport <addr>]\n"
        "  %s unload-drive --drive <n> --slot <n> [--transport <addr>]\n"
        "  %s load --transport <addr> --slot <addr> --drive <addr>\n"
        "  %s unload --transport <addr> --drive <addr> --slot <addr>\n"
        "\n"
        "Notes:\n"
        "- Addresses are element addresses from READ ELEMENT STATUS.\n"
        "- Use --force to bypass device ID and TUR checks.\n"
        "- Use --no-tur to skip the automatic TEST UNIT READY check.\n"
        "- Use --dry-run to show resolved element addresses without moving media.\n"
        "- Use --confirm to require interactive confirmation before moving media.\n"
        "- Use --debug to print IORegistry details for troubleshooting.\n"
        "- This is a stub: it sends SMC commands but does not fully parse responses.\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0
    );
}

static bool g_debug = false;

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

static bool get_child_entry_id_with_property(io_registry_entry_t parent, CFStringRef key, uint64_t *entry_id_out) {
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(parent, kIOServicePlane, &iter) != KERN_SUCCESS) {
        return false;
    }
    io_registry_entry_t child;
    while ((child = IOIteratorNext(iter))) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(child, key, kCFAllocatorDefault, 0);
        if (value) {
            if (entry_id_out) {
                IORegistryEntryGetRegistryEntryID(child, entry_id_out);
            }
            CFRelease(value);
            IOObjectRelease(child);
            IOObjectRelease(iter);
            return true;
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(iter);
    return false;
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
    bool full = (flags & 0x20) != 0;
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
    uint32_t alloc = 4096;
    uint8_t cdb[12] = {0};
    cdb[0] = 0xB8; // READ ELEMENT STATUS
    cdb[1] = 0x00; // all element types
    cdb[4] = 0xFF; // request all available elements
    cdb[5] = 0xFF;
    cdb[6] = (alloc >> 16) & 0xFF;
    cdb[7] = (alloc >> 8) & 0xFF;
    cdb[8] = alloc & 0xFF;

    uint8_t *buf = calloc(1, alloc);
    if (!buf) return 1;
    int rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 30000);
    if (rc != 0) {
        free(buf);
        return rc;
    }

    uint32_t report_bytes = (buf[5] << 16) | (buf[6] << 8) | buf[7];
    if (report_bytes == 0) {
        free(buf);
        return 1;
    }
    uint32_t needed = report_bytes + 8;
    if (needed > alloc && needed < 65535) {
        free(buf);
        alloc = needed;
        buf = calloc(1, alloc);
        if (!buf) return 1;
        cdb[6] = (alloc >> 16) & 0xFF;
        cdb[7] = (alloc >> 8) & 0xFF;
        cdb[8] = alloc & 0xFF;
        rc = execute_cdb(handle, cdb, sizeof(cdb), buf, alloc, kSCSIDataTransfer_FromTargetToInitiator, 30000);
        if (rc != 0) {
            free(buf);
            return rc;
        }
    }

    uint32_t parse_len = (report_bytes + 8 <= alloc) ? report_bytes + 8 : alloc;
    bool ok = parse_element_status_map(buf, parse_len, map);
    free(buf);
    return ok ? 0 : 1;
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
        } else {
            fprintf(stderr, "Failed to read element map.\n");
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "load-slot") == 0) {
        size_t slot_index = 0, drive_index = 0;
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
        if (!have_slot || !have_drive) {
            fprintf(stderr, "Missing --slot or --drive.\n");
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
        printf("LOAD: transport=0x%04x slot=%u(0x%04x) drive=%u(0x%04x)\n",
               transport, (unsigned)slot_index, slot_addr,
               (unsigned)drive_index, drive_addr);
        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, slot_addr, drive_addr);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1; goto out;
            }
            rc = cmd_move_medium(&handle, transport, slot_addr, drive_addr);
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "unload-drive") == 0) {
        size_t slot_index = 0, drive_index = 0;
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
        if (!have_slot || !have_drive) {
            fprintf(stderr, "Missing --slot or --drive.\n");
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
            rc = cmd_move_medium(&handle, transport, drive_addr, slot_addr);
        }
        element_map_free(&map);
    } else if (strcmp(argv[1], "load") == 0) {
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
            fprintf(stderr, "Missing --transport, --slot, or --drive.\n");
            rc = 1; goto out;
        }
        if (dry_run) {
            printf("DRY RUN: MOVE transport=0x%04x source=0x%04x dest=0x%04x\n",
                   transport, slot, drive);
        } else {
            if (confirm && !confirm_move()) {
                fprintf(stderr, "Aborted.\n");
                rc = 1; goto out;
            }
            rc = cmd_move_medium(&handle, transport, slot, drive);
        }
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
