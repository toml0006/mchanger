# mchanger

A command-line tool to control SCSI media changer devices (jukeboxes/autoloaders) on macOS. Originally developed for the [Sony VGP-XL1B](https://www.sony.com/electronics/support/home-video-media-changers/vgp-xl1b) 200-disc changer, a device released at the height of the [Windows Media Center](https://en.wikipedia.org/wiki/Windows_Media_Center) craze in the mid 2000s.

Should work with other SCSI-compliant media changers, assuming you can find one.

## Features

- List and detect media changer devices (including FireWire SBP2 devices)
- Load discs from storage slots into the drive
- Unload discs from the drive back to storage slots
- Eject discs to the import/export slot for physical removal
- Automatic disc swapping (unloads current disc before loading a new one)
- Automatic macOS disc ejection before physical media moves
- Verbose mode shows mounted disc names and sizes

## Requirements

- macOS 10.4 (Tiger) through macOS 15 (Sequoia)
  - Tested on macOS 10.15 (Catalina) through macOS 15 (Sequoia)
  - Uses CoreFoundation, IOKit, and DiskArbitration frameworks
- A SCSI media changer device connected via:
  - FireWire (built-in port or Thunderbolt-to-FireWire adapter)
  - Other SCSI interfaces supported by macOS

> **Important:** macOS 16 (Tahoe) [removed FireWire support entirely](https://tidbits.com/2025/09/19/support-for-firewire-removed-from-macos-26-tahoe/). If your changer connects via FireWire, you must use macOS 15 (Sequoia) or earlier. Plan your OS upgrades accordingly, or don't!

## Build

### CLI Tool

```sh
make
```

Or manually:

```sh
cc -Wall -Wextra -O2 -o mchanger mchanger.c \
  -framework CoreFoundation \
  -framework IOKit \
  -framework DiskArbitration
```

### Library

To build as a static library (for use in other applications):

```sh
make lib
```

This creates `libmchanger.a`. Link against it and include `mchanger.h`:

```c
#include "mchanger.h"

// Open the changer
MChangerHandle *changer = mchanger_open(NULL);

// Load slot 1 into the drive
mchanger_load_slot(changer, 1, 1);

// Close when done
mchanger_close(changer);
```

Link with:
```sh
cc -o myapp myapp.c -L. -lmchanger \
  -framework CoreFoundation -framework IOKit -framework DiskArbitration
```

## Usage

### List available changers

```sh
./mchanger list          # List changers with brief info
./mchanger list-all      # List all changers including non-standard
./mchanger list-map      # Show element addresses (slots, drives, etc.)
```

### Load a disc into the drive

```sh
./mchanger load-slot --slot 1              # Load slot 1 into drive
./mchanger load-slot --slot 2 -v           # Load slot 2 with verbose output
./mchanger load-slot --slot 1 --dry-run    # Show what would happen
```

If a disc is already in the drive, it will automatically be unloaded to its original slot first.

### Unload the drive

```sh
./mchanger unload-drive --slot 1           # Unload drive to slot 1
```

### Eject a disc from the machine

```sh
./mchanger eject --slot 1                  # Eject disc from slot 1 to I/E slot
```

If the disc is currently in the drive, it will be unloaded first, then moved to the import/export slot where you can physically remove it. Yes, you do have to walk over to the machine for this part.

### Device information

```sh
./mchanger inquiry                         # Show device inquiry data
./mchanger test-unit-ready                 # Check if device is ready
./mchanger mode-sense-element              # Show element address assignment
./mchanger read-element-status --element-type all --start 0 --count 50 --alloc 4096
```

## Options

| Option | Description |
|--------|-------------|
| `--slot <n>` | Slot number (1-based) |
| `--drive <n>` | Drive number (default: 1) |
| `--transport <addr>` | Transport element address (usually auto-detected) |
| `--dry-run` | Show what would happen without moving media |
| `--confirm` | Require interactive confirmation before moves |
| `--force` | Bypass device ID and TEST UNIT READY checks |
| `--no-tur` | Skip TEST UNIT READY check |
| `--verbose`, `-v` | Show mounted disc info during operations |
| `--debug` | Print IORegistry details for troubleshooting |

## How It Works

The tool communicates with the media changer using SCSI Media Changer (SMC) commands:

- `READ ELEMENT STATUS` (0xB8) - Query status of slots, drives, and transport
- `MOVE MEDIUM` (0xA5) - Move media between elements
- `MODE SENSE` (0x1A) - Get element address assignments
- Standard SCSI commands (INQUIRY, TEST UNIT READY, etc.)

For FireWire devices, it uses the IOFireWireSBP2 interface to send SCSI commands over the Serial Bus Protocol.

## License

MIT License - See LICENSE file for details.
