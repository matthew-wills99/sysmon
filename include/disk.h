#ifndef DISK_H
#define DISK_H

#include <stddef.h>
#include <time.h>

#include "utils.h"

enum DiskType {
    UNK,
    SSD,
    HDD,
    VRT                          /* not physical hardware (loop, dm, md, zram, ...) */
};

typedef struct {
    char name[32];
    enum DiskType type;
    float usagePercent;          /* used / size */
    SizeInfo used;               /* space used on mounted filesystems of this disk */
    SizeInfo size;               /* raw size of the disk */
    SizeInfo readPerSecond;
    SizeInfo writePerSecond;

    /* Internal bookkeeping */
    unsigned long long sizeBytes;
    unsigned long long sectorsRead, sectorsWritten;             /* latest sample */
    unsigned long long prevSectorsRead, prevSectorsWritten;     /* previous sample */
} DiskInfo;

typedef struct {
    DiskInfo *items;
    size_t count;

    /* Internal: when the last sample was taken, for the per-second rates */
    struct timespec lastSample;
    int hasSample;
} DiskList;

/* Finds the physical disks (or every block device with includeVirtual), reads
   their static info and takes a baseline sample.
   Returns 0 on success, non-zero on failure. Pair with disk_free(). */
int  disk_init(DiskList *list, int includeVirtual);

/* Takes a new sample. Read/write speeds are calculated between this call and
   the previous one, so call it on an interval. Returns 0 on success, -1 on failure. */
int  disk_update(DiskList *list);

void disk_free(DiskList *list);

#endif /* DISK_H */