#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <mtd/mtd-user.h>
#include <errno.h>

void print_progress(const char *label, long long current, long long total) {
    int width = 40;
    int progress = (int)((double)current / total * width);
    printf("\r%s: [", label);
    for (int i = 0; i < width; ++i) {
        if (i < progress) printf("=");
        else if (i == progress) printf(">");
        else printf(" ");
    }
    printf("] %3d%% (%lld/%lld)", (int)((double)current / total * 100), current, total);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input_file> <mtd_device>\n", argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *mtd_path = argv[2];

    int input_fd = open(input_path, O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "Error: Cannot open input file %s: %s\n", input_path, strerror(errno));
        return 1;
    }

    struct stat input_stat;
    if (fstat(input_fd, &input_stat) < 0) {
        fprintf(stderr, "Error: Cannot stat input file: %s\n", strerror(errno));
        close(input_fd);
        return 1;
    }
    long long file_size = input_stat.st_size;

    int mtd_fd = open(mtd_path, O_RDWR);
    if (mtd_fd < 0) {
        fprintf(stderr, "Error: Cannot open MTD device %s: %s\n", mtd_path, strerror(errno));
        close(input_fd);
        return 1;
    }

    struct mtd_info_user mtd_info;
    if (ioctl(mtd_fd, MEMGETINFO, &mtd_info) < 0) {
        fprintf(stderr, "Error: MEMGETINFO ioctl failed on %s: %s\n", mtd_path, strerror(errno));
        close(input_fd);
        close(mtd_fd);
        return 1;
    }

    printf("MTD Device: %s\n", mtd_path);
    printf("  Total Size: %u bytes (%u KiB)\n", mtd_info.size, mtd_info.size / 1024);
    printf("  Erase Size: %u bytes (%u KiB)\n", mtd_info.erasesize, mtd_info.erasesize / 1024);
    printf("Input File: %s\n", input_path);
    printf("  Size:       %lld bytes (%lld KiB)\n", file_size, file_size / 1024);

    if (file_size > mtd_info.size) {
        fprintf(stderr, "Error: Input file size (%lld) exceeds MTD device size (%u)\n", file_size, mtd_info.size);
        close(input_fd);
        close(mtd_fd);
        return 1;
    }

    // Determine how much to erase (align up to erase size)
    long long erase_length = ((file_size + mtd_info.erasesize - 1) / mtd_info.erasesize) * mtd_info.erasesize;
    if (erase_length > mtd_info.size) {
        erase_length = mtd_info.size;
    }

    printf("Erasing %lld bytes (%lld KiB) in units of %u KiB...\n", erase_length, erase_length / 1024, mtd_info.erasesize / 1024);

    // Erase loop
    for (long long offset = 0; offset < erase_length; offset += mtd_info.erasesize) {
        struct erase_info_user erase;
        erase.start = offset;
        erase.length = mtd_info.erasesize;

        print_progress("Erasing", offset, erase_length);

        if (ioctl(mtd_fd, MEMERASE, &erase) < 0) {
            printf("\n");
            fprintf(stderr, "Error: Erase failed at offset 0x%llx: %s\n", offset, strerror(errno));
            close(input_fd);
            close(mtd_fd);
            return 1;
        }
    }
    print_progress("Erasing", erase_length, erase_length);
    printf("\nErase completed successfully.\n");

    // Write + immediately verify each erase block, retrying (erase + rewrite) on mismatch.
    // Chunk size MUST equal the real erase granularity: on NOR flash a program op can only
    // clear bits (1->0), so a corrupted chunk can't be "fixed" by writing over it again —
    // the exact erase block(s) it lives in must be MEMERASE'd again first. Chunk-per-erase-block
    // keeps a retry's erase from ever touching already-good neighboring data.
    #define MAX_CHUNK_RETRIES 3
    unsigned int chunk_size = mtd_info.erasesize;
    long long total_chunks = (file_size + chunk_size - 1) / chunk_size;
    long long chunks_with_faults = 0;   // chunks that needed >=1 retry to verify correctly
    long long total_fault_events = 0;   // every individual failed verify attempt, across all chunks

    printf("Writing + verifying input file to MTD device (chunk=%u bytes, retry=%d)...\n",
           chunk_size, MAX_CHUNK_RETRIES);

    char *buf = malloc(chunk_size);
    char *verify_buf = malloc(chunk_size);
    if (!buf || !verify_buf) {
        fprintf(stderr, "Error: Out of memory\n");
        free(buf); free(verify_buf);
        close(input_fd);
        close(mtd_fd);
        return 1;
    }

    long long offset = 0;
    while (offset < file_size) {
        long long to_read = file_size - offset;
        if (to_read > chunk_size) to_read = chunk_size;

        if (lseek(input_fd, offset, SEEK_SET) < 0) {
            printf("\n");
            fprintf(stderr, "Error: Cannot seek input file to 0x%llx: %s\n", offset, strerror(errno));
            free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
            return 1;
        }
        ssize_t read_bytes = read(input_fd, buf, to_read);
        if (read_bytes != to_read) {
            printf("\n");
            fprintf(stderr, "Error: Read from input file failed at 0x%llx: %s\n", offset, strerror(errno));
            free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
            return 1;
        }

        print_progress("Writing", offset, file_size);

        int attempt;
        int ok = 0;
        for (attempt = 1; attempt <= MAX_CHUNK_RETRIES; ++attempt) {
            if (attempt > 1) {
                // Re-erase exactly this erase block before retrying the write — required,
                // NOR flash can't un-program a bit back to 1 without a real erase cycle.
                struct erase_info_user erase;
                erase.start = offset;
                erase.length = chunk_size;
                if (ioctl(mtd_fd, MEMERASE, &erase) < 0) {
                    printf("\n");
                    fprintf(stderr, "Error: Re-erase failed at offset 0x%llx (retry %d): %s\n",
                            offset, attempt, strerror(errno));
                    free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
                    return 1;
                }
            }

            if (lseek(mtd_fd, offset, SEEK_SET) < 0) {
                printf("\n");
                fprintf(stderr, "Error: Cannot seek MTD device to 0x%llx: %s\n", offset, strerror(errno));
                free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
                return 1;
            }
            ssize_t written_bytes = write(mtd_fd, buf, read_bytes);
            if (written_bytes != read_bytes) {
                printf("\n");
                fprintf(stderr, "Error: Write to MTD device failed at offset 0x%llx (retry %d): %s\n",
                        offset, attempt, written_bytes < 0 ? strerror(errno) : "short write");
                free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
                return 1;
            }

            // Read back straight from the MTD character device — unlike /dev/mtdblockN this
            // path is NOT served from the page/buffer cache, so this is a real, physical
            // same-session verification, not a mirage.
            if (lseek(mtd_fd, offset, SEEK_SET) < 0) {
                printf("\n");
                fprintf(stderr, "Error: Cannot seek MTD device for verify at 0x%llx: %s\n", offset, strerror(errno));
                free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
                return 1;
            }
            ssize_t read_mtd = read(mtd_fd, verify_buf, read_bytes);
            if (read_mtd != read_bytes) {
                printf("\n");
                fprintf(stderr, "Error: Read-back from MTD device failed at 0x%llx (retry %d): %s\n",
                        offset, attempt, read_mtd < 0 ? strerror(errno) : "short read");
                free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
                return 1;
            }

            if (memcmp(buf, verify_buf, read_bytes) == 0) {
                ok = 1;
                break;
            }

            total_fault_events++;
            printf("\n");
            fprintf(stderr, "WARNING: chunk at offset 0x%llx failed verification (attempt %d/%d, fault #%lld so far)",
                    offset, attempt, MAX_CHUNK_RETRIES, total_fault_events);
            for (int i = 0; i < read_bytes; ++i) {
                if (buf[i] != verify_buf[i]) {
                    fprintf(stderr, " — first mismatch at +0x%x (expected 0x%02x, got 0x%02x)",
                            i, (unsigned char)buf[i], (unsigned char)verify_buf[i]);
                    break;
                }
            }
            fprintf(stderr, "\n");
        }

        if (attempt > 1 && ok) {
            chunks_with_faults++;
        }

        if (!ok) {
            fprintf(stderr, "ERROR: chunk at offset 0x%llx still corrupt after %d attempts — aborting.\n",
                    offset, MAX_CHUNK_RETRIES);
            fprintf(stderr, "Partition is now in a PARTIALLY WRITTEN state. DO NOT reboot — recover\n");
            fprintf(stderr, "from a bare UART shell (restore.sh) before investigating further.\n");
            fprintf(stderr, "Fault summary before abort: %lld/%lld chunks needed at least one retry, "
                            "%lld total failed verify attempts.\n",
                    chunks_with_faults, total_chunks, total_fault_events);
            free(buf); free(verify_buf); close(input_fd); close(mtd_fd);
            return 1;
        }

        offset += read_bytes;
    }
    print_progress("Writing", file_size, file_size);
    printf("\nWrite + per-chunk verification completed successfully.\n");
    printf("VERIFICATION SUCCESSFUL: every chunk matched the source file on first read-back or after retry.\n");
    printf("FAULT SUMMARY: %lld/%lld chunks needed at least one retry (%lld total failed verify attempts, "
           "%lld successful chunks first-try).\n",
           chunks_with_faults, total_chunks, total_fault_events, total_chunks - chunks_with_faults);

    free(buf);
    free(verify_buf);
    close(input_fd);
    close(mtd_fd);
    return 0;
}
