#!/bin/bash

# End-to-end hardware test: slot -> drive -> raw BIN/CUE -> slot.

set -u
set -o pipefail

TEST_DIR="$HOME/discbot-hardware-tests"
CLI="$TEST_DIR/mchanger-cli-x86_64"
READER="$TEST_DIR/test-cd-reader-x86_64"
SLOT="${SLOT:-1}"
OUTPUT_DIR="$TEST_DIR/rip-output"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
OUTPUT_BASE="$OUTPUT_DIR/audio-slot${SLOT}-$RUN_ID"
LOG_FILE="$OUTPUT_DIR/audio-slot${SLOT}-$RUN_ID.log"
loaded=0
bsd_name=""

element_state() {
    "$CLI" read-element-status --element-type all --start 0 \
        --count 65535 --alloc 4096 2>&1
}

media_is_restored() {
    local state
    state="$(element_state)" || return 1
    printf '%s\n' "$state" | grep -Eq \
        'Element 0x0002 \(drive 1\).*full=0' || return 1
    printf '%s\n' "$state" | grep -Eq \
        'Element 0x0004 \(slot 1\).*full=1' || return 1
}

restore_disc() {
    local result=0
    if [[ "$loaded" -eq 0 ]]; then return 0; fi

    printf '\nReturning the disc to slot %s...\n' "$SLOT"
    if [[ -n "$bsd_name" ]]; then
        diskutil eject "/dev/$bsd_name" >/dev/null 2>&1 || true
    fi
    "$CLI" unload --slot "$SLOT" --drive 1 || true

    for attempt in 1 2 3; do
        sleep 3
        if media_is_restored; then
            printf 'Verified: drive empty and slot %s occupied.\n' "$SLOT"
            loaded=0
            return 0
        fi
        if [[ "$attempt" -lt 3 ]]; then
            printf 'Return state not settled; retrying unload (%s/3)...\n' "$attempt"
            "$CLI" unload --slot "$SLOT" --drive 1 || true
        fi
    done

    printf 'ERROR: Could not verify that the disc returned to slot %s. Inspect the changer.\n' \
        "$SLOT" >&2
    result=1
    return "$result"
}

finish() {
    local test_status=$?
    trap - EXIT HUP INT TERM
    restore_disc
    local restore_status=$?
    if [[ "$restore_status" -ne 0 ]]; then exit 2; fi
    exit "$test_status"
}

trap finish EXIT HUP INT TERM

mkdir -p "$OUTPUT_DIR"
chmod 700 "$OUTPUT_DIR"
if [[ ! -x "$CLI" || ! -x "$READER" ]]; then
    printf 'Missing hardware-test executable in %s\n' "$TEST_DIR" >&2
    exit 2
fi

printf 'RIPT audio hardware test\n'
printf '========================\n'
printf 'This will load slot %s, read every raw CD sector, and return the disc.\n' "$SLOT"
printf 'Administrator authorization is needed only to read the root:operator device.\n\n'
sudo -v || exit 2

printf 'Loading slot %s...\n' "$SLOT"
"$CLI" load --slot "$SLOT"
loaded=1

printf 'Waiting for macOS optical-media discovery...\n'
for attempt in $(jot 60 1); do
    bsd_name="$(drutil status 2>/dev/null | sed -n 's|.*Name: /dev/\([^ ]*\).*|\1|p' | head -1)"
    if [[ -n "$bsd_name" && -e "/dev/$bsd_name" ]]; then break; fi
    sleep 1
done
if [[ -z "$bsd_name" || ! -e "/dev/$bsd_name" ]]; then
    printf 'Timed out waiting for the loaded optical disc.\n' >&2
    exit 1
fi

printf 'Raw device: /dev/%s\n' "$bsd_name"
printf 'Output base: %s\n' "$OUTPUT_BASE"
printf 'Full raw-sector log: %s\n\n' "$LOG_FILE"

sudo "$READER" "$bsd_name" "$OUTPUT_BASE" 2>&1 | tee "$LOG_FILE"
rip_status=${PIPESTATUS[0]}
if [[ "$rip_status" -ne 0 ]]; then exit "$rip_status"; fi

sudo chown "$(id -u):$(id -g)" \
    "$OUTPUT_BASE.bin" "$OUTPUT_BASE.cue"
chmod 600 "$OUTPUT_BASE.bin" "$OUTPUT_BASE.cue" "$LOG_FILE"

expected_bytes=$(awk '/^TOC:/{for (i=1;i<=NF;i++) if ($i=="raw") print $(i-1)*2352}' "$LOG_FILE")
actual_bytes=$(stat -f %z "$OUTPUT_BASE.bin")
if [[ -z "$expected_bytes" || "$actual_bytes" -ne "$expected_bytes" ]]; then
    printf 'Image-size verification failed: expected=%s actual=%s\n' \
        "${expected_bytes:-unknown}" "$actual_bytes" >&2
    exit 1
fi

printf '\nSHA-256:\n'
shasum -a 256 "$OUTPUT_BASE.bin" | tee "$OUTPUT_BASE.sha256"
chmod 600 "$OUTPUT_BASE.sha256"
printf '\nCUE sheet:\n'
cat "$OUTPUT_BASE.cue"
printf '\nRaw image size verified: %s bytes.\n' "$actual_bytes"
