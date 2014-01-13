/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This program takes a file on an ext4 filesystem and produces a list
// of the blocks that file occupies, which enables the file contents
// to be read directly from the block device without mounting the
// filesystem.
//
// If the filesystem is using an encrypted block device, it will also
// read the file and rewrite it to the same blocks of the underlying
// (unencrypted) block device, so the file contents can be read
// without the need for the decryption key.
//
// The output of this program is a "block map" which looks like this:
//
//     /dev/block/platform/msm_sdcc.1/by-name/userdata     # block device
//     49652 4096                        # file size in bytes, block size
//     3                                 # count of block ranges
//     1000 1008                         # block range 0
//     2100 2102                         # ... block range 1
//     30 33                             # ... block range 2
//
// Each block range represents a half-open interval; the line "30 33"
// reprents the blocks [30, 31, 32].
//
// Recovery can take this block map file and retrieve the underlying
// file data to use as an update package.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/mman.h>

#include <cutils/properties.h>
#include <fs_mgr.h>

#define WINDOW_SIZE 5
#define RECOVERY_COMMAND_FILE "/cache/recovery/command"
#define RECOVERY_COMMAND_FILE_TMP "/cache/recovery/command.tmp"
#define CACHE_BLOCK_MAP "/cache/recovery/block.map"

static int write_at_offset(unsigned char* buffer, size_t size,
                           int wfd, off64_t offset)
{
    lseek64(wfd, offset, SEEK_SET);
    size_t written = 0;
    while (written < size) {
        ssize_t wrote = write(wfd, buffer + written, size - written);
        if (wrote < 0) {
            fprintf(stderr, "error writing offset %lld: %s\n", offset, strerror(errno));
            return -1;
        }
        written += wrote;
    }
    return 0;
}

void add_block_to_ranges(int** ranges, int* range_alloc, int* range_used, int new_block)
{
    // If the current block start is < 0, set the start to the new
    // block.  (This only happens for the very first block of the very
    // first range.)
    if ((*ranges)[*range_used*2-2] < 0) {
        (*ranges)[*range_used*2-2] = new_block;
        (*ranges)[*range_used*2-1] = new_block;
    }

    if (new_block == (*ranges)[*range_used*2-1]) {
        // If the new block comes immediately after the current range,
        // all we have to do is extend the current range.
        ++(*ranges)[*range_used*2-1];
    } else {
        // We need to start a new range.

        // If there isn't enough room in the array, we need to expand it.
        if (*range_used >= *range_alloc) {
            *range_alloc *= 2;
            *ranges = realloc(*ranges, *range_alloc * 2 * sizeof(int));
        }

        ++*range_used;
        (*ranges)[*range_used*2-2] = new_block;
        (*ranges)[*range_used*2-1] = new_block+1;
    }
}

const char* find_block_device(const char* path, int* encryptable, int* encrypted)
{
    // The fstab path is always "/fstab.${ro.hardware}".
    char fstab_path[PATH_MAX+1] = "/fstab.";
    if (!property_get("ro.hardware", fstab_path+strlen(fstab_path), "")) {
        fprintf(stderr, "failed to get ro.hardware\n");
        return NULL;
    }

    struct fstab* fstab = fs_mgr_read_fstab(fstab_path);
    if (!fstab) {
        fprintf(stderr, "failed to read %s\n", fstab_path);
        return NULL;
    }

    // Look for a volume whose mount point is the prefix of path and
    // return its block device.  Set encrypted if it's currently
    // encrypted.
    int i;
    for (i = 0; i < fstab->num_entries; ++i) {
        struct fstab_rec* v = &fstab->recs[i];
        if (!v->mount_point) continue;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
            (path[len] == '/' || path[len] == 0)) {
            *encrypted = 0;
            *encryptable = 0;
            if (fs_mgr_is_encryptable(v)) {
                *encryptable = 1;
                char buffer[PROPERTY_VALUE_MAX+1];
                if (property_get("ro.crypto.state", buffer, "") &&
                    strcmp(buffer, "encrypted") == 0) {
                    *encrypted = 1;
                }
            }
            return v->blk_device;
        }
    }

    return NULL;
}

char* parse_recovery_command_file()
{
    char* fn = NULL;
    int count = 0;
    char temp[1024];

    FILE* fo = fopen(RECOVERY_COMMAND_FILE_TMP, "w");

    FILE* f = fopen(RECOVERY_COMMAND_FILE, "r");
    while (fgets(temp, sizeof(temp), f)) {
        printf("read: %s", temp);
        if (strncmp(temp, "--update_package=", strlen("--update_package=")) == 0) {
            fn = strdup(temp + strlen("--update_package="));
            strcpy(temp, "--update_package=@" CACHE_BLOCK_MAP "\n");
        }
        fputs(temp, fo);
    }
    fclose(f);
    fclose(fo);

    if (fn) {
        char* newline = strchr(fn, '\n');
        if (newline) *newline = 0;
    }
    return fn;
}

int produce_block_map(const char* path, const char* map_file, const char* blk_dev,
                      int encrypted)
{
    struct stat sb;
    int ret;

    FILE* mapf = fopen(map_file, "w");

    ret = stat(path, &sb);
    if (ret != 0) {
        fprintf(stderr, "failed to stat %s\n", path);
        return -1;
    }

    printf(" block size: %ld bytes\n", sb.st_blksize);

    int blocks = ((sb.st_size-1) / sb.st_blksize) + 1;
    printf("  file size: %lld bytes, %d blocks\n", sb.st_size, blocks);

    int* ranges;
    int range_alloc = 1;
    int range_used = 1;
    ranges = malloc(range_alloc * 2 * sizeof(int));
    ranges[0] = -1;
    ranges[1] = -1;

    fprintf(mapf, "%s\n%lld %lu\n", blk_dev, sb.st_size, sb.st_blksize);

    unsigned char* buffers[WINDOW_SIZE];
    int i;
    if (encrypted) {
        for (i = 0; i < WINDOW_SIZE; ++i) {
            buffers[i] = malloc(sb.st_blksize);
        }
    }
    int head_block = 0;
    int head = 0, tail = 0;
    size_t pos = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "failed to open fd for reading: %s\n", strerror(errno));
        return -1;
    }
    fsync(fd);

    int wfd = -1;
    if (encrypted) {
        wfd = open(blk_dev, O_WRONLY);
        if (wfd < 0) {
            fprintf(stderr, "failed to open fd for writing: %s\n", strerror(errno));
            return -1;
        }
    }

    while (pos < sb.st_size) {
        if ((tail+1) % WINDOW_SIZE == head) {
            // write out head buffer
            int block = head_block;
            ret = ioctl(fd, FIBMAP, &block);
            if (ret != 0) {
                fprintf(stderr, "failed to find block %d\n", head_block);
                return -1;
            }
            add_block_to_ranges(&ranges, &range_alloc, &range_used, block);
            if (encrypted) {
                if (write_at_offset(buffers[head], sb.st_blksize, wfd, (off64_t)sb.st_blksize * block) != 0) {
                    return -1;
                }
            }
            head = (head + 1) % WINDOW_SIZE;
            ++head_block;
        }

        // read next block to tail
        if (encrypted) {
            size_t so_far = 0;
            while (so_far < sb.st_blksize && pos < sb.st_size) {
                ssize_t this_read = read(fd, buffers[tail] + so_far, sb.st_blksize - so_far);
                if (this_read < 0) {
                    fprintf(stderr, "failed to read: %s\n", strerror(errno));
                    return -1;
                }
                so_far += this_read;
                pos += this_read;
            }
        } else {
            // If we're not encrypting; we don't need to actually read
            // anything, just skip pos forward as if we'd read a
            // block.
            pos += sb.st_blksize;
        }
        tail = (tail+1) % WINDOW_SIZE;
    }

    while (head != tail) {
        // write out head buffer
        int block = head_block;
        ret = ioctl(fd, FIBMAP, &block);
        if (ret != 0) {
            fprintf(stderr, "failed to find block %d\n", head_block);
            return -1;
        }
        add_block_to_ranges(&ranges, &range_alloc, &range_used, block);
        if (encrypted) {
            if (write_at_offset(buffers[head], sb.st_blksize, wfd, (off64_t)sb.st_blksize * block) != 0) {
                return -1;
            }
        }
        head = (head + 1) % WINDOW_SIZE;
        ++head_block;
    }

    fprintf(mapf, "%d\n", range_used);
    for (i = 0; i < range_used; ++i) {
        fprintf(mapf, "%d %d\n", ranges[i*2], ranges[i*2+1]);
    }

    fclose(mapf);
    close(fd);
    if (encrypted) {
        close(wfd);
    }

    return 0;
}

void reboot_to_recovery() {
    property_set("sys.powerctl", "reboot,recovery");
    sleep(10);
}

int main(int argc, char** argv)
{
    const char* input_path;
    const char* map_file;
    int do_reboot = 1;

    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s [<transform_path> <map_file>]\n", argv[0]);
        return 2;
    }

    if (argc == 3) {
        // when command-line args are given this binary is being used
        // for debugging; don't reboot to recovery at the end.
        input_path = argv[1];
        map_file = argv[2];
        do_reboot = 0;
    } else {
        input_path = parse_recovery_command_file();
        if (input_path == NULL) {
            // if we're rebooting to recovery without a package (say,
            // to wipe data), then we don't need to do anything before
            // going to recovery.
            fprintf(stderr, "no recovery command file or no update package arg");
            reboot_to_recovery();
            return 1;
        }
        map_file = CACHE_BLOCK_MAP;
    }

    // Turn the name of the file we're supposed to convert into an
    // absolute path, so we can find what filesystem it's on.
    char path[PATH_MAX+1];
    if (realpath(input_path, path) == NULL) {
        fprintf(stderr, "failed to convert %s to absolute path: %s\n", input_path, strerror(errno));
        return 1;
    }

    int encryptable;
    int encrypted;
    const char* blk_dev = find_block_device(path, &encryptable, &encrypted);
    if (blk_dev == NULL) {
        fprintf(stderr, "failed to find block device for %s\n", path);
        return 1;
    }

    // If the filesystem it's on isn't encrypted, we only produce the
    // block map, we don't rewrite the file contents (it would be
    // pointless to do so).
    printf("encryptable: %s\n", encryptable ? "yes" : "no");
    printf("  encrypted: %s\n", encrypted ? "yes" : "no");

    if (!encryptable) {
        // If the file is on a filesystem that doesn't support
        // encryption (eg, /cache), then leave it alone.
        //
        // TODO: change this to be !encrypted -- if the file is on
        // /data but /data isn't encrypted, we don't need to use the
        // block map mechanism.  We do for now so as to get more
        // testing of it (since most dogfood devices aren't
        // encrypted).

        unlink(RECOVERY_COMMAND_FILE_TMP);
    } else {
        if (produce_block_map(path, map_file, blk_dev, encrypted) != 0) {
            return 1;
        }
    }

    rename(RECOVERY_COMMAND_FILE_TMP, RECOVERY_COMMAND_FILE);
    reboot_to_recovery();
    return 0;
}
