# xl1b_changer - SCSI Media Changer Library and CLI
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
xl1b_changer: xl1b_changer.c xl1b_changer.h
	$(CC) $(CFLAGS) -o $@ xl1b_changer.c $(FRAMEWORKS)

# Static library (for use by other applications)
lib: libxl1b_changer.a

libxl1b_changer.a: xl1b_changer.c xl1b_changer.h
	$(CC) $(CFLAGS) -DXL1B_NO_MAIN -c xl1b_changer.c -o xl1b_changer.o
	ar rcs $@ xl1b_changer.o
	rm -f xl1b_changer.o

# Test binary
test_xl1b_changer: test_xl1b_changer.c libxl1b_changer.a xl1b_changer.h
	$(CC) $(CFLAGS) -o $@ test_xl1b_changer.c -L. -lxl1b_changer $(FRAMEWORKS)

# Run tests
test: test_xl1b_changer
	./test_xl1b_changer

# Clean build artifacts
clean:
	rm -f xl1b_changer xl1b_changer.o libxl1b_changer.a test_xl1b_changer

.PHONY: lib test clean
