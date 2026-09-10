# mchanger

A command-line tool to control SCSI media changer devices (jukeboxes/autoloaders) on macOS. Originally developed for the [Sony VGP-XL1B](https://www.sony.com/electronics/support/home-video-media-changers/vgp-xl1b) 200-disc changer, a device released at the height of the [Windows Media Center](https://en.wikipedia.org/wiki/Windows_Media_Center) craze in the mid 2000s.

Should work with other SCSI-compliant media changers, assuming you can find one.

## Hardware qualification

The library includes a layered test program. The default suite is safe to run
without a changer and never moves media:

```bash
make test
```

With a changer connected, run the read-only qualification suite. It requires
successful discovery, INQUIRY, TEST UNIT READY, a coherent 200-slot element
map, repeatable bulk inventory, and agreement between bulk and individual
status reads:

```bash
make qualification
```

For another changer model, override the expected slot count:

```bash
make qualification EXPECTED_SLOTS=100
```

Mechanical movement is a separate, explicit test. Choose a slot that is known
to contain a non-valuable test disc and make sure drive 1 is empty. The test
refuses to run otherwise. It loads the disc, verifies the reported source and
element state, then returns the disc to the same slot:

```bash
make motion-test SLOT=1
```

The test does not exercise the import/export slot or leave a disc intentionally
loaded. If a post-load check fails, it still attempts to return the disc before
reporting the failure.

These targets currently exercise the macOS IOKit/FireWire backend. The test
program itself calls only the public `mchanger` API, so the same behavioral
qualification can be reused on Raspberry Pi after adding the Linux `sg`/SBP-2
transport backend. The existing library cannot yet be built on Raspberry Pi.

## Features

- List and detect media changer devices (including FireWire SBP2 devices)
- **Insert** discs from the IE port into storage slots
- **Retrieve** discs from storage slots to the IE port for removal
- **Load** discs from storage slots into the drive for playback
- **Unload** discs from the drive back to storage slots
- **Eject** discs directly from drive to IE port (combined unload + retrieve)
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

## Installation

### Homebrew

```sh
brew install toml0006/mchanger/mchanger
```

### Pre-built binary

Download the latest universal binary from [Releases](https://github.com/toml0006/mchanger/releases).

### Build from source

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

#### Library

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

### Insert a disc into the machine

```sh
./mchanger insert --slot 100               # Insert disc from IE port into slot 100
./mchanger insert --slot 50 --dry-run      # Show what would happen
```

The command first verifies that the destination is empty. On PowerFile/Sony
changers, the `MOVE MEDIUM` request opens the front gate and waits; insert one
disc when the gate opens. The changer then stores it in the selected slot.

Applications can invoke the same guarded operation through
`mchanger_import_slot()`. Use `mchanger_export_slot()` for the reverse path.

### Retrieve a disc from the machine

```sh
./mchanger retrieve --slot 100             # Move disc from slot 100 to IE port
```

Moves a disc from a storage slot to the IE port so you can physically remove it.

### Load a disc into the drive

```sh
./mchanger load --slot 1                   # Load slot 1 into drive
./mchanger load --slot 2 -v                # Load slot 2 with verbose output
./mchanger load --slot 1 --dry-run         # Show what would happen
```

If a disc is already in the drive, it will automatically be unloaded to its original slot first.

### Unload the drive

```sh
./mchanger unload --slot 1                 # Unload drive to slot 1
```

### Eject a disc from the machine

```sh
./mchanger eject --slot 1                  # Eject disc from slot 1 to IE port
```

If the disc is currently in the drive, it will be unloaded first, then moved to the IE port. This is a convenience command that combines `unload` + `retrieve`.

### Device information

```sh
./mchanger inquiry                         # Show device inquiry data
./mchanger test-unit-ready                 # Check if device is ready
./mchanger mode-sense-element              # Show element address assignment
./mchanger read-element-status --element-type all --start 0 --count 50 --alloc 4096
```

### Experimental import/export gate probe

SMC-3 defines the optional `OPEN/CLOSE IMPORT/EXPORT ELEMENT` command. The
probe requires an explicit confirmation, resolves the selected I/E element
from the changer's own element map, prints the complete CDB, and submits it
exactly once:

```sh
./mchanger probe-open-ie --ie 1 --confirm --no-tur
```

This command is useful for qualifying other changers; it is not the PowerFile
bulk-load mechanism. The PowerFile R200DL firmware revision 06A rejects opcode
`0x1B` with `ILLEGAL REQUEST` / `INVALID COMMAND OPERATION CODE` (`05/20/00`).

Archived PowerFile technical-support instructions document the actual software
sequence as a normal `MOVE MEDIUM` from the mail slot to an empty storage slot
(their example is `mm 1 4`). That move opens the gate and waits for insertion.
Repeating the operation for selected empty slots implements bulk load; repeating
the reverse storage-to-mail-slot move implements bulk unload. Element addresses
must always come from the device's reported map rather than being hard-coded.

This behavior was verified on a Sony VGP-XL1B chassis on 2026-08-18. Its
internal changer LUN reports `PowrFile PowerFile R200DL`, revision `06A`, with
transport `0x0000`, I/E `0x0001`, and slot 3 `0x0006`. Moving I/E to slot 3
opened the gate, accepted a disc, and left slot 3 full. Letting the gate wait
without inserting a disc returned sense `06/53/00` (`MEDIA LOAD OR EJECT
FAILED`); inventory stayed unchanged and the changer remained responsive.
Applications should present that result as an insertion timeout/cancellation,
not as a request to power-cycle the changer.

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
- `OPEN/CLOSE IMPORT/EXPORT ELEMENT` (0x1B) - Optional gate access; never retried
- Standard SCSI commands (INQUIRY, TEST UNIT READY, etc.)

For FireWire devices, it uses the IOFireWireSBP2 interface to send SCSI commands over the Serial Bus Protocol.

## Supported Connections

**Currently implemented:**
- FireWire (IEEE 1394) via IOFireWireSBP2

**Theoretically supported** (contributions welcome):
- USB Mass Storage (if the changer exposes SCSI commands)
- SAS (Serial Attached SCSI)
- Thunderbolt via SAS adapters
- Fibre Channel
- iSCSI

The SCSI commands are transport-agnostic—the same READ ELEMENT STATUS and MOVE MEDIUM commands work regardless of how the device is connected. Only the device discovery layer is currently FireWire-specific. Adding support for other transports would require extending `mchanger_list_changers()` to search additional IOKit device classes.

## License

MIT License - See LICENSE file for details.
