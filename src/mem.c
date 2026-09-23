#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "utils.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Labels we want from /proc/meminfo (values are in kB) */
static const char *include_mem[] = {
    "MemTotal", "MemAvailable", "SwapTotal", "SwapFree"
};

int mem_update(MemoryInfo *mem) {
    kv_pair *entries;
    size_t count = parse_kv_file("/proc/meminfo", include_mem,
                                 ARRAY_LEN(include_mem), &entries);
    if (count == (size_t)-1) return -1;

    unsigned long long memTotal = 0, memAvailable = 0;
    unsigned long long swapTotal = 0, swapFree = 0;
    int haveTotal = 0, haveAvailable = 0;

    for (size_t i = 0; i < count; i++) {
        const char *label = entries[i].label;
        /* values look like "16303204 kB"; strtoull stops at the space */
        unsigned long long v = strtoull(entries[i].value, NULL, 10);

        if (strcmp(label, "MemTotal") == 0)          { memTotal = v;     haveTotal = 1; }
        else if (strcmp(label, "MemAvailable") == 0) { memAvailable = v; haveAvailable = 1; }
        else if (strcmp(label, "SwapTotal") == 0)    { swapTotal = v; }
        else if (strcmp(label, "SwapFree") == 0)     { swapFree = v; }
    }
    free_kv_pairs(entries, count);

    if (!haveTotal || !haveAvailable || memTotal == 0) return -1;

    /* "Used" = everything that isn't available for new allocations */
    unsigned long long memUsed  = (memAvailable < memTotal) ? memTotal - memAvailable : 0;
    unsigned long long swapUsed = (swapFree < swapTotal) ? swapTotal - swapFree : 0;

    /* /proc/meminfo reports kB, and size_from_bytes wants bytes */
    mem->total     = size_from_bytes((double)memTotal  * 1024.0);
    mem->used      = size_from_bytes((double)memUsed   * 1024.0);
    mem->swapTotal = size_from_bytes((double)swapTotal * 1024.0);
    mem->swapUsed  = size_from_bytes((double)swapUsed  * 1024.0);

    mem->mainUsagePercent = 100.0f * (float)memUsed / (float)memTotal;
    mem->swapUsagePercent = (swapTotal > 0)
                          ? 100.0f * (float)swapUsed / (float)swapTotal
                          : 0.0f;   /* no swap configured */

    return 0;
}