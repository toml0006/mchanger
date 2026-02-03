# xl1b_changer

Tools to interact with the Sony XL1B changer attached to this machine.

## Build

```sh
cc -Wall -Wextra -O2 -o xl1b_changer xl1b_changer.c \
  -framework CoreFoundation \
  -framework IOKit
```

## Usage

```sh
./xl1b_changer list
./xl1b_changer inquiry
./xl1b_changer list-map
./xl1b_changer load-slot --slot 1 --drive 1 --dry-run
```

## Notes

- Use `--confirm` for interactive confirmation before moving media.
- Use `--force` to bypass device ID and TUR checks.
- Use `--debug` to print IORegistry details for troubleshooting.
