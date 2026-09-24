/*
09/20/2026

Matthew Wills

Linux System Monitor
*/

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "disk.h"
#include "gpu.h"
#include "mem.h"
#include "net.h"
#include "process.h"
#include "term.h"
#include "ui.h"
#include "version.h"

#define DEFAULT_INTERVAL_MS 1000
#define MIN_INTERVAL_MS     100        /* below this, the kernel's 10 ms counters make values choppy */
#define MAX_INTERVAL_MS     3600000
#define FIRST_SAMPLE_MS     300        /* the first update happens quickly so the screen fills fast */

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;

static void on_quit(int sig)   { (void)sig; running = 0; }
static void on_resize(int sig) { (void)sig; resized = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ */
/* Command line                                                       */
/* ------------------------------------------------------------------ */

static void usage(FILE *out, const char *prog) {
    char themes[128] = "";
    for (int i = 0; i < THEME_COUNT; i++) {
        strcat(themes, i ? ", " : "");
        strcat(themes, THEMES[i].name);
    }
    fprintf(out,
        "Usage: %s [options]\n"
        "\n"
        "A live Linux system monitor: CPU, GPU, memory, drives, network and processes.\n"
        "\n"
        "Options:\n"
        "  -i, --interval <ms>  refresh interval in milliseconds (%d-%d, default %d)\n"
        "      --compact        draw with no logo or graphs in a smaller footprint\n"
        "      --no-color       run without colours\n"
        "      --all-net        include non-physical network interfaces\n"
        "      --all-disk       include all non-physical block devices\n"
        "      --theme <name>   colour theme: %s (default: %s)\n"
        "      --gpu-info       also show GPUs with no readable data (as n/a) instead of\n"
        "                       leaving them out of the display\n"
        "      --gpu-debug      print a report of how GPUs were detected, including any\n"
        "                       with no readable data, then exit without launching\n"
        "  -h, --help           show this help\n"
        "  -V, --version        show the version\n"
        "\n"
        "Keys:\n"
        "  c                    select which cores are graphed\n"
        "  < >  [ ]  { }  ( )   page through cores, drives, GPUs, networks (only\n"
        "                       needed when a list is too long for its box)\n"
        "  left/right           choose the process sort column\n"
        "  up/down              sort ascending / descending\n"
        "  pgup/pgdn            scroll the process list (shift: a page at a time)\n"
        "  enter                show/hide the full name of a truncated process\n"
        "  q                    quit\n"
        "\n"
        "Colours: 256-colour palette by default, an 8-colour fallback on smaller\n"
        "terminals. Disabled by --no-color, NO_COLOR, TERM=dumb, or if `tput colors`\n"
        "fails or reports fewer than 8 colours.\n",
        prog, MIN_INTERVAL_MS, MAX_INTERVAL_MS, DEFAULT_INTERVAL_MS, themes, THEMES[0].name);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    long intervalMs = DEFAULT_INTERVAL_MS;
    int flagNoColor = 0, flagAllNet = 0, flagAllDisk = 0, flagCompact = 0, flagGpuInfo = 0, flagGpuDebug = 0;
    const Theme *theme = &THEMES[0];

    static const struct option longOpts[] = {
        { "interval", required_argument, NULL, 'i' },
        { "help",     no_argument,       NULL, 'h' },
        { "version",  no_argument,       NULL, 'V' },
        { "no-color", no_argument,       NULL, 1 },
        { "all-net",  no_argument,       NULL, 2 },
        { "all-disk", no_argument,       NULL, 3 },
        { "compact",  no_argument,       NULL, 4 },
        { "gpu-info",  no_argument,      NULL, 5 },
        { "gpu-debug", no_argument,      NULL, 7 },
        { "theme",    required_argument, NULL, 6 },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:hV", longOpts, NULL)) != -1) {
        switch (opt) {
        case 'i': {
            char *end;
            intervalMs = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' ||
                intervalMs < MIN_INTERVAL_MS || intervalMs > MAX_INTERVAL_MS) {
                fprintf(stderr, "%s: interval must be a whole number between %d and %d ms\n",
                        argv[0], MIN_INTERVAL_MS, MAX_INTERVAL_MS);
                return 2;
            }
            break;
        }
        case 'h': usage(stdout, argv[0]); return 0;
        case 'V': printf("%s %s\n", SYSMON_NAME, SYSMON_VERSION); return 0;
        case 1:   flagNoColor = 1; break;
        case 2:   flagAllNet = 1; break;
        case 3:   flagAllDisk = 1; break;
        case 4:   flagCompact = 1; break;
        case 5:   flagGpuInfo = 1; break;
        case 7:   flagGpuDebug = 1; break;
        case 6:
            theme = theme_find(optarg);
            if (!theme) {
                fprintf(stderr, "%s: unknown theme '%s' (available:", argv[0], optarg);
                for (int i = 0; i < THEME_COUNT; i++) fprintf(stderr, " %s", THEMES[i].name);
                fprintf(stderr, ")\n");
                return 2;
            }
            break;
        default:  usage(stderr, argv[0]); return 2;
        }
    }
    if (optind < argc) {
        usage(stderr, argv[0]);
        return 2;
    }

    if (flagGpuDebug) {                           /* diagnostics only: no terminal needed */
        GpuPaths paths = { NULL, NULL, NULL, stdout, 1 };
        GpuList probe;
        gpu_init_with(&probe, &paths);
        gpu_free(&probe);
        return 0;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "%s: needs an interactive terminal\n", argv[0]);
        return 1;
    }

    setlocale(LC_ALL, "");
    term_set_theme(theme);
    int sgrOk;
    ColorMode color = term_detect_color(flagNoColor, &sgrOk);   /* runs `tput`, so before raw mode */
    int unicode = term_is_utf8();

    CpuInfo cpu;
    MemoryInfo mem = {0};
    DiskList disks;
    NetworkList nets;
    GpuList gpus;
    ProcessList procs;
    Screen screen;
    Ui *ui = NULL;
    int status = 1;
    const char *error = NULL;

    if (cpu_init(&cpu) != 0) {
        fprintf(stderr, "Failed to initialise CPU stats\n");
        return 1;
    }
    if (disk_init(&disks, flagAllDisk) != 0) {
        fprintf(stderr, "Failed to initialise disk stats\n");
        goto free_cpu;
    }
    if (net_init(&nets, flagAllNet) != 0) {
        fprintf(stderr, "Failed to initialise network stats\n");
        goto free_disk;
    }
    gpu_init(&gpus, flagGpuInfo);                  /* never fails: having no GPU is normal */
    if (process_init(&procs) != 0) {
        fprintf(stderr, "Failed to initialise process stats\n");
        goto free_gpu;
    }
    if (mem_update(&mem) != 0) {
        fprintf(stderr, "Failed to read memory stats\n");
        goto free_procs;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);                     /* no SA_RESTART: signals interrupt poll() */
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_quit;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_resize;
    sigaction(SIGWINCH, &sa, NULL);

    if (term_enter() != 0) {
        fprintf(stderr, "Failed to set up the terminal\n");
        goto free_procs;
    }

    int cols, rows;
    term_get_size(&cols, &rows);
    scr_init(&screen, cols > 0 ? cols : 80, rows > 0 ? rows : 24, color, unicode, sgrOk);
    ui = ui_create(&screen, &cpu, &mem, &disks, &nets, &gpus, &procs, intervalMs, flagCompact);
    if (!ui) {
        term_leave();
        fprintf(stderr, "Out of memory\n");
        goto free_screen;
    }
    ui_set_size(ui, cols, rows);

    /* Every module keeps its own counters and turns them into per-second rates
       using the real time between updates, so the numbers stay accurate at any
       interval. Key presses redraw straight away without touching the data. */
    double nextData = now_ms() + (intervalMs < FIRST_SAMPLE_MS ? intervalMs : FIRST_SAMPLE_MS);
    int haveData = 0, needDraw = 0;

    while (running) {
        double now = now_ms();
        double wait = nextData - now;
        long uiWait = ui_timeout(ui, now);              /* flash / message expiry */
        double uiDeadline = (uiWait >= 0) ? now + (double)uiWait : 0;
        if (uiWait >= 0 && (double)uiWait < wait) wait = (double)uiWait;
        if (wait < 0) wait = 0;

        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        int rc = poll(&pfd, 1, (int)(wait + 0.999));
        if (!running) break;

        if (resized) {
            resized = 0;
            term_get_size(&cols, &rows);
            ui_set_size(ui, cols, rows);
            needDraw = 1;
        }

        if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            if (term_fill_input() < 0) break;                  /* stdin closed */
            Key k;
            while (term_parse_key(&k)) {
                int r = ui_key(ui, &k, now_ms());
                if (r == UI_QUIT) { running = 0; break; }
                if (r == UI_REDRAW) needDraw = 1;
            }
            if (!running) break;
        }

        now = now_ms();
        if (now >= nextData) {
            if (cpu_update(&cpu) != 0)         { error = "Failed to update CPU stats";     break; }
            if (mem_update(&mem) != 0)         { error = "Failed to read memory stats";    break; }
            if (disk_update(&disks) != 0)      { error = "Failed to update disk stats";    break; }
            if (net_update(&nets) != 0)        { error = "Failed to update network stats"; break; }
            gpu_update(&gpus);
            if (process_update(&procs) != 0)   { error = "Failed to update process list";  break; }
            ui_push_samples(ui, now_ms() / 1000.0);
            haveData = 1;
            needDraw = 1;

            nextData += (double)intervalMs;
            if (nextData < now_ms()) nextData = now_ms() + (double)intervalMs;   /* don't burst to catch up */
        }

        if (uiWait >= 0 && now_ms() >= uiDeadline) needDraw = 1;   /* a UI timer expired */

        if (needDraw && haveData) {
            ui_draw(ui, now_ms());
            scr_flush(&screen);
            needDraw = 0;
        }
    }

    term_leave();
    if (error) fprintf(stderr, "%s\n", error);
    status = error ? 1 : 0;

    ui_destroy(ui);
free_screen:
    scr_free(&screen);
free_procs:
    process_free(&procs);
free_gpu:
    gpu_free(&gpus);
    net_free(&nets);
free_disk:
    disk_free(&disks);
free_cpu:
    cpu_free(&cpu);
    return status;
}