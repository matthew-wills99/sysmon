#ifndef GPU_H
#define GPU_H

#include <stddef.h>
#include <stdio.h>

#include "utils.h"

typedef enum { GPU_UNKNOWN = 0, GPU_NVIDIA, GPU_AMD, GPU_INTEL } GpuVendor;

typedef struct {
    char name[64];
    GpuVendor vendor;

    /* Each reading is only meaningful when its has* flag is set */
    int hasUsage;
    int usageEstimated;          /* Intel: derived from RC6 residency / clock speed, not an engine counter */
    int hasMem;
    int hasTemp;
    int suspended;               /* powered down by runtime PM: left alone so it can stay asleep */

    float usagePercent;
    float memPercent;
    float tempC;
    SizeInfo memUsed;
    SizeInfo memTotal;

    /* Internal: where to read this GPU's numbers from */
    char pathBase[192];          /* .../cardN/device */
    char pathCard[192];          /* .../cardN */
    char pathTemp[192];
    char pathPower[192];         /* runtime_status */
    char pathVramUsed[192], pathVramTotal[192];
    char pathA[192], pathB[192]; /* usage source: busy file, or counter, or freq + max freq */
    int  usageSrc;
    void *nvmlDev;
    double lastCounter, lastTime;
    int  haveLast;
} GpuInfo;

typedef struct GpuNvml GpuNvml;

typedef struct {
    GpuInfo *items;
    size_t count;
    GpuNvml *nvml;               /* libnvidia-ml, loaded at runtime if the driver is installed */
} GpuList;

/* Where to look. NULL members use the system defaults; tests point these at fake trees. */
typedef struct {
    const char *drm;             /* /sys/class/drm */
    const char *pci;             /* /sys/bus/pci/devices */
    const char *pciIds;          /* pci.ids database (for model names) */
    FILE *log;                   /* if set, explains what was tried and what was found (--gpu-info) */
} GpuPaths;

/* Finds NVIDIA, AMD and Intel GPUs. Having none is normal, so this always returns 0
   and simply leaves count at 0. Pair with gpu_free(). */
int  gpu_init(GpuList *list);
int  gpu_init_with(GpuList *list, const GpuPaths *paths);

/* Refreshes every GPU. Returns 0; a GPU that can't be read just loses its has* flags. */
int  gpu_update(GpuList *list);
void gpu_free(GpuList *list);

#endif /* GPU_H */