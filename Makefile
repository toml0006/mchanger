# mchanger - SCSI Media Changer Library and CLI
#
# Build targets:
#   make          - Build the CLI tool
#   make lib      - Build the static library
#   make test     - Run library tests
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
test_mchanger: test_mchanger.c libmchanger.a mchanger.h
	$(CC) $(CFLAGS) -o $@ test_mchanger.c -L. -lmchanger $(FRAMEWORKS)

# Run tests
test: test_mchanger
	./test_mchanger

# Clean build artifacts
clean:
	rm -f mchanger mchanger.o libmchanger.a test_mchanger

.PHONY: lib test clean
