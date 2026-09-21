/*
09/20/2026

Matthew Wills

Linux System Monitor
*/

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "mem.h"
#include "disk.h"
#include "net.h"
#include "process.h"

#define DEFAULT_INTERVAL_MS 1000
#define MIN_INTERVAL_MS     100        /* below this, the kernel's 10 ms counters make values choppy */
#define MAX_INTERVAL_MS     3600000
#define PROCESS_ROWS        10         /* rows shown in the process table; 0 shows every process */

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/* Command line                                                       */
/* ------------------------------------------------------------------ */

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "Usage: %s [-i interval_ms] [-n frames]\n"
        "  -i ms      refresh interval in milliseconds (%d-%d, default %d)\n"
        "  -n frames  exit after this many frames (default 0 = run until Ctrl+C)\n"
        "  -h         show this help\n",
        prog, MIN_INTERVAL_MS, MAX_INTERVAL_MS, DEFAULT_INTERVAL_MS);
}

/* Returns 0 to run, 1 if help was shown, -1 on a bad argument */
static int parse_args(int argc, char **argv, long *intervalMs, long *maxFrames) {
    int opt;
    char *end;

    while ((opt = getopt(argc, argv, "i:n:h")) != -1) {
        switch (opt) {
        case 'i':
            *intervalMs = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' ||
                *intervalMs < MIN_INTERVAL_MS || *intervalMs > MAX_INTERVAL_MS) {
                fprintf(stderr, "Interval must be a whole number between %d and %d ms\n",
                        MIN_INTERVAL_MS, MAX_INTERVAL_MS);
                return -1;
            }
            break;
        case 'n':
            *maxFrames = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || *maxFrames < 0) {
                fprintf(stderr, "Frame count must be a non-negative whole number\n");
                return -1;
            }
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 1;
        default:
            usage(stderr, argv[0]);
            return -1;
        }
    }

    if (optind < argc) {
        usage(stderr, argv[0]);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */

static void add_ms(struct timespec *t, long ms) {
    t->tv_sec  += ms / 1000;
    t->tv_nsec += (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

static int is_before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

/* Sleeps until an absolute deadline, so the time spent collecting data doesn't
   push the schedule back. Returns early if we are asked to quit. */
static void wait_until(const struct timespec *deadline) {
    while (running && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL) == EINTR)
        ;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_frame(int tty, long intervalMs, long frame,
                       const CpuInfo *cpu, const MemoryInfo *mem, const DiskList *disks,
                       const NetworkList *nets, const ProcessList *procs) {
    if (tty)
        printf("\033[H\033[2J");                       /* cursor home + clear screen */
    else if (frame > 1)
        printf("\n----------------------------------------\n\n");

    printf("Linux System Monitor   (refresh: %ld ms, Ctrl+C to quit)\n\n", intervalMs);
    cpu_print(cpu);
    printf("\n");
    mem_print(mem);
    printf("\n");
    disk_print(disks);
    printf("\n");
    net_print(nets);
    printf("\n");
    process_print(procs, PROCESS_ROWS);

    fflush(stdout);   /* one write per frame keeps redraws flicker-free */
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    long intervalMs = DEFAULT_INTERVAL_MS;
    long maxFrames = 0;

    int parsed = parse_args(argc, argv, &intervalMs, &maxFrames);
    if (parsed != 0) return (parsed > 0) ? 0 : 2;

    CpuInfo cpu;
    MemoryInfo mem;
    DiskList disks;
    NetworkList nets;
    ProcessList procs;
    int status = 1;

    if (cpu_init(&cpu) != 0) {
        fprintf(stderr, "Failed to initialise CPU stats\n");
        return 1;
    }
    if (disk_init(&disks) != 0) {
        fprintf(stderr, "Failed to initialise disk stats\n");
        goto free_cpu;
    }
    if (net_init(&nets) != 0) {
        fprintf(stderr, "Failed to initialise network stats\n");
        goto free_disk;
    }
    if (process_init(&procs) != 0) {
        fprintf(stderr, "Failed to initialise process stats\n");
        goto free_net;
    }

    /* Ctrl+C / kill: no SA_RESTART, so a pending sleep is interrupted */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int tty = isatty(STDOUT_FILENO);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (tty) {
        printf("\033[?1049h\033[?25l");                /* alternate screen, hide cursor */
        printf("Collecting first sample (%ld ms)...\n", intervalMs);
        fflush(stdout);
    }

    /* cpu_init/disk_init/... took the baseline samples. Each update below turns
       the counters accumulated since the previous sample into rates using the
       real elapsed time, so the numbers stay per-second at any interval. */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    const char *error = NULL;
    long frame = 0;

    while (running) {
        add_ms(&next, intervalMs);
        wait_until(&next);
        if (!running) break;

        if (cpu_update(&cpu) != 0)       { error = "Failed to update CPU stats";     break; }
        if (mem_update(&mem) != 0)       { error = "Failed to read memory stats";    break; }
        if (disk_update(&disks) != 0)    { error = "Failed to update disk stats";    break; }
        if (net_update(&nets) != 0)      { error = "Failed to update network stats"; break; }
        if (process_update(&procs) != 0) { error = "Failed to update process list";  break; }

        frame++;
        draw_frame(tty, intervalMs, frame, &cpu, &mem, &disks, &nets, &procs);

        if (maxFrames > 0 && frame >= maxFrames) break;

        /* If collecting took longer than the interval, don't try to catch up in a burst */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (is_before(&next, &now)) next = now;
    }

    if (tty) {
        printf("\033[?25h\033[?1049l");                /* show cursor, leave alternate screen */
        fflush(stdout);
    }
    if (error) fprintf(stderr, "%s\n", error);
    status = error ? 1 : 0;

    process_free(&procs);
free_net:
    net_free(&nets);
free_disk:
    disk_free(&disks);
free_cpu:
    cpu_free(&cpu);
    return status;
}