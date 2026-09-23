#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "disk.h"
#include "utils.h"

#define SYS_BLOCK   "/sys/block"
#define DEVICE_LINK "device"     /* /sys/block/<disk>/device, present on real hardware */
#define SECTOR_SIZE 512ULL       /* sysfs and /proc/diskstats always count 512-byte sectors */
#define MAX_MOUNTS  128

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Reads a sysfs file holding a single number. Returns 0 on success. */
static int read_sysfs_ull(const char *path, unsigned long long *out) {
    char *s = read_info_file(path, NULL);
    if (!s) return -1;
    *out = strtoull(s, NULL, 10);
    free(s);
    return 0;
}

static double seconds_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static int compare_disks(const void *a, const void *b) {
    return strcmp(((const DiskInfo *)a)->name, ((const DiskInfo *)b)->name);
}

/* ------------------------------------------------------------------ */
/* Discovering physical disks                                         */
/* ------------------------------------------------------------------ */

/* A block device counts as a physical disk if /sys/block/<name>/device exists
   and /sys/block/<name>/queue/rotational exists. This rules out loop, ram,
   zram, dm-* and md* devices, which have no backing hardware. With
   includeVirtual those are listed too (except empty ones, which have nothing
   to show), marked as VRT. */
static int discover_disks(DiskList *list, int includeVirtual) {
    DIR *d = opendir(SYS_BLOCK);
    if (!d) {
        perror("opendir " SYS_BLOCK);
        return -1;
    }

    size_t cap = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t nameLen = strlen(e->d_name);
        if (nameLen >= sizeof list->items[0].name) continue;

        char path[PATH_MAX];

        snprintf(path, sizeof path, SYS_BLOCK "/%s/" DEVICE_LINK, e->d_name);
        int physical = is_dir(path);
        if (!physical && !includeVirtual) continue;

        unsigned long long rotational;
        snprintf(path, sizeof path, SYS_BLOCK "/%s/queue/rotational", e->d_name);
        if (read_sysfs_ull(path, &rotational) != 0) continue;

        unsigned long long sectors = 0;
        snprintf(path, sizeof path, SYS_BLOCK "/%s/size", e->d_name);
        read_sysfs_ull(path, &sectors);   /* stays 0 if unreadable */
        if (!physical && sectors == 0) continue;

        if (list->count == cap) {
            size_t newCap = cap ? cap * 2 : 8;
            DiskInfo *tmp = realloc(list->items, newCap * sizeof *tmp);
            if (!tmp) {
                closedir(d);
                disk_free(list);
                return -1;
            }
            list->items = tmp;
            cap = newCap;
        }

        DiskInfo *disk = &list->items[list->count++];
        memset(disk, 0, sizeof *disk);
        memcpy(disk->name, e->d_name, nameLen + 1);
        disk->type      = !physical ? VRT : (rotational == 1) ? HDD : SSD;
        disk->sizeBytes = sectors * SECTOR_SIZE;
        disk->size      = size_from_bytes((double)disk->sizeBytes);
    }
    closedir(d);

    /* readdir order is arbitrary, so sort for a stable display order */
    if (list->count > 1) qsort(list->items, list->count, sizeof *list->items, compare_disks);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Used space (from mounted filesystems)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char dev[64];                 /* block device name, e.g. "sda1" or "dm-0" */
    unsigned long long usedBytes;
} MountUsage;

/* /proc/mounts escapes spaces and the like as octal, e.g. "\040" */
static void unescape_mount_field(char *s) {
    char *w = s;
    const char *r = s;
    while (*r) {
        if (r[0] == '\\' && isdigit((unsigned char)r[1]) &&
            isdigit((unsigned char)r[2]) && isdigit((unsigned char)r[3])) {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Collects used bytes for every mounted block-device filesystem. A device
   mounted more than once (bind mounts, btrfs subvolumes) is counted once. */
static size_t collect_mounts(MountUsage *out, size_t max) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;

    size_t n = 0;
    char line[1024];

    while (n < max && fgets(line, sizeof line, f)) {
        char src[256], mnt[512];
        if (sscanf(line, "%255s %511s", src, mnt) != 2) continue;
        if (strncmp(src, "/dev/", 5) != 0) continue;

        /* Resolve symlinks like /dev/mapper/vg-root -> /dev/dm-1 */
        char real[PATH_MAX];
        if (!realpath(src, real) || strncmp(real, "/dev/", 5) != 0) continue;

        const char *dev = real + 5;
        if (strlen(dev) >= sizeof out[0].dev) continue;

        int duplicate = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(out[i].dev, dev) == 0) { duplicate = 1; break; }
        }
        if (duplicate) continue;

        unescape_mount_field(mnt);
        struct statvfs vfs;
        if (statvfs(mnt, &vfs) != 0) continue;

        memcpy(out[n].dev, dev, strlen(dev) + 1);           /* length checked above */
        out[n].usedBytes = (vfs.f_blocks > vfs.f_bfree)
                         ? (unsigned long long)(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize
                         : 0;
        n++;
    }
    fclose(f);
    return n;
}

/* Does block device `dev` live on physical disk `disk`?
   Handles the disk itself, its partitions, and stacked devices (LVM, LUKS,
   md RAID) by following /sys/block/<dev>/slaves. */
static int device_on_disk(const char *dev, const char *disk, int depth) {
    if (strcmp(dev, disk) == 0) return 1;

    char path[PATH_MAX];

    /* Partitions show up as /sys/block/<disk>/<partition> */
    snprintf(path, sizeof path, SYS_BLOCK "/%s/%s", disk, dev);
    if (is_dir(path)) return 1;

    if (depth >= 4) return 0;

    snprintf(path, sizeof path, SYS_BLOCK "/%s/slaves", dev);
    DIR *d = opendir(path);
    if (!d) return 0;

    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        found = device_on_disk(e->d_name, disk, depth + 1);
    }
    closedir(d);
    return found;
}

static void update_usage(DiskList *list) {
    MountUsage mounts[MAX_MOUNTS];
    size_t mountCount = collect_mounts(mounts, MAX_MOUNTS);

    for (size_t i = 0; i < list->count; i++) {
        DiskInfo *disk = &list->items[i];
        unsigned long long used = 0;

        for (size_t m = 0; m < mountCount; m++) {
            if (device_on_disk(mounts[m].dev, disk->name, 0)) used += mounts[m].usedBytes;
        }

        /* Filesystem rounding/padding can push "used" a hair past the raw disk size */
        if (disk->sizeBytes > 0 && used > disk->sizeBytes) used = disk->sizeBytes;

        disk->used = size_from_bytes((double)used);
        disk->usagePercent = (disk->sizeBytes > 0)
                           ? 100.0f * (float)used / (float)disk->sizeBytes
                           : 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int disk_init(DiskList *list, int includeVirtual) {
    memset(list, 0, sizeof *list);

    if (discover_disks(list, includeVirtual) != 0) return 1;

    /* Baseline sample; rates stay 0 until the next disk_update() */
    if (disk_update(list) != 0) {
        disk_free(list);
        return 1;
    }
    return 0;
}

int disk_update(DiskList *list) {
    char *data = read_info_file("/proc/diskstats", NULL);
    if (!data) return -1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = list->hasSample ? seconds_between(&list->lastSample, &now) : 0.0;

    /* Roll the previous sample over */
    for (size_t i = 0; i < list->count; i++) {
        list->items[i].prevSectorsRead    = list->items[i].sectorsRead;
        list->items[i].prevSectorsWritten = list->items[i].sectorsWritten;
    }

    /* /proc/diskstats: major minor name reads reads_merged sectors_read ms
       writes writes_merged sectors_written ... */
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[64];
        unsigned long long rd, wr;

        if (sscanf(line, "%*u %*u %63s %*u %*u %llu %*u %*u %*u %llu", name, &rd, &wr) != 3)
            continue;

        for (size_t i = 0; i < list->count; i++) {
            if (strcmp(list->items[i].name, name) == 0) {
                list->items[i].sectorsRead    = rd;
                list->items[i].sectorsWritten = wr;
                break;
            }
        }
    }
    free(data);

    for (size_t i = 0; i < list->count; i++) {
        DiskInfo *disk = &list->items[i];

        unsigned long long dRead  = (disk->sectorsRead >= disk->prevSectorsRead)
                                  ? disk->sectorsRead - disk->prevSectorsRead : 0;
        unsigned long long dWrite = (disk->sectorsWritten >= disk->prevSectorsWritten)
                                  ? disk->sectorsWritten - disk->prevSectorsWritten : 0;

        double readBps  = (dt > 0) ? (double)dRead  * SECTOR_SIZE / dt : 0.0;
        double writeBps = (dt > 0) ? (double)dWrite * SECTOR_SIZE / dt : 0.0;

        disk->readPerSecond  = size_from_bytes(readBps);
        disk->writePerSecond = size_from_bytes(writeBps);
    }

    list->lastSample = now;
    list->hasSample = 1;

    update_usage(list);
    return 0;
}

void disk_free(DiskList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
}