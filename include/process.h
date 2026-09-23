#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <time.h>

#include "utils.h"

typedef struct {
    int pid;
    char name[64];               /* program name: argv[0] without its path, else the kernel's comm name */
    float CPUPercent;
    float memPercent;            /* resident memory as a percentage of total RAM */
    SizeInfo memUsed;            /* resident memory (RSS) */
    SizeInfo memTotal;           /* total system RAM (the same for every process) */

    /* Internal bookkeeping */
    unsigned long long cpuTicks; /* cumulative utime + stime, in clock ticks */
} ProcessInfo;

typedef struct {
    ProcessInfo *items;          /* sorted by pid */
    size_t count;
    size_t capacity;

    /* Internal */
    unsigned long long memTotalBytes;
    long pageSize;
    struct timespec lastSample;
    int hasSample;
} ProcessList;

/* Reads static info and takes a baseline sample.
   Returns 0 on success, non-zero on failure. Pair with process_free(). */
int  process_init(ProcessList *list);

/* Rebuilds the process list. CPU usage is calculated between this call and the
   previous one, so call it on an interval. Returns 0 on success, -1 on failure
   (in which case the previous list is left untouched). */
int  process_update(ProcessList *list);

void process_free(ProcessList *list);

/* Writes the process's full command line (arguments joined by spaces) into buf.
   Returns 0 on success, -1 if there isn't one (kernel threads) or it can't be read. */
int  process_cmdline(int pid, char *buf, size_t n);

#endif /* PROCESS_H */