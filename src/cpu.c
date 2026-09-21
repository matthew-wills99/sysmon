#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "utils.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Labels we want from /proc/cpuinfo when discovering the topology */
static const char *include_cpuinfo[] = {
    "processor", "model name", "siblings", "core id", "cpu cores"
};

/* Labels we want from /proc/cpuinfo when refreshing clock speeds */
static const char *include_clock[] = {
    "processor", "cpu MHz"
};

/* Rows we want from /proc/stat (any label containing "cpu" is also kept) */
static const char *include_stat[] = {
    "cpu", "btime"
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Parses the numbers following the label on a /proc/stat cpu row.
   total = sum of every column, idle = 5th column (4th number).
   Returns how many numbers were read. */
static int parse_cpu_row(const char *rest, unsigned long long *total,
                         unsigned long long *idle) {
    *total = 0;
    *idle = 0;

    int col = 0;
    const char *p = rest;
    char *endp;

    for (;;) {
        unsigned long long v = strtoull(p, &endp, 10);
        if (endp == p) break;   /* no more numbers */
        *total += v;
        if (col == 3) *idle = v;
        col++;
        p = endp;
    }
    return col;
}

static float usage_percent(unsigned long long total, unsigned long long idle,
                           unsigned long long prevTotal, unsigned long long prevIdle) {
    unsigned long long dTotal = total - prevTotal;
    unsigned long long dIdle  = idle  - prevIdle;
    if (dTotal == 0 || dIdle > dTotal) return 0.0f;
    return 100.0f * (float)(dTotal - dIdle) / (float)dTotal;
}

/* Adds one thread's counters to the core it belongs to. */
static void add_thread_to_core(CpuInfo *cpu, long thread,
                               unsigned long long total, unsigned long long idle) {
    if (thread < 0 || thread >= MAX_THREADS) return;

    int coreId = cpu->threadCoreId[thread];
    if (coreId < 0) return;

    int idx = cpu->coreIndexById[coreId];
    if (idx < 0) return;

    cpu->cores[idx].total += total;
    cpu->cores[idx].idle  += idle;
}

/* Re-reads /proc/cpuinfo and sets each core's clockSpeed to the average of
   its threads' current MHz. */
static int refresh_clocks(CpuInfo *cpu) {
    kv_pair *entries;
    size_t count = parse_kv_file("/proc/cpuinfo", include_clock,
                                 ARRAY_LEN(include_clock), &entries);
    if (count == (size_t)-1) return -1;

    float mhzSum[MAX_CORES] = {0};
    int samples[MAX_CORES] = {0};
    long proc = -1;

    for (size_t i = 0; i < count; i++) {
        const char *label = entries[i].label;
        const char *value = entries[i].value;

        if (strcmp(label, "processor") == 0) {
            proc = strtol(value, NULL, 10);
        } else if (strcmp(label, "cpu MHz") == 0 && proc >= 0 && proc < MAX_THREADS) {
            int coreId = cpu->threadCoreId[proc];
            if (coreId < 0) continue;

            int idx = cpu->coreIndexById[coreId];
            if (idx < 0) continue;

            mhzSum[idx] += strtof(value, NULL);
            samples[idx]++;
        }
    }
    free_kv_pairs(entries, count);

    for (size_t i = 0; i < cpu->coreCount; i++) {
        if (samples[i] > 0) cpu->cores[i].clockSpeed = mhzSum[i] / (float)samples[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int cpu_init(CpuInfo *cpu) {
    memset(cpu, 0, sizeof *cpu);
    memset(cpu->threadCoreId, 0xFF, sizeof cpu->threadCoreId);     /* all -1 */
    memset(cpu->coreIndexById, 0xFF, sizeof cpu->coreIndexById);   /* all -1 */

    kv_pair *entries;
    size_t count = parse_kv_file("/proc/cpuinfo", include_cpuinfo,
                                 ARRAY_LEN(include_cpuinfo), &entries);
    if (count == (size_t)-1) {
        perror("parse_kv_file");
        return 1;
    }

    int coreSeen[MAX_CORES] = {0};
    long proc = -1;

    for (size_t i = 0; i < count; i++) {
        const char *label = entries[i].label;
        const char *value = entries[i].value;

        /* Single-read statistics */
        if (!cpu->name[0] && strcmp(label, "model name") == 0) {
            snprintf(cpu->name, sizeof cpu->name, "%s", value);
        } else if (cpu->threads == 0 && strcmp(label, "siblings") == 0) {
            cpu->threads = (int)strtol(value, NULL, 10);
        } else if (cpu->physicalCores == 0 && strcmp(label, "cpu cores") == 0) {
            cpu->physicalCores = (int)strtol(value, NULL, 10);
        }

        /* Topology: which thread belongs to which core */
        else if (strcmp(label, "processor") == 0) {
            proc = strtol(value, NULL, 10);
        } else if (strcmp(label, "core id") == 0) {
            long id = strtol(value, NULL, 10);
            if (id >= 0 && id < MAX_CORES) {
                coreSeen[id] = 1;
                if (proc >= 0 && proc < MAX_THREADS) cpu->threadCoreId[proc] = (int)id;
            }
        }
    }
    free_kv_pairs(entries, count);

    for (int id = 0; id < MAX_CORES; id++) {
        if (coreSeen[id]) cpu->coreCount++;
    }
    if (cpu->coreCount == 0) {
        fprintf(stderr, "cpu_init: no cores found in /proc/cpuinfo\n");
        return 1;
    }

    cpu->cores = calloc(cpu->coreCount, sizeof *cpu->cores);
    if (!cpu->cores) {
        cpu->coreCount = 0;
        return 1;
    }

    size_t n = 0;
    for (int id = 0; id < MAX_CORES; id++) {
        if (!coreSeen[id]) continue;
        snprintf(cpu->cores[n].name, sizeof cpu->cores[n].name, "core%d", id);
        cpu->coreIndexById[id] = (int)n;
        n++;
    }

    /* Baseline sample. Until a second cpu_update() call, usage reflects the
       average since boot. This also fills in the initial clock speeds. */
    if (cpu_update(cpu) != 0) {
        cpu_free(cpu);
        return 1;
    }
    return 0;
}

int cpu_update(CpuInfo *cpu) {
    size_t fileLen;
    char *data = read_info_file("/proc/stat", &fileLen);
    if (!data) return -1;

    /* Roll the previous sample over and reset the accumulators */
    cpu->prevTotal = cpu->total;
    cpu->prevIdle  = cpu->idle;
    for (size_t i = 0; i < cpu->coreCount; i++) {
        cpu->cores[i].prevTotal = cpu->cores[i].total;
        cpu->cores[i].prevIdle  = cpu->cores[i].idle;
        cpu->cores[i].total = 0;
        cpu->cores[i].idle  = 0;
    }

    const char *p   = data;
    const char *end = data + fileLen;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;

        /* First column: skip leading whitespace, read to the next whitespace */
        const char *id = p;
        while (id < eol && (*id == ' ' || *id == '\t')) id++;
        const char *idEnd = id;
        while (idEnd < eol && *idEnd != ' ' && *idEnd != '\t') idEnd++;
        size_t idLen = (size_t)(idEnd - id);

        p = (eol < end) ? eol + 1 : end;   /* advance now so `continue` is safe */

        char label[64];
        if (idLen == 0 || idLen >= sizeof label) continue;
        memcpy(label, id, idLen);
        label[idLen] = '\0';

        if (!strstr(label, "cpu") &&
            !is_included(label, include_stat, ARRAY_LEN(include_stat)))
            continue;

        /* Null-terminated copy of the rest of the row so strtoull can't run
           past the end of it into the next row */
        char rest[512];
        size_t restLen = (size_t)(eol - idEnd);
        if (restLen >= sizeof rest) restLen = sizeof rest - 1;
        memcpy(rest, idEnd, restLen);
        rest[restLen] = '\0';

        if (strncmp(label, "cpu", 3) == 0) {
            unsigned long long total, idle;
            if (parse_cpu_row(rest, &total, &idle) < 4) continue;

            if (label[3] == '\0') {
                /* "cpu" row: overall totals */
                cpu->total = total;
                cpu->idle  = idle;
            } else if (isdigit((unsigned char)label[3])) {
                /* "cpuN" row: add this thread to its core */
                add_thread_to_core(cpu, strtol(label + 3, NULL, 10), total, idle);
            }
        } else if (strcmp(label, "btime") == 0) {
            cpu->bootTime = strtoull(rest, NULL, 10);
        }
    }
    free(data);

    /* Uptime = current wall-clock time minus the boot time from /proc/stat */
    time_t now = time(NULL);
    cpu->uptimeSeconds = (cpu->bootTime > 0 && (unsigned long long)now > cpu->bootTime)
                       ? (unsigned long long)now - cpu->bootTime
                       : 0;

    cpu->usagePercent = usage_percent(cpu->total, cpu->idle,
                                      cpu->prevTotal, cpu->prevIdle);
    for (size_t i = 0; i < cpu->coreCount; i++) {
        cpu->cores[i].usagePercent = usage_percent(cpu->cores[i].total, cpu->cores[i].idle,
                                                   cpu->cores[i].prevTotal, cpu->cores[i].prevIdle);
    }

    return refresh_clocks(cpu);
}

/* 93784 -> "1d 02h 03m 04s" */
static void format_uptime(unsigned long long secs, char *buf, size_t n) {
    unsigned long long d = secs / 86400;
    unsigned long long h = (secs % 86400) / 3600;
    unsigned long long m = (secs % 3600) / 60;
    unsigned long long s = secs % 60;

    if (d > 0) snprintf(buf, n, "%llud %02lluh %02llum %02llus", d, h, m, s);
    else       snprintf(buf, n, "%02lluh %02llum %02llus", h, m, s);
}

void cpu_print(const CpuInfo *cpu) {
    char uptime[48];
    format_uptime(cpu->uptimeSeconds, uptime, sizeof uptime);

    char boot[32] = "unknown";
    time_t t = (time_t)cpu->bootTime;
    struct tm *tm = localtime(&t);
    if (tm) strftime(boot, sizeof boot, "%Y-%m-%d %H:%M:%S", tm);

    printf("CPU:              %s\n", cpu->name);
    printf("Physical cores:   %d\n", cpu->physicalCores);
    printf("Threads:          %d\n", cpu->threads);
    printf("Cores detected:   %zu\n", cpu->coreCount);
    printf("Boot time:        %s\n", boot);
    printf("Uptime:           %s\n", uptime);
    printf("Total usage:      %.1f%%\n\n", cpu->usagePercent);

    for (size_t i = 0; i < cpu->coreCount; i++) {
        printf("%-8s  %8.2f MHz  %5.1f%%\n",
               cpu->cores[i].name,
               cpu->cores[i].clockSpeed,
               cpu->cores[i].usagePercent);
    }
}

void cpu_free(CpuInfo *cpu) {
    free(cpu->cores);
    cpu->cores = NULL;
    cpu->coreCount = 0;
}