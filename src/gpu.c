#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gpu.h"

#define DEFAULT_DRM "/sys/class/drm"
#define DEFAULT_PCI "/sys/bus/pci/devices"

#define PCI_VENDOR_INTEL  0x8086UL
#define PCI_VENDOR_AMD    0x1002UL
#define PCI_VENDOR_NVIDIA 0x10DEUL

enum { SRC_NONE = 0, SRC_AMD_BUSY, SRC_NVML, SRC_INTEL_RC6, SRC_INTEL_FREQ };

#define LOG(f, ...) do { if (f) fprintf((f), __VA_ARGS__); } while (0)

/* WSL2 has no DRM/sysfs GPU devices. The GPU is passed through as /dev/dxg (DirectX). */
#define WSL_DXG "/dev/dxg"

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* dst = a "/" b, or "" if it doesn't fit */
static int join(char *dst, size_t n, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 2 > n) {
        dst[0] = '\0';
        return -1;
    }
    memcpy(dst, a, la);
    dst[la] = '/';
    memcpy(dst + la + 1, b, lb + 1);
    return 0;
}

static int readable(const char *path) {
    return path[0] != '\0' && access(path, R_OK) == 0;
}

static int read_ll(const char *path, long long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long long v;
    int ok = fscanf(f, "%lld", &v) == 1;
    fclose(f);
    if (!ok) return -1;
    *out = v;
    return 0;
}

/* "0x1002\n" -> 0x1002 */
static int read_hex(const char *path, unsigned long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[32];
    char *got = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!got) return -1;
    char *end;
    unsigned long v = strtoul(buf, &end, 16);
    if (end == buf) return -1;
    *out = v;
    return 0;
}

/* A GPU that runtime power management has put to sleep must not be read: touching its
   sysfs files or asking the driver for numbers wakes it up, which defeats the power
   saving on hybrid-graphics laptops. */
static int is_suspended(const char *statusPath) {
    if (!readable(statusPath)) return 0;
    FILE *f = fopen(statusPath, "r");
    if (!f) return 0;
    char buf[32] = "";
    if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
    fclose(f);
    return strncmp(buf, "suspended", 9) == 0;
}

static void copy_path(char *dst, size_t n, const char *src) {
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

/* First of root/rels[i] that can be read */
static int first_readable(char *dst, size_t n, const char *root, const char *const *rels) {
    for (int i = 0; rels[i]; i++) {
        char p[192];
        if (join(p, sizeof p, root, rels[i]) == 0 && readable(p)) {
            copy_path(dst, n, p);
            return 1;
        }
    }
    dst[0] = '\0';
    return 0;
}

static GpuInfo *add_gpu(GpuList *l) {
    GpuInfo *tmp = realloc(l->items, (l->count + 1) * sizeof *tmp);
    if (!tmp) return NULL;
    l->items = tmp;
    GpuInfo *g = &l->items[l->count++];
    memset(g, 0, sizeof *g);
    return g;
}

/* ------------------------------------------------------------------ */
/* Model names from pci.ids                                           */
/* ------------------------------------------------------------------ */

static const char *const PCI_IDS_DEFAULT[] = {
    "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
    "/usr/share/pci.ids", "/var/lib/pciutils/pci.ids", NULL
};

static int hex4(const char *s, unsigned *out) {
    for (int i = 0; i < 4; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    *out = (unsigned)strtoul((char[]){ s[0], s[1], s[2], s[3], 0 }, NULL, 16);
    return 1;
}

/* Vendor lines are "1002  Name", device lines "\t73bf  Name" */
static int pci_ids_lookup_file(const char *file, unsigned vendor, unsigned device, char *out, size_t n) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;

    char line[512];
    int inVendor = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned id;
        if (line[0] != '\t') {
            if (inVendor) break;                                  /* left the vendor's block */
            if (hex4(line, &id) && line[4] == ' ' && id == vendor) inVendor = 1;
        } else if (inVendor && line[1] != '\t') {
            if (hex4(line + 1, &id) && id == device) {
                const char *name = line + 5;
                while (*name == ' ' || *name == '\t') name++;
                copy_path(out, n, name);
                out[strcspn(out, "\r\n")] = '\0';
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

static int pci_ids_lookup(const GpuPaths *p, unsigned vendor, unsigned device, char *out, size_t n) {
    if (p->pciIds) return pci_ids_lookup_file(p->pciIds, vendor, device, out, n);
    for (int i = 0; PCI_IDS_DEFAULT[i]; i++)
        if (pci_ids_lookup_file(PCI_IDS_DEFAULT[i], vendor, device, out, n)) return 1;
    return 0;
}

/* "Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]" -> "Radeon RX 6800/6800 XT / 6900 XT" */
static void model_name(const char *raw, GpuVendor v, char *out, size_t n) {
    char tmp[128];
    const char *open = strrchr(raw, '[');
    const char *close = open ? strchr(open, ']') : NULL;
    if (open && close && close > open + 1) {
        size_t len = (size_t)(close - open - 1);
        if (len >= sizeof tmp) len = sizeof tmp - 1;
        memcpy(tmp, open + 1, len);
        tmp[len] = '\0';
    } else {
        copy_path(tmp, sizeof tmp, raw);
    }
    /* "UHD Graphics 620" alone is ambiguous; "GeForce" and "Radeon" are not */
    if (v == GPU_INTEL && !strstr(tmp, "Intel")) {
        char full[160];
        snprintf(full, sizeof full, "Intel %s", tmp);
        copy_path(out, n, full);
    } else {
        copy_path(out, n, tmp);
    }
}

static void fallback_name(GpuVendor v, char *out, size_t n) {
    snprintf(out, n, "%s GPU", v == GPU_NVIDIA ? "NVIDIA" : v == GPU_AMD ? "AMD" : "Intel");
}

/* ------------------------------------------------------------------ */
/* NVIDIA (NVML)                                                      */
/* ------------------------------------------------------------------ */

typedef void *nvmlDevice_t;
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
/* Only domain/bus/device are used, and they sit at the same offsets in every
   version of the struct; the buffer is oversized to fit the newest layout. */
typedef struct {
    char head[16];
    unsigned int domain, bus, device, pciDeviceId, pciSubSystemId;
    char rest[96];
} nvmlPciInfo_t;

struct GpuNvml {
    void *lib;
    int (*init)(void);
    int (*shutdown)(void);
    int (*getCount)(unsigned int *);
    int (*getHandle)(unsigned int, nvmlDevice_t *);
    int (*getName)(nvmlDevice_t, char *, unsigned int);
    int (*getUtil)(nvmlDevice_t, nvmlUtilization_t *);
    int (*getMem)(nvmlDevice_t, nvmlMemory_t *);
    int (*getTemp)(nvmlDevice_t, int, unsigned int *);
    int (*getPci)(nvmlDevice_t, nvmlPciInfo_t *);
};

static void *sym2(void *lib, const char *a, const char *b) {
    void *p = dlsym(lib, a);
    return (p || !b) ? p : dlsym(lib, b);
}

/* Loaded at runtime so there is no build-time dependency, and machines without the
   NVIDIA driver are unaffected. */
static unsigned int nvml_count(GpuNvml *n);

static void *open_nvml_lib(FILE *log) {
    /* The loader's normal search first, then where WSL2 puts the Windows driver's Linux
       libraries: /usr/lib/wsl/lib is often missing from the loader path on WSL. */
    static const char *const names[] = {
        "libnvidia-ml.so.1", "libnvidia-ml.so", "/usr/lib/wsl/lib/libnvidia-ml.so.1", NULL
    };
    for (int i = 0; names[i]; i++) {
        void *lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            LOG(log, "NVML: loaded %s\n", names[i]);
            return lib;
        }
        const char *err = dlerror();
        LOG(log, "NVML: could not load %s (%s)\n", names[i], err ? err : "unknown error");
    }

    glob_t gl;
    if (glob("/usr/lib/wsl/drivers/*/libnvidia-ml.so.1", 0, NULL, &gl) == 0) {
        for (size_t i = 0; i < gl.gl_pathc; i++) {
            void *lib = dlopen(gl.gl_pathv[i], RTLD_LAZY | RTLD_LOCAL);
            if (lib) {
                LOG(log, "NVML: loaded %s\n", gl.gl_pathv[i]);
                globfree(&gl);
                return lib;
            }
            LOG(log, "NVML: could not load %s\n", gl.gl_pathv[i]);
        }
        globfree(&gl);
    } else {
        LOG(log, "NVML: nothing matching /usr/lib/wsl/drivers/*/libnvidia-ml.so.1\n");
    }
    return NULL;
}

static GpuNvml *nvml_load(FILE *log) {
    void *lib = open_nvml_lib(log);
    if (!lib) return NULL;

    GpuNvml *n = calloc(1, sizeof *n);
    if (!n) { dlclose(lib); return NULL; }
    n->lib = lib;
    *(void **)&n->init      = sym2(lib, "nvmlInit_v2", "nvmlInit");
    *(void **)&n->shutdown  = sym2(lib, "nvmlShutdown", NULL);
    *(void **)&n->getCount  = sym2(lib, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    *(void **)&n->getHandle = sym2(lib, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    *(void **)&n->getName   = sym2(lib, "nvmlDeviceGetName", NULL);
    *(void **)&n->getUtil   = sym2(lib, "nvmlDeviceGetUtilizationRates", NULL);
    *(void **)&n->getMem    = sym2(lib, "nvmlDeviceGetMemoryInfo", NULL);
    *(void **)&n->getTemp   = sym2(lib, "nvmlDeviceGetTemperature", NULL);
    *(void **)&n->getPci    = sym2(lib, "nvmlDeviceGetPciInfo_v3", "nvmlDeviceGetPciInfo");

    if (!n->init || !n->shutdown || !n->getCount || !n->getHandle || !n->getUtil ||
        !n->getMem || !n->getTemp) {
        LOG(log, "NVML: the library is missing functions this program needs\n");
        dlclose(lib);
        free(n);
        return NULL;
    }
    int rc = n->init();
    if (rc != 0) {
        LOG(log, "NVML: nvmlInit failed with error %d (is the NVIDIA driver running?)\n", rc);
        dlclose(lib);
        free(n);
        return NULL;
    }
    LOG(log, "NVML: initialised, %u device(s)\n", nvml_count(n));
    return n;
}

static unsigned int nvml_count(GpuNvml *n) {
    unsigned int c = 0;
    if (!n || n->getCount(&c) != 0) return 0;
    return c;
}

static void nvml_discover(GpuList *l, const GpuPaths *p) {
    GpuNvml *n = l->nvml;
    const char *pci = p->pci ? p->pci : DEFAULT_PCI;

    for (unsigned int i = 0, count = nvml_count(n); i < count; i++) {
        nvmlDevice_t dev;
        if (n->getHandle(i, &dev) != 0) continue;
        GpuInfo *g = add_gpu(l);
        if (!g) return;

        g->vendor = GPU_NVIDIA;
        g->usageSrc = SRC_NVML;
        g->nvmlDev = dev;

        char raw[64] = "";
        if (n->getName && n->getName(dev, raw, sizeof raw) == 0 && raw[0]) {
            const char *nm = raw;
            if (strncmp(nm, "NVIDIA ", 7) == 0) nm += 7;          /* "GeForce RTX 3080" is enough */
            snprintf(g->name, sizeof g->name, "%s", nm);
        } else {
            fallback_name(GPU_NVIDIA, g->name, sizeof g->name);
        }

        /* The PCI address tells us where its runtime power state lives */
        nvmlPciInfo_t info;
        memset(&info, 0, sizeof info);
        if (n->getPci && n->getPci(dev, &info) == 0) {
            char addr[32], dir[192];
            snprintf(addr, sizeof addr, "%04x:%02x:%02x.0", info.domain, info.bus, info.device);
            if (join(dir, sizeof dir, pci, addr) == 0) join(g->pathPower, sizeof g->pathPower, dir, "power/runtime_status");
        }
    }
}

static void nvml_update_one(GpuNvml *n, GpuInfo *g) {
    nvmlDevice_t dev = g->nvmlDev;

    nvmlUtilization_t u;
    if (n->getUtil(dev, &u) == 0) {
        g->hasUsage = 1;
        g->usagePercent = (float)u.gpu;
    }
    nvmlMemory_t m;
    if (n->getMem(dev, &m) == 0 && m.total > 0) {
        g->hasMem = 1;
        g->memUsed = size_from_bytes((double)m.used);
        g->memTotal = size_from_bytes((double)m.total);
        g->memPercent = 100.0f * (float)((double)m.used / (double)m.total);
    }
    unsigned int t;
    if (n->getTemp(dev, 0 /* NVML_TEMPERATURE_GPU */, &t) == 0) {
        g->hasTemp = 1;
        g->tempC = (float)t;
    }
}

/* ------------------------------------------------------------------ */
/* AMD and Intel (sysfs)                                              */
/* ------------------------------------------------------------------ */

static void find_hwmon_temp(GpuInfo *g) {
    char dir[192];
    if (join(dir, sizeof dir, g->pathBase, "hwmon") != 0) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
        char hw[192], t[192];
        if (join(hw, sizeof hw, dir, e->d_name) != 0) continue;
        static const char *const temps[] = { "temp1_input", "temp2_input", NULL };
        if (first_readable(t, sizeof t, hw, temps)) {
            copy_path(g->pathTemp, sizeof g->pathTemp, t);
            break;
        }
    }
    closedir(d);
}

static void setup_amd(GpuInfo *g) {
    char f[192];
    if (join(f, sizeof f, g->pathBase, "gpu_busy_percent") == 0 && readable(f)) {
        copy_path(g->pathA, sizeof g->pathA, f);
        g->usageSrc = SRC_AMD_BUSY;
    }
    char u[192], t[192];
    if (join(u, sizeof u, g->pathBase, "mem_info_vram_used") == 0 && readable(u) &&
        join(t, sizeof t, g->pathBase, "mem_info_vram_total") == 0 && readable(t)) {
        copy_path(g->pathVramUsed, sizeof g->pathVramUsed, u);
        copy_path(g->pathVramTotal, sizeof g->pathVramTotal, t);
    }
}

/* Intel exposes no plain "busy %" without privileged perf events, so this is an estimate:
   the share of time the GPU was out of its RC6 sleep state, else actual/max clock speed. */
static void setup_intel(GpuInfo *g) {
    static const char *const rc6Card[] = { "gt/gt0/rc6_residency_ms", "power/rc6_residency_ms", NULL };
    static const char *const rc6Xe[]   = { "tile0/gt0/gtidle/idle_residency_ms", NULL };
    static const char *const actCard[] = { "gt/gt0/rps_act_freq_mhz", "gt_act_freq_mhz", NULL };
    static const char *const actXe[]   = { "tile0/gt0/freq0/act_freq", NULL };
    static const char *const maxCard[] = { "gt/gt0/rps_RP0_freq_mhz", "gt_RP0_freq_mhz", "gt_max_freq_mhz", NULL };
    static const char *const maxXe[]   = { "tile0/gt0/freq0/max_freq", NULL };

    char rc6[192];
    long long v = 0;
    if ((first_readable(rc6, sizeof rc6, g->pathCard, rc6Card) ||
         first_readable(rc6, sizeof rc6, g->pathBase, rc6Xe)) &&
        read_ll(rc6, &v) == 0 && v > 0) {              /* 0 means RC6 is disabled: useless as a signal */
        copy_path(g->pathA, sizeof g->pathA, rc6);
        g->usageSrc = SRC_INTEL_RC6;
    } else {
        char act[192], max[192];
        int haveAct = first_readable(act, sizeof act, g->pathCard, actCard) ||
                      first_readable(act, sizeof act, g->pathBase, actXe);
        int haveMax = first_readable(max, sizeof max, g->pathCard, maxCard) ||
                      first_readable(max, sizeof max, g->pathBase, maxXe);
        if (haveAct && haveMax) {
            copy_path(g->pathA, sizeof g->pathA, act);
            copy_path(g->pathB, sizeof g->pathB, max);
            g->usageSrc = SRC_INTEL_FREQ;
        }
    }

    /* Arc cards have local memory; integrated GPUs share system RAM and have none */
    static const char *const lmemTotal[] = { "lmem_total_bytes", NULL };
    static const char *const lmemAvail[] = { "lmem_avail_bytes", NULL };
    char tot[192], avl[192];
    const char *roots[2] = { g->pathCard, g->pathBase };
    for (int i = 0; i < 2; i++) {
        if (first_readable(tot, sizeof tot, roots[i], lmemTotal) &&
            first_readable(avl, sizeof avl, roots[i], lmemAvail)) {
            copy_path(g->pathVramTotal, sizeof g->pathVramTotal, tot);
            copy_path(g->pathVramUsed, sizeof g->pathVramUsed, avl);   /* holds "available", see update */
            break;
        }
    }
}

typedef struct { int num; char name[32]; } CardEnt;

static int card_number(const char *name) {
    if (strncmp(name, "card", 4) != 0 || !isdigit((unsigned char)name[4])) return -1;
    char *end;
    long v = strtol(name + 4, &end, 10);
    return (*end == '\0') ? (int)v : -1;                     /* skips "card0-HDMI-A-1" connectors */
}

static int cmp_card(const void *a, const void *b) {
    return ((const CardEnt *)a)->num - ((const CardEnt *)b)->num;
}

static void discover_sysfs(GpuList *l, const GpuPaths *p, int skipNvidia) {
    const char *drm = p->drm ? p->drm : DEFAULT_DRM;
    DIR *d = opendir(drm);
    if (!d) {
        LOG(p->log, "sysfs: %s does not exist (no DRM devices; normal on WSL2)\n", drm);
        return;
    }

    CardEnt cards[64];
    int nc = 0;
    struct dirent *e;
    while (nc < 64 && (e = readdir(d)) != NULL) {
        int num = card_number(e->d_name);
        if (num < 0 || strlen(e->d_name) >= sizeof cards[0].name) continue;
        cards[nc].num = num;
        memcpy(cards[nc].name, e->d_name, strlen(e->d_name) + 1);
        nc++;
    }
    closedir(d);
    qsort(cards, (size_t)nc, sizeof cards[0], cmp_card);

    for (int i = 0; i < nc; i++) {
        char card[192], base[192], f[192];
        if (join(card, sizeof card, drm, cards[i].name) != 0 || join(base, sizeof base, card, "device") != 0) continue;

        unsigned long vendor, device = 0, cls = 0x030000;
        if (join(f, sizeof f, base, "vendor") != 0 || read_hex(f, &vendor) != 0) {
            LOG(p->log, "sysfs: %s: no PCI vendor id, ignored\n", cards[i].name);
            continue;
        }
        if (join(f, sizeof f, base, "device") == 0) read_hex(f, &device);
        if (join(f, sizeof f, base, "class") == 0 && read_hex(f, &cls) == 0 && (cls >> 16) != 0x03) {
            LOG(p->log, "sysfs: %s: vendor 0x%04lx is not a display controller, ignored\n", cards[i].name, vendor);
            continue;
        }

        GpuVendor gv = vendor == PCI_VENDOR_NVIDIA ? GPU_NVIDIA :
                       vendor == PCI_VENDOR_AMD    ? GPU_AMD :
                       vendor == PCI_VENDOR_INTEL  ? GPU_INTEL : GPU_UNKNOWN;
        if (gv == GPU_UNKNOWN) {                             /* virtio, vmwgfx, simple framebuffers ... */
            LOG(p->log, "sysfs: %s: vendor 0x%04lx is not NVIDIA, AMD or Intel, ignored\n", cards[i].name, vendor);
            continue;
        }
        if (gv == GPU_NVIDIA && skipNvidia) {                /* NVML already lists it, with real numbers */
            LOG(p->log, "sysfs: %s: NVIDIA, skipped because NVML lists it\n", cards[i].name);
            continue;
        }
        LOG(p->log, "sysfs: %s: vendor 0x%04lx device 0x%04lx, added\n", cards[i].name, vendor, device);

        GpuInfo *g = add_gpu(l);
        if (!g) break;
        g->vendor = gv;
        copy_path(g->pathCard, sizeof g->pathCard, card);
        copy_path(g->pathBase, sizeof g->pathBase, base);
        if (join(f, sizeof f, base, "power/runtime_status") == 0) copy_path(g->pathPower, sizeof g->pathPower, f);

        char raw[160];
        if (pci_ids_lookup(p, (unsigned)vendor, (unsigned)device, raw, sizeof raw))
            model_name(raw, gv, g->name, sizeof g->name);
        else
            fallback_name(gv, g->name, sizeof g->name);

        find_hwmon_temp(g);
        if (gv == GPU_AMD)   setup_amd(g);
        if (gv == GPU_INTEL) setup_intel(g);
        /* nouveau (NVIDIA without NVML) keeps just the name and a hwmon temperature */
    }
}

static void update_sysfs(GpuInfo *g, double nowMs) {
    long long v;

    switch (g->usageSrc) {
    case SRC_AMD_BUSY:
        if (read_ll(g->pathA, &v) == 0) {
            g->hasUsage = 1;
            g->usagePercent = (float)v;
        }
        break;

    case SRC_INTEL_RC6:
        g->usageEstimated = 1;
        if (read_ll(g->pathA, &v) == 0) {
            if (g->haveLast > 0 && nowMs > g->lastTime && (double)v >= g->lastCounter) {
                double asleep = ((double)v - g->lastCounter) / (nowMs - g->lastTime);
                if (asleep < 0) asleep = 0;
                if (asleep > 1) asleep = 1;
                g->usagePercent = (float)(100.0 * (1.0 - asleep));
                g->hasUsage = 1;
            }
            g->lastCounter = (double)v;
            g->lastTime = nowMs;
            if (g->haveLast == 0) g->haveLast = 1;
        }
        break;

    case SRC_INTEL_FREQ: {
        long long act, max;
        g->usageEstimated = 1;
        if (read_ll(g->pathA, &act) == 0 && read_ll(g->pathB, &max) == 0 && max > 0) {
            double u = 100.0 * (double)act / (double)max;
            g->usagePercent = (float)(u > 100.0 ? 100.0 : u);
            g->hasUsage = 1;
        }
        break;
    }
    default:
        break;
    }

    if (g->pathVramTotal[0]) {
        long long total, used;
        if (read_ll(g->pathVramTotal, &total) == 0 && read_ll(g->pathVramUsed, &used) == 0 && total > 0) {
            if (g->vendor == GPU_INTEL) used = total - used;      /* Intel reports what is available */
            if (used < 0) used = 0;
            g->hasMem = 1;
            g->memUsed = size_from_bytes((double)used);
            g->memTotal = size_from_bytes((double)total);
            g->memPercent = 100.0f * (float)((double)used / (double)total);
        }
    }
    if (g->pathTemp[0] && read_ll(g->pathTemp, &v) == 0) {
        g->hasTemp = 1;
        g->tempC = (float)v / 1000.0f;                            /* hwmon is in millidegrees */
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* A GPU with no usage counter, no memory figures and no temperature has nothing to show.
   (The check is on what can be read, not on the latest reading, so a GPU never flickers away.) */
static int has_data_source(const GpuInfo *g) {
    return g->usageSrc != SRC_NONE || g->pathVramTotal[0] || g->pathTemp[0];
}

/* Outside --gpu-info such devices are dropped, so they don't take up space in the display */
static void drop_dataless(GpuList *l) {
    size_t out = 0;
    for (size_t i = 0; i < l->count; i++)
        if (has_data_source(&l->items[i])) {
            if (out != i) l->items[out] = l->items[i];
            out++;
        }
    l->count = out;
}

static const char *source_name(int src) {
    switch (src) {
        case SRC_NVML:        return "NVML";
        case SRC_AMD_BUSY:    return "amdgpu gpu_busy_percent";
        case SRC_INTEL_RC6:   return "RC6 residency (estimate)";
        case SRC_INTEL_FREQ:  return "clock speed (estimate)";
        default:              return "no usage counter";
    }
}

int gpu_init_with(GpuList *list, const GpuPaths *paths) {
    memset(list, 0, sizeof *list);
    FILE *log = paths->log;

    list->nvml = nvml_load(log);
    if (list->nvml && nvml_count(list->nvml) == 0) {           /* driver present but no devices */
        LOG(log, "NVML: loaded but reports no devices\n");
        list->nvml->shutdown();
        dlclose(list->nvml->lib);
        free(list->nvml);
        list->nvml = NULL;
    }

    discover_sysfs(list, paths, list->nvml != NULL);
    if (list->nvml) nvml_discover(list, paths);

    /* WSL2 passes the GPU through as /dev/dxg (DirectX). Linux gets no usage, memory or
       temperature from that, so it is only mentioned in the --gpu-info report. */
    int wsl = access(WSL_DXG, F_OK) == 0;
    LOG(log, "WSL2: %s %s\n", WSL_DXG, wsl ? "exists" : "does not exist");
    if (log && list->count == 0 && wsl) {
        GpuInfo *g = add_gpu(list);
        if (g) {
            copy_path(g->name, sizeof g->name, "DirectX GPU (WSL2)");
            LOG(log, "WSL2: the GPU is only reachable through DirectX; Linux can read no statistics from it\n");
        }
    }
    if (!log) drop_dataless(list);

    gpu_update(list);                                          /* baseline for the RC6 counters */

    if (log) {
        static const char *const vn[] = { "unknown vendor", "NVIDIA", "AMD", "Intel" };
        fprintf(log, "\nResult: %zu GPU(s)\n", list->count);
        for (size_t i = 0; i < list->count; i++) {
            const GpuInfo *g = &list->items[i];
            fprintf(log, "  GPU%zu  %-14s %s\n        source: %s", i, vn[g->vendor], g->name, source_name(g->usageSrc));
            if (g->suspended) fprintf(log, " (asleep, not queried)");
            else if (g->hasUsage) fprintf(log, ", usage %.0f%%", g->usagePercent);
            if (g->hasMem) fprintf(log, ", memory %.1f%c of %.1f%c", g->memUsed.value, g->memUsed.unit[0], g->memTotal.value, g->memTotal.unit[0]);
            if (g->hasTemp) fprintf(log, ", %.0f C", g->tempC);
            fprintf(log, "\n");
            if (!has_data_source(g)) fprintf(log, "        nothing to read from this device, so it is not shown in the normal display\n");
        }
    }
    return 0;
}

int gpu_init(GpuList *list) {
    GpuPaths defaults = { NULL, NULL, NULL, NULL };
    return gpu_init_with(list, &defaults);
}

int gpu_update(GpuList *list) {
    double now = now_ms();

    for (size_t i = 0; i < list->count; i++) {
        GpuInfo *g = &list->items[i];
        g->hasUsage = g->hasMem = g->hasTemp = 0;
        g->suspended = is_suspended(g->pathPower);
        if (g->suspended) {
            g->usagePercent = 0;                               /* asleep counts as idle for the graph */
            g->haveLast = 0;                                   /* counters restart after a wake-up */
            continue;
        }
        if (g->usageSrc == SRC_NVML && list->nvml) nvml_update_one(list->nvml, g);
        else                                       update_sysfs(g, now);
    }
    return 0;
}

void gpu_free(GpuList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    if (list->nvml) {
        list->nvml->shutdown();
        dlclose(list->nvml->lib);
        free(list->nvml);
        list->nvml = NULL;
    }
}