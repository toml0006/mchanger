# mchanger - SCSI Media Changer Library and CLI
#
# Build targets:
#   make          - Build the CLI tool
#   make lib      - Build the static library
#   make test     - Run software tests; hardware tests skip if disconnected
#   make qualification - Require a connected 200-slot changer (read-only)
#   make motion-test SLOT=1 - Load and return one known occupied slot
#   make clean    - Remove build artifacts

CC = cc
CFLAGS = -Wall -Wextra -O2
FRAMEWORKS = -framework CoreFoundation -framework IOKit -framework DiskArbitration

# CLI tool (default target)
mchanger: mchanger.c mchanger.h
	$(CC) $(CFLAGS) -o $@ mchanger.c $(FRAMEWORKS)

# Static library (for use by other applications)
lib: libmchanger.a

libmchanger.a: mchanger.c mchanger.h
	$(CC) $(CFLAGS) -DMCHANGER_NO_MAIN -c mchanger.c -o mchanger.o
	ar rcs $@ mchanger.o
	rm -f mchanger.o

# Test binary
test_mchanger: test_mchanger.c mchanger.c mchanger.h
	$(CC) $(CFLAGS) -DMCHANGER_NO_MAIN -DMCHANGER_TESTING -o $@ test_mchanger.c mchanger.c $(FRAMEWORKS)

# Run tests
test: test_mchanger
	./test_mchanger

# Read-only hardware qualification. Override EXPECTED_SLOTS for another model.
EXPECTED_SLOTS ?= 200
qualification: test_mchanger
	./test_mchanger --require-hardware --expect-slots $(EXPECTED_SLOTS)

# Explicitly opt in to moving one known occupied slot into drive 1 and back.
motion-test: test_mchanger
	@test -n "$(SLOT)" || (echo "Usage: make motion-test SLOT=<known-occupied-slot>" && exit 2)
	./test_mchanger --require-hardware --expect-slots $(EXPECTED_SLOTS) --motion-slot $(SLOT)

# Clean build artifacts
clean:
	rm -f mchanger mchanger.o libmchanger.a test_mchanger
	rm -rf test_mchanger.dSYM

.PHONY: lib test qualification motion-test clean
