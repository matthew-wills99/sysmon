#ifndef MEM_H
#define MEM_H

#include "utils.h"

typedef struct {
    float mainUsagePercent;
    float swapUsagePercent;
    SizeInfo used;
    SizeInfo total;
    SizeInfo swapUsed;
    SizeInfo swapTotal;
} MemoryInfo;

/* Reads /proc/meminfo and fills in *mem. Unlike CPU usage this is a snapshot,
   so there is nothing to initialise or free. Call it whenever you want fresh
   numbers. Returns 0 on success, -1 on failure. */
int  mem_update(MemoryInfo *mem);

#endif /* MEM_H */