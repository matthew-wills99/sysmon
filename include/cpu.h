#ifndef CPU_H
#define CPU_H

#include <stddef.h>

#define MAX_CORES   256
#define MAX_THREADS 256

typedef struct {
    char name[16];
    float usagePercent;
    float clockSpeed;                         /* MHz, averaged over the core's threads */
    unsigned long long total, idle;           /* latest sample */
    unsigned long long prevTotal, prevIdle;   /* previous sample */
} CoreInfo;

typedef struct {
    /* Read once in cpu_init() */
    char name[64];
    int physicalCores;
    int threads;
    unsigned long long bootTime;              /* seconds since the epoch */
    unsigned long long uptimeSeconds;         /* now - bootTime, refreshed by cpu_update() */

    /* Overall usage (the "cpu" row of /proc/stat) */
    float usagePercent;
    unsigned long long total, idle;
    unsigned long long prevTotal, prevIdle;

    /* Per-core data, refreshed by cpu_update() */
    CoreInfo *cores;
    size_t coreCount;

    /* Internal lookup tables built by cpu_init() */
    int threadCoreId[MAX_THREADS];            /* processor N -> core id (-1 = unknown) */
    int coreIndexById[MAX_CORES];             /* core id -> index into cores[] (-1 = none) */
} CpuInfo;

/* Collects static info, builds the thread->core map and takes a baseline sample.
   Returns 0 on success, non-zero on failure. Pair with cpu_free(). */
int  cpu_init(CpuInfo *cpu);

/* Takes a new sample. Usage is calculated between this call and the previous
   one, so call it on an interval (e.g. once per second).
   Returns 0 on success, -1 on failure. */
int  cpu_update(CpuInfo *cpu);

void cpu_print(const CpuInfo *cpu);
void cpu_free(CpuInfo *cpu);

#endif /* CPU_H */