#define _GNU_SOURCE

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "utils.h"

#define SYS_NET "/sys/class/net"

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static double seconds_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static int compare_interfaces(const void *a, const void *b) {
    return strcmp(((const NetworkInfo *)a)->name, ((const NetworkInfo *)b)->name);
}

/* ------------------------------------------------------------------ */
/* Discovering physical interfaces                                    */
/* ------------------------------------------------------------------ */

/* An interface is physical if /sys/class/net/<name>/device exists. This rules
   out lo, bridges, veth pairs, docker0, tun/tap, VPNs and other virtual links. */
static int discover_interfaces(NetworkList *list, int includeVirtual) {
    DIR *d = opendir(SYS_NET);
    if (!d) {
        perror("opendir " SYS_NET);
        return -1;
    }

    size_t cap = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        size_t nameLen = strlen(e->d_name);
        if (nameLen >= sizeof list->items[0].name) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, SYS_NET "/%s/device", e->d_name);
        if (!includeVirtual && access(path, F_OK) != 0) continue;

        if (list->count == cap) {
            size_t newCap = cap ? cap * 2 : 4;
            NetworkInfo *tmp = realloc(list->items, newCap * sizeof *tmp);
            if (!tmp) {
                closedir(d);
                net_free(list);
                return -1;
            }
            list->items = tmp;
            cap = newCap;
        }

        NetworkInfo *nic = &list->items[list->count++];
        memset(nic, 0, sizeof *nic);
        memcpy(nic->name, e->d_name, nameLen + 1);
    }
    closedir(d);

    /* readdir order is arbitrary, so sort for a stable display order */
    if (list->count > 1) qsort(list->items, list->count, sizeof *list->items, compare_interfaces);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int net_init(NetworkList *list, int includeVirtual) {
    memset(list, 0, sizeof *list);

    if (discover_interfaces(list, includeVirtual) != 0) return 1;

    /* Baseline sample; speeds stay 0 until the next net_update() */
    if (net_update(list) != 0) {
        net_free(list);
        return 1;
    }
    return 0;
}

int net_update(NetworkList *list) {
    char *data = read_info_file("/proc/net/dev", NULL);
    if (!data) return -1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = list->hasSample ? seconds_between(&list->lastSample, &now) : 0.0;

    /* Roll the previous sample over */
    for (size_t i = 0; i < list->count; i++) {
        list->items[i].prevRxBytes = list->items[i].rxBytes;
        list->items[i].prevTxBytes = list->items[i].txBytes;
    }

    /* Each row looks like "  eth0: <rx bytes> <7 more rx columns> <tx bytes> ...".
       The two header lines contain no ':' so they are skipped. There may be no
       space after the colon, so split on it rather than on whitespace. */
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';

        const char *name = trim(line);
        unsigned long long rx, tx;
        if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx) != 2)
            continue;

        for (size_t i = 0; i < list->count; i++) {
            if (strcmp(list->items[i].name, name) == 0) {
                list->items[i].rxBytes = rx;
                list->items[i].txBytes = tx;
                break;
            }
        }
    }
    free(data);

    for (size_t i = 0; i < list->count; i++) {
        NetworkInfo *nic = &list->items[i];

        unsigned long long dRx = (nic->rxBytes >= nic->prevRxBytes) ? nic->rxBytes - nic->prevRxBytes : 0;
        unsigned long long dTx = (nic->txBytes >= nic->prevTxBytes) ? nic->txBytes - nic->prevTxBytes : 0;

        nic->downPerSecond = size_from_bytes((dt > 0) ? (double)dRx / dt : 0.0);
        nic->upPerSecond   = size_from_bytes((dt > 0) ? (double)dTx / dt : 0.0);
    }

    list->lastSample = now;
    list->hasSample = 1;
    return 0;
}

void net_free(NetworkList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
}