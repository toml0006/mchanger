/* Hardware qualification for Discbot/Bridging/CDReader.c. */

#include "../Discbot/Bridging/CDReader.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void cue_time(FILE *file, uint32_t sectors) {
    fprintf(file, "%02u:%02u:%02u",
            sectors / (75 * 60), (sectors / 75) % 60, sectors % 75);
}

static int read_chunk(DiscBotCDReader *reader, uint32_t lba, uint32_t count,
                      uint8_t *buffer, char *error, size_t error_length) {
    uint32_t completed = 0;
    if (discbot_cd_reader_read(reader, lba, count, buffer, &completed,
                               error, error_length) == 0 && completed == count) {
        return 0;
    }

    /* Match RIPT's recovery behavior: retry a rejected multi-sector request
       as individual sectors before declaring the disc unreadable. */
    if (count == 1) return -1;
    for (uint32_t index = 0; index < count; index++) {
        completed = 0;
        if (discbot_cd_reader_read(
                reader, lba + index, 1,
                buffer + (size_t)index * DISCBOT_CD_SECTOR_SIZE,
                &completed, error, error_length) != 0 || completed != 1) {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s <bsd-name> <output-base>\n"
                "       %s --probe <bsd-name>\n",
                argv[0], argv[0]);
        return 2;
    }

    int probe_only = strcmp(argv[1], "--probe") == 0;
    const char *bsd_name = probe_only ? argv[2] : argv[1];

    char error[512] = {0};
    DiscBotCDReader *reader = discbot_cd_reader_open(
        bsd_name, error, sizeof(error));
    if (!reader) {
        fprintf(stderr, "Open failed: %s\n", error);
        return 1;
    }

    uint32_t track_count = discbot_cd_reader_track_count(reader);
    uint32_t leadout = discbot_cd_reader_leadout_lba(reader);
    uint32_t first_lba = track_count > 0
        ? discbot_cd_reader_track_start_lba(reader, 0) : 0;
    if (track_count == 0 || leadout <= first_lba) {
        fprintf(stderr, "Invalid CD layout\n");
        discbot_cd_reader_close(reader);
        return 1;
    }

    uint8_t first_session = discbot_cd_reader_track_session(reader, 0);
    printf("TOC: %u tracks, first LBA %u, lead-out %u, %u raw sectors\n",
           track_count, first_lba, leadout, leadout - first_lba);
    for (uint32_t index = 0; index < track_count; index++) {
        uint8_t session = discbot_cd_reader_track_session(reader, index);
        uint8_t control = discbot_cd_reader_track_control(reader, index);
        if (session != first_session) {
            fprintf(stderr, "Multiple sessions are not supported by BIN/CUE output\n");
            discbot_cd_reader_close(reader);
            return 1;
        }
        printf("  Track %02u: session %u, LBA %u, %s, control 0x%02x\n",
               discbot_cd_reader_track_number(reader, index), session,
               discbot_cd_reader_track_start_lba(reader, index),
               (control & 0x04) ? "DATA" : "AUDIO", control);
    }

    if (probe_only) {
        uint8_t sector[DISCBOT_CD_SECTOR_SIZE];
        if (read_chunk(reader, first_lba, 1, sector, error, sizeof(error)) != 0) {
            fprintf(stderr, "First-sector probe failed at LBA %u: %s\n", first_lba, error);
            discbot_cd_reader_close(reader);
            return 1;
        }
        if (read_chunk(reader, leadout - 1, 1, sector, error, sizeof(error)) != 0) {
            fprintf(stderr, "Last-sector probe failed at LBA %u: %s\n", leadout - 1, error);
            discbot_cd_reader_close(reader);
            return 1;
        }
        printf("PASS: raw CD access and first/last sector reads succeeded\n");
        discbot_cd_reader_close(reader);
        return 0;
    }

    char bin_path[1024], cue_path[1024], partial_path[1024];
    if (snprintf(bin_path, sizeof(bin_path), "%s.bin", argv[2]) >= (int)sizeof(bin_path) ||
        snprintf(cue_path, sizeof(cue_path), "%s.cue", argv[2]) >= (int)sizeof(cue_path) ||
        snprintf(partial_path, sizeof(partial_path), "%s.partial", argv[2]) >= (int)sizeof(partial_path)) {
        fprintf(stderr, "Output path is too long\n");
        discbot_cd_reader_close(reader);
        return 2;
    }
    if (access(bin_path, F_OK) == 0 || access(cue_path, F_OK) == 0) {
        fprintf(stderr, "Output already exists\n");
        discbot_cd_reader_close(reader);
        return 2;
    }

    FILE *output = fopen(partial_path, "wb");
    if (!output) {
        fprintf(stderr, "Could not create %s: %s\n", partial_path, strerror(errno));
        discbot_cd_reader_close(reader);
        return 1;
    }

    enum { chunk_sectors = 16 };
    uint8_t *buffer = malloc(chunk_sectors * DISCBOT_CD_SECTOR_SIZE);
    uint32_t current = first_lba;
    uint32_t next_percent = 0;
    int result = 1;
    if (!buffer) {
        fprintf(stderr, "Could not allocate read buffer\n");
        goto cleanup;
    }
    while (current < leadout) {
        uint32_t remaining = leadout - current;
        uint32_t count = remaining < chunk_sectors ? remaining : chunk_sectors;
        if (read_chunk(reader, current, count, buffer, error, sizeof(error)) != 0) {
            fprintf(stderr, "Read failed at LBA %u: %s\n", current, error);
            goto cleanup;
        }
        size_t bytes = (size_t)count * DISCBOT_CD_SECTOR_SIZE;
        if (fwrite(buffer, 1, bytes, output) != bytes) {
            fprintf(stderr, "Write failed: %s\n", strerror(errno));
            goto cleanup;
        }
        current += count;
        uint32_t percent = (uint32_t)(
            ((uint64_t)(current - first_lba) * 100) / (leadout - first_lba));
        if (percent >= next_percent) {
            printf("Progress: %u%% (%u/%u sectors)\n", percent,
                   current - first_lba, leadout - first_lba);
            fflush(stdout);
            next_percent = percent + 5;
        }
    }
    if (fflush(output) != 0 || fsync(fileno(output)) != 0 || fclose(output) != 0) {
        output = NULL;
        fprintf(stderr, "Could not finalize raw image: %s\n", strerror(errno));
        goto cleanup;
    }
    output = NULL;
    if (rename(partial_path, bin_path) != 0) {
        fprintf(stderr, "Could not finalize %s: %s\n", bin_path, strerror(errno));
        goto cleanup;
    }

    FILE *cue = fopen(cue_path, "w");
    if (!cue) {
        fprintf(stderr, "Could not create %s: %s\n", cue_path, strerror(errno));
        unlink(bin_path);
        goto cleanup;
    }
    const char *bin_name = strrchr(bin_path, '/');
    bin_name = bin_name ? bin_name + 1 : bin_path;
    fprintf(cue, "FILE \"%s\" BINARY\n", bin_name);
    for (uint32_t index = 0; index < track_count; index++) {
        uint8_t control = discbot_cd_reader_track_control(reader, index);
        fprintf(cue, "  TRACK %02u %s\n",
                discbot_cd_reader_track_number(reader, index),
                (control & 0x04) ? "MODE1/2352" : "AUDIO");
        if (control & 0x01) fprintf(cue, "    FLAGS PRE\n");
        if (control & 0x02) fprintf(cue, "    FLAGS DCP\n");
        if (control & 0x08) fprintf(cue, "    FLAGS 4CH\n");
        fprintf(cue, "    INDEX 01 ");
        cue_time(cue, discbot_cd_reader_track_start_lba(reader, index) - first_lba);
        fputc('\n', cue);
    }
    if (fclose(cue) != 0) {
        fprintf(stderr, "Could not finalize cue sheet: %s\n", strerror(errno));
        unlink(bin_path);
        unlink(cue_path);
        goto cleanup;
    }

    printf("Created: %s\nCreated: %s\n", bin_path, cue_path);
    result = 0;

cleanup:
    if (output) fclose(output);
    if (result != 0) unlink(partial_path);
    free(buffer);
    discbot_cd_reader_close(reader);
    return result;
}
