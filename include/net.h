#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <time.h>

#include "utils.h"

typedef struct {
    char name[16];               /* interface name, e.g. "eth0", "wlp3s0" (max 15 chars + null) */
    SizeInfo downPerSecond;
    SizeInfo upPerSecond;

    /* Internal bookkeeping */
    unsigned long long rxBytes, txBytes;                 /* latest sample */
    unsigned long long prevRxBytes, prevTxBytes;         /* previous sample */
} NetworkInfo;

typedef struct {
    NetworkInfo *items;
    size_t count;

    /* Internal: when the last sample was taken, for the per-second rates */
    struct timespec lastSample;
    int hasSample;
} NetworkList;

/* Finds the physical network interfaces and takes a baseline sample.
   Returns 0 on success, non-zero on failure. Pair with net_free(). */
int  net_init(NetworkList *list);

/* Takes a new sample. Speeds are calculated between this call and the
   previous one, so call it on an interval. Returns 0 on success, -1 on failure. */
int  net_update(NetworkList *list);

void net_print(const NetworkList *list);
void net_free(NetworkList *list);

#endif /* NET_H */