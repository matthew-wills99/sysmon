#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "process.h"
#include "utils.h"

/* 0: CPU% is a share of the whole machine (0-100), consistent with the overall CPU figure.
   1: CPU% is a share of a single core, like top (a busy multi-threaded process can exceed 100). */
#define PER_CORE_PERCENT 0

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static double seconds_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static int compare_pid(const void *a, const void *b) {
    int pa = ((const ProcessInfo *)a)->pid;
    int pb = ((const ProcessInfo *)b)->pid;
    return (pa > pb) - (pa < pb);
}

/* Highest CPU first, then highest memory, then lowest pid */
static int compare_cpu_desc(const void *a, const void *b) {
    const ProcessInfo *pa = *(const ProcessInfo *const *)a;
    const ProcessInfo *pb = *(const ProcessInfo *const *)b;

    if (pa->CPUPercent != pb->CPUPercent) return (pa->CPUPercent < pb->CPUPercent) ? 1 : -1;
    if (pa->memPercent != pb->memPercent) return (pa->memPercent < pb->memPercent) ? 1 : -1;
    return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}

static int add_process(ProcessList *list, const ProcessInfo *p) {
    /* if the process list is already at capacity, increase it or initialize it if needed */
    if (list->count == list->capacity) {
        /* maintain a capacity that is a power of 2 */
        size_t newCapacity = list->capacity ? list->capacity * 2 : 256;
        ProcessInfo *tmp = realloc(list->items, newCapacity * sizeof *tmp);
        if (!tmp) return -1;
        list->items = tmp;
        list->capacity = newCapacity;
    }
    list->items[list->count++] = *p;
    return 0;
}

/* Fills in pid, name, cpuTicks and the memory figures from /proc/<pid>/stat.
   Returns -1 if the process is gone or unreadable. */
static int read_process(int pid, const ProcessList *list, ProcessInfo *p) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);

    char *data = read_info_file(path, NULL);
    if (!data) return -1;

    /* Format: "pid (comm) state ppid ...". comm can contain spaces and even
       parentheses, so find the first '(' and the LAST ')'. */
    char *open  = strchr(data, '(');
    char *close = strrchr(data, ')');
    if (!open || !close || close < open) {
        free(data);
        return -1;
    }

    memset(p, 0, sizeof *p);
    p->pid = pid;

    size_t nameLen = (size_t)(close - open - 1);
    if (nameLen >= sizeof p->name) nameLen = sizeof p->name - 1;
    memcpy(p->name, open + 1, nameLen);
    p->name[nameLen] = '\0';

    /* After ')': fields 3..13 are skipped, 14 = utime, 15 = stime,
       16..23 are skipped, 24 = rss (in pages). */
    unsigned long long utime, stime;
    long rssPages;
    int n = sscanf(close + 1,
                   "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "   /* fields 3-13 */
                   "%llu %llu "                                     /* 14, 15 */
                   "%*s %*s %*s %*s %*s %*s %*s %*s "               /* 16-23 */
                   "%ld",                                           /* 24 */
                   &utime, &stime, &rssPages);
    free(data);
    if (n != 3) return -1;

    if (rssPages < 0) rssPages = 0;
    unsigned long long usedBytes = (unsigned long long)rssPages * (unsigned long long)list->pageSize;

    p->cpuTicks = utime + stime;
    p->memUsed  = size_from_bytes((double)usedBytes);
    p->memTotal = size_from_bytes((double)list->memTotalBytes);
    p->memPercent = (list->memTotalBytes > 0)
                  ? 100.0f * (float)usedBytes / (float)list->memTotalBytes
                  : 0.0f;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int process_init(ProcessList *list) {
    memset(list, 0, sizeof *list);

    list->pageSize = sysconf(_SC_PAGESIZE);
    if (list->pageSize <= 0) list->pageSize = 4096;

    static const char *include[] = { "MemTotal" };
    kv_pair *entries;
    size_t count = parse_kv_file("/proc/meminfo", include, 1, &entries);
    if (count == (size_t)-1) {
        perror("parse_kv_file");
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].label, "MemTotal") == 0)
            list->memTotalBytes = strtoull(entries[i].value, NULL, 10) * 1024ULL;   /* kB -> bytes */
    }
    free_kv_pairs(entries, count);

    if (list->memTotalBytes == 0) {
        fprintf(stderr, "process_init: could not read MemTotal\n");
        return 1;
    }

    /* Baseline sample; CPU usage stays 0 until the next process_update() */
    if (process_update(list) != 0) {
        process_free(list);
        return 1;
    }
    return 0;
}

int process_update(ProcessList *list) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = list->hasSample ? seconds_between(&list->lastSample, &now) : 0.0;

    /* Build the new list on the side so the old one (and its tick counts) stay
       intact for comparison, and stay valid if anything fails. */
    ProcessList next = {0};
    struct dirent *e;

    while ((e = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;   /* only pid directories */

        char *endp;
        long pid = strtol(e->d_name, &endp, 10);
        if (*endp != '\0' || pid <= 0) continue;

        ProcessInfo p;
        if (read_process((int)pid, list, &p) != 0) continue;   /* exited while we were reading */

        if (add_process(&next, &p) != 0) {
            closedir(proc);
            free(next.items);
            return -1;
        }
    }
    closedir(proc);

    /* Sorted by pid so the previous sample can be looked up with bsearch */
    qsort(next.items, next.count, sizeof *next.items, compare_pid);

    if (dt > 0 && list->count > 0) {
        double clkTck = (double)sysconf(_SC_CLK_TCK);
        long cpus = PER_CORE_PERCENT ? 1 : sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 1) cpus = 1;
        double capacityTicks = dt * clkTck * (double)cpus;

        for (size_t i = 0; i < next.count; i++) {
            ProcessInfo *p = &next.items[i];
            const ProcessInfo *prev = bsearch(p, list->items, list->count,
                                              sizeof *list->items, compare_pid);
            if (prev && p->cpuTicks >= prev->cpuTicks) {
                p->CPUPercent = (float)((double)(p->cpuTicks - prev->cpuTicks) / capacityTicks * 100.0);
            }
        }
    }

    free(list->items);
    list->items    = next.items;
    list->count    = next.count;
    list->capacity = next.capacity;
    list->lastSample = now;
    list->hasSample = 1;
    return 0;
}

void process_print(const ProcessList *list, size_t limit) {
    if (list->count == 0) {
        printf("Processes: none\n");
        return;
    }

    /* Sort pointers, not the list itself, since the list must stay ordered by pid */
    const ProcessInfo **view = malloc(list->count * sizeof *view);
    if (!view) return;
    for (size_t i = 0; i < list->count; i++) view[i] = &list->items[i];
    qsort(view, list->count, sizeof *view, compare_cpu_desc);

    size_t rows = (limit == 0 || limit > list->count) ? list->count : limit;

    printf("Processes: %zu total\n", list->count);
    printf("%7s  %-16s %6s  %6s  %s\n", "PID", "NAME", "CPU%", "MEM%", "MEMORY");
    for (size_t i = 0; i < rows; i++) {
        const ProcessInfo *p = view[i];
        printf("%7d  %-16s %5.1f%%  %5.1f%%  %.2f %s / %.2f %s\n",
               p->pid, p->name, p->CPUPercent, p->memPercent,
               p->memUsed.value, p->memUsed.unit,
               p->memTotal.value, p->memTotal.unit);
    }
    free(view);
}

void process_free(ProcessList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}