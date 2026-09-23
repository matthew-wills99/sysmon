#define _GNU_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "graph.h"
#include "ui.h"
#include "utils.h"
#include "version.h"

/* ------------------------------------------------------------------ */
/* Sizes                                                              */
/* ------------------------------------------------------------------ */

#define FULL_MIN_W        126
#define FULL_MIN_H        33
#define COMPACT_MIN_W     80
#define COMPACT_MIN_H     22
#define COMPACT_MAX_W     100          /* compact mode is meant to be small, so it stops growing here */
#define COMPACT_MAX_H     32
#define NET_SECTION_H     11           /* the network box never changes height */
#define CONTROLS_H        8
#define CONTROLS_NARROW_W 28
#define DEFAULT_CORES_SELECTED 8
#define FLASH_MS          350.0        /* how long a no-colour hover flash lasts */
#define MSG_MS            1600.0       /* how long "End of list" stays up */
#define FREEZE_MS         5000.0       /* the list holds its order this long after you last scrolled */
#define LOGO_FRAME_MS     100.0        /* logo animation: redraw rate while the logo is on screen */
#define LOGO_SPEED        4.0          /* gradient drift, in steps per second of a 12-stop loop */
#define PROC_SCROLL_STEP  1            /* rows moved per pgup/pgdn (shift+pgup/pgdn moves a page) */

enum { LAYOUT_FULL, LAYOUT_COMPACT, LAYOUT_TOO_SMALL };
enum { COL_PID, COL_NAME, COL_CPU, COL_MEM, COL_COUNT };

struct Ui {
    Screen *scr;
    CpuInfo *cpu;
    MemoryInfo *mem;
    DiskList *disks;
    NetworkList *nets;
    GpuList *gpus;
    ProcessList *procs;

    int forceCompact;
    int sizeUnknown;

    History cpuHist, memHist, netHist, gpuHist;
    float *scratch, *gpuScratch;
    int scratchN;

    /* core selection: coreSel drives the graph, coreEdit is what is being edited */
    int coreN;
    unsigned char *coreSel, *coreEdit;
    int selMode, selCursor, corePage;
    int coreRows, corePerPage, corePages;        /* from the last draw */

    int drivePage, drivePages, drivePerPage;
    int gpuPage, gpuPages, gpuPerPage;
    int netPage, netPages, netPerPage;

    int sortCol, sortDesc;
    int procTop, procRows, procNameW;
    int hoverPid, hoverIdx, hoverVisible, expandedPid;
    double flashUntil, msgUntil, nowMs;
    double animNext;                             /* when the logo animation wants its next frame; 0 = not on screen */
    const char *msg;

    ProcessInfo **view;
    size_t viewCap, viewCount;

    /* While you are scrolling, rows keep the order they had (numbers still update),
       otherwise a process hovering around 0.0%/0.1% would hop about the list. */
    double lastListInteraction;
    int frozenActive;
    int *frozenPid;
    size_t frozenN, frozenCap;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static Rect inset(Rect r, int n) {
    Rect o = { r.x + n, r.y + n, r.w - 2 * n, r.h - 2 * n };
    if (o.w < 0) o.w = 0;
    if (o.h < 0) o.h = 0;
    return o;
}

static uint8_t core_color(int i) { return (uint8_t)(C_CORE0 + (i % CORE_COLORS)); }

/* GPU0 takes the section's red; the others get distinct colours of their own */
static uint8_t gpu_color(int i) { return i == 0 ? C_GPU : (uint8_t)(C_CORE0 + (i * 3) % CORE_COLORS); }

static uint8_t temp_color(float c) { return c < 70.0f ? C_TEXT : (c < 85.0f ? C_WARN : C_BAD); }

static uint8_t usage_color(float pct) {
    return pct < 60.0f ? C_GOOD : (pct < 85.0f ? C_WARN : C_BAD);
}

/* 1.5M, 76.0, 512B ... at most 5 characters */
static void fmt_val(SizeInfo v, int withUnit, char *buf, size_t n) {
    char u[2] = { withUnit ? v.unit[0] : '\0', '\0' };
    if (v.unit[0] == 'B')         snprintf(buf, n, "%.0f%s", v.value, u);
    else if (v.value >= 99.95f)   snprintf(buf, n, "%.0f%s", v.value, u);
    else                          snprintf(buf, n, "%.1f%s", v.value, u);
}

static void fmt_size(SizeInfo v, char *buf, size_t n) { fmt_val(v, 1, buf, n); }

/* "76.0/700G" - the unit is only repeated when it differs */
static void fmt_pair(SizeInfo used, SizeInfo total, char *buf, size_t n) {
    char a[16], b[16];
    fmt_val(used, used.unit[0] != total.unit[0], a, sizeof a);
    fmt_val(total, 1, b, sizeof b);
    snprintf(buf, n, "%s/%s", a, b);
}

static void fmt_uptime(unsigned long long secs, char *buf, size_t n) {
    snprintf(buf, n, "%02llu:%02llu:%02llu:%02llu",
             secs / 86400, (secs % 86400) / 3600, (secs % 3600) / 60, secs % 60);
}

static double nice_ceil(double v) {
    if (v < 1024.0) v = 1024.0;
    double p = pow(10.0, floor(log10(v)));
    double m = v / p;
    double n = m <= 1 ? 1 : (m <= 2 ? 2 : (m <= 5 ? 5 : 10));
    return n * p;
}

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

static void draw_box(Ui *ui, Rect r, uint8_t color, const char *title, uint8_t attr) {
    Screen *s = ui->scr;
    if (r.w < 2 || r.h < 2) return;

    Style st = STYLE(color, attr);
    scr_puts(s, r.x, r.y, 1, G->tl, st);
    scr_puts(s, r.x + r.w - 1, r.y, 1, G->tr, st);
    scr_puts(s, r.x, r.y + r.h - 1, 1, G->bl, st);
    scr_puts(s, r.x + r.w - 1, r.y + r.h - 1, 1, G->br, st);
    for (int x = r.x + 1; x < r.x + r.w - 1; x++) {
        scr_puts(s, x, r.y, 1, G->h, st);
        scr_puts(s, x, r.y + r.h - 1, 1, G->h, st);
    }
    for (int y = r.y + 1; y < r.y + r.h - 1; y++) {
        scr_puts(s, r.x, y, 1, G->v, st);
        scr_puts(s, r.x + r.w - 1, y, 1, G->v, st);
    }

    if (title && *title && r.w > 8) {
        char t[64];
        snprintf(t, sizeof t, " %s ", title);
        scr_puts(s, r.x + 2, r.y, r.w - 4, t, STYLE(color, ATTR_BOLD));
    }
}

/* Text sitting on a box's top or bottom border, e.g. "─ 100% ──" */
static void border_label(Ui *ui, Rect r, int bottom, int right, const char *text, Style st) {
    char t[64];
    snprintf(t, sizeof t, " %s ", text);
    int w = text_width(t);
    if (w + 4 > r.w) return;
    int x = right ? r.x + r.w - 2 - w : r.x + 2;
    scr_puts(ui->scr, x, bottom ? r.y + r.h - 1 : r.y, w, t, st);
}

static void draw_bar(Ui *ui, int x, int y, int w, float frac, uint8_t fill) {
    Screen *s = ui->scr;
    if (w < 1) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    int full, rem = 0;
    if (s->unicode) {
        int eighths = (int)lroundf(frac * (float)w * 8.0f);
        full = eighths / 8;
        rem = eighths % 8;
    } else {
        full = (int)lroundf(frac * (float)w);
    }
    for (int i = 0; i < w; i++) {
        if (i < full)                 scr_puts(s, x + i, y, 1, G->bar_full, STYLE(fill, 0));
        else if (i == full && rem > 0) scr_puts(s, x + i, y, 1, G->bar_part[rem], STYLE(fill, 0));
        else                          scr_puts(s, x + i, y, 1, G->bar_empty, STYLE(C_DIM, 0));
    }
}

/* Draws "key: description" pairs separated by two spaces. Returns the width used. */
static int draw_hints(Ui *ui, int x, int y, int maxw, uint8_t keyColor,
                      const char *const *keys, const char *const *descs, int n) {
    Screen *s = ui->scr;
    int used = 0;
    for (int i = 0; i < n && used < maxw; i++) {
        if (i > 0) used += scr_puts(s, x + used, y, maxw - used, "  ", STYLE(C_DIM, 0));
        used += scr_puts(s, x + used, y, maxw - used, keys[i], STYLE(keyColor, ATTR_BOLD));
        used += scr_puts(s, x + used, y, maxw - used, ": ", STYLE(C_DIM, 0));
        used += scr_puts(s, x + used, y, maxw - used, descs[i], STYLE(C_TEXT, 0));
    }
    return used;
}

/* Nested box around a graph, with -60 / 0 seconds on the bottom border.
   Returns the plotting area inside it. */
static Rect draw_graph_frame(Ui *ui, Rect r, uint8_t color, const char *topLabel) {
    draw_box(ui, r, color, NULL, ATTR_DIM);
    if (topLabel) border_label(ui, r, 0, 0, topLabel, STYLE(C_DIM, 0));
    border_label(ui, r, 1, 0, "-60 seconds", STYLE(C_DIM, 0));
    border_label(ui, r, 1, 1, "0 seconds", STYLE(C_DIM, 0));
    return inset(r, 1);
}

/* ------------------------------------------------------------------ */
/* Sections                                                           */
/* ------------------------------------------------------------------ */

static void draw_cpu(Ui *ui, Rect r, int full) {
    Screen *s = ui->scr;
    CpuInfo *cpu = ui->cpu;

    draw_box(ui, r, C_CPU, "CPU", 0);
    Rect in = inset(r, 1);
    if (in.w < 24 || in.h < 5) return;

    int tw = in.w;
    Rect gr = { 0, 0, 0, 0 };
    if (full && in.w >= 52 + 24) {
        tw = 52;
        gr = (Rect){ in.x + tw, in.y, in.w - tw, in.h };
    }
    int tx = in.x + 1, tww = tw - 2;

    /* line 1: name ............ Uptime: DD:HH:MM:SS */
    char up[32], upline[48];
    fmt_uptime(cpu->uptimeSeconds, up, sizeof up);
    snprintf(upline, sizeof upline, "Uptime: %s", up);
    int upw = text_width(upline);
    scr_puts(s, tx + tww - upw, in.y, upw, "Uptime: ", STYLE(C_DIM, 0));
    scr_puts(s, tx + tww - upw + 8, in.y, upw - 8, up, STYLE(C_TEXT, 0));
    char nm[80];
    fit_text(cpu->name[0] ? cpu->name : "Unknown CPU", tww - upw - 2, G->ell, nm, sizeof nm);
    scr_puts(s, tx, in.y, tww - upw - 1, nm, STYLE(C_CPU, ATTR_BOLD));

    /* line 2: cores | threads ........ avg MHz */
    double mhz = 0;
    for (size_t i = 0; i < cpu->coreCount; i++) mhz += cpu->cores[i].clockSpeed;
    if (cpu->coreCount) mhz /= (double)cpu->coreCount;
    int cores = cpu->physicalCores > 0 ? cpu->physicalCores : (int)cpu->coreCount;
    scr_printf(s, tx, in.y + 1, tww, STYLE(C_TEXT, 0), "%d", cores);
    int w1 = (int)snprintf(NULL, 0, "%d", cores);
    scr_puts(s, tx + w1, in.y + 1, tww - w1, " cores | ", STYLE(C_DIM, 0));
    scr_printf(s, tx + w1 + 9, in.y + 1, tww, STYLE(C_TEXT, 0), "%d", cpu->threads);
    int w2 = (int)snprintf(NULL, 0, "%d", cpu->threads);
    scr_puts(s, tx + w1 + 9 + w2, in.y + 1, tww, " threads", STYLE(C_DIM, 0));
    char avg[32];
    snprintf(avg, sizeof avg, "avg %.0f MHz", mhz);
    scr_puts_right(s, tx + tww, in.y + 1, tww, avg, STYLE(C_TEXT, 0));

    /* line 3: total usage bar */
    scr_puts(s, tx, in.y + 2, 5, "Total", STYLE(C_CPU, ATTR_BOLD));
    int barW = tww - 6 - 7;
    draw_bar(ui, tx + 6, in.y + 2, barW, cpu->usagePercent / 100.0f, usage_color(cpu->usagePercent));
    scr_printf(s, tx + tww - 6, in.y + 2, 6, STYLE(usage_color(cpu->usagePercent), ATTR_BOLD),
               "%5.1f%%", cpu->usagePercent);

    /* per-core list, filled column by column */
    int hdr = full ? 1 : 0;
    int listTop = in.y + 3 + hdr;
    int rowsPerCol = imax(1, in.h - 3 - hdr - 1);
    int bw = full ? 6 : 0;
    int cw = full ? 24 : 17;
    int ncols = imax(1, (tww + 1) / (cw + 1));
    int perPage = ncols * rowsPerCol;
    int n = ui->coreN;
    int pages = imax(1, (n + perPage - 1) / perPage);

    ui->coreRows = rowsPerCol;
    ui->corePerPage = perPage;
    ui->corePages = pages;
    if (ui->selMode) ui->corePage = ui->selCursor / perPage;
    ui->corePage = clampi(ui->corePage, 0, pages - 1);

    int first = ui->corePage * perPage;
    int last = imin(n, first + perPage);

    if (hdr) {
        for (int c = 0; c < ncols; c++) {
            if (first + c * rowsPerCol >= n) break;
            char h[48];
            if (bw > 0) snprintf(h, sizeof h, " %-6s %-*s %4s %4s", "core", bw, "usage", "%", "MHz");
            else        snprintf(h, sizeof h, " %-6s %4s %4s", "core", "%", "MHz");
            scr_puts(s, tx + c * (cw + 1), in.y + 3, cw, h, STYLE(C_DIM, 0));
        }
    }

    for (int i = first; i < last; i++) {
        int pos = i - first;
        int x = tx + (pos / rowsPerCol) * (cw + 1);
        int y = listTop + (pos % rowsPerCol);
        const CoreInfo *core = &cpu->cores[i];

        Style nameSt;
        if (ui->selMode) {
            nameSt = STYLE(ui->coreEdit[i] ? C_GOOD : C_BAD, (i == ui->selCursor) ? ATTR_BOLD : 0);
            if (i == ui->selCursor) scr_puts(s, x, y, 1, G->cursor, STYLE(C_TEXT, ATTR_BOLD));
        } else {
            nameSt = ui->coreSel[i] ? STYLE(core_color(i), ATTR_BOLD) : STYLE(C_DIM, 0);
        }
        scr_printf(s, x + 1, y, 6, nameSt, "%-6s", core->name);

        int px = x + 8;
        if (bw > 0) {
            uint8_t bc = (ui->selMode || ui->coreSel[i]) ? core_color(i) : C_DIM;
            draw_bar(ui, x + 8, y, bw, core->usagePercent / 100.0f, bc);
            px = x + 8 + bw + 1;
        }
        scr_printf(s, px, y, 4, STYLE(C_TEXT, 0), "%3.0f%%", core->usagePercent);
        scr_printf(s, px + 5, y, 4, STYLE(C_DIM, 0), "%4.0f", core->clockSpeed);
    }

    /* bottom row: < > Cores 1/2 ............ total:32 */
    int py = in.y + in.h - 1;
    if (pages > 1) {
        char t[32];
        int u = scr_puts(s, tx, py, tww, "< >", STYLE(C_CPU, ATTR_BOLD));
        snprintf(t, sizeof t, " Cores %d/%d", ui->corePage + 1, pages);
        scr_puts(s, tx + u, py, tww - u, t, STYLE(C_DIM, 0));
    }
    char tot[32];
    snprintf(tot, sizeof tot, "total:%d", n);
    scr_puts_right(s, tx + tww, py, tww, tot, STYLE(C_DIM, 0));

    /* graph of the selected cores */
    if (gr.w > 0) {
        Rect ga = draw_graph_frame(ui, gr, C_CPU, "100%");
        GraphLine *lines = malloc((size_t)(n > 0 ? n : 1) * sizeof *lines);
        int nl = 0;
        for (int i = 0; lines && i < n; i++)
            if (ui->coreSel[i]) lines[nl++] = (GraphLine){ i, core_color(i) };
        if (nl == 0) {
            const char *msg = "no cores selected";
            scr_puts(s, ga.x + (ga.w - text_width(msg)) / 2, ga.y + ga.h / 2, ga.w, msg, STYLE(C_DIM, 0));
        } else {
            graph_draw(s, ga, &ui->cpuHist, lines, nl, 100.0);
        }
        free(lines);
    }
}

/* "Main 68% 10.9/16.0G" */
static void mem_text(Ui *ui, int x, int y, int w, const char *label, float pct,
                     SizeInfo used, SizeInfo total, uint8_t color, int hasSwap) {
    Screen *s = ui->scr;
    scr_puts(s, x, y, w, label, STYLE(color, ATTR_BOLD));
    if (!hasSwap) {
        scr_puts(s, x + 5, y, w - 5, "none", STYLE(C_DIM, 0));
        return;
    }
    scr_printf(s, x + 5, y, 4, STYLE(usage_color(pct), ATTR_BOLD), "%3.0f%%", pct);
    char pair[32];
    fmt_pair(used, total, pair, sizeof pair);
    scr_puts(s, x + 10, y, w - 10, pair, STYLE(C_TEXT, 0));
}

static void draw_mem(Ui *ui, Rect r, int full) {
    Screen *s = ui->scr;
    MemoryInfo *m = ui->mem;
    int hasSwap = m->swapTotal.value > 0;

    draw_box(ui, r, C_MEM, "Memory", 0);
    Rect in = inset(r, 1);
    if (in.w < 24 || in.h < 2) return;

    if (!full) {
        /* two lines: label, percent, bar, used/total */
        for (int k = 0; k < 2 && k < in.h; k++) {
            int swap = (k == 1);
            int x = in.x + 1, w = in.w - 2, y = in.y + k;
            uint8_t col = swap ? C_SWAP : C_MEM;
            float pct = swap ? m->swapUsagePercent : m->mainUsagePercent;

            scr_puts(s, x, y, w, swap ? "Swap" : "Main", STYLE(col, ATTR_BOLD));
            if (swap && !hasSwap) {
                scr_puts(s, x + 5, y, w - 5, "none", STYLE(C_DIM, 0));
                continue;
            }
            char pair[32];
            fmt_pair(swap ? m->swapUsed : m->used, swap ? m->swapTotal : m->total, pair, sizeof pair);
            int pw = text_width(pair);
            scr_printf(s, x + 5, y, 4, STYLE(usage_color(pct), ATTR_BOLD), "%3.0f%%", pct);
            scr_puts_right(s, x + w, y, pw, pair, STYLE(C_TEXT, 0));
            draw_bar(ui, x + 10, y, w - 10 - pw - 1, pct / 100.0f, col);
        }
        return;
    }

    int half = in.w / 2;
    mem_text(ui, in.x + 1, in.y, half - 2, "Main", m->mainUsagePercent, m->used, m->total, C_MEM, 1);
    mem_text(ui, in.x + half + 1, in.y, in.w - half - 2, "Swap", m->swapUsagePercent,
             m->swapUsed, m->swapTotal, C_SWAP, hasSwap);
    draw_bar(ui, in.x + 1, in.y + 1, half - 2, m->mainUsagePercent / 100.0f, C_MEM);
    if (hasSwap) draw_bar(ui, in.x + half + 1, in.y + 1, in.w - half - 2, m->swapUsagePercent / 100.0f, C_SWAP);

    Rect gr = { in.x, in.y + 2, in.w, in.h - 2 };
    if (gr.h < 3) return;
    Rect ga = draw_graph_frame(ui, gr, C_MEM, "100%");
    GraphLine lines[2] = { { 0, C_MEM }, { 1, C_SWAP } };
    graph_draw(s, ga, &ui->memHist, lines, hasSwap ? 2 : 1, 100.0);
}

/* How many rows of interfaces/drives fit, and whether the last row becomes pager controls */
static void list_layout(int count, int availRows, int perRow, int *rows, int *pager, int *perPage) {
    if (availRows < 1) availRows = 1;
    if (count <= availRows * perRow) {
        *pager = 0;
        *rows = imax(1, (count + perRow - 1) / perRow);
        *perPage = imax(1, count);
    } else {
        *pager = 1;
        *rows = imax(1, availRows - 1);
        *perPage = *rows * perRow;
    }
}

static void draw_pager_row(Ui *ui, int x, int y, int w, const char *keys, const char *noun,
                           int page, int pages, int total, uint8_t color) {
    Screen *s = ui->scr;
    char t[48];
    int u = scr_puts(s, x, y, w, keys, STYLE(color, ATTR_BOLD));
    snprintf(t, sizeof t, " %s %d/%d", noun, page + 1, pages);
    scr_puts(s, x + u, y, w - u, t, STYLE(C_DIM, 0));
    snprintf(t, sizeof t, "total:%d", total);
    scr_puts_right(s, x + w, y, w, t, STYLE(C_DIM, 0));
}

static void draw_net_cell(Ui *ui, int x, int y, int w, const NetworkInfo *n) {
    Screen *s = ui->scr;
    int nameW = imax(3, w - 14);
    char nm[48], v[16];

    fit_text(n->name, nameW, G->ell, nm, sizeof nm);
    scr_puts(s, x, y, nameW, nm, STYLE(C_NET, ATTR_BOLD));

    int vx = x + nameW + 1;
    scr_puts(s, vx, y, 1, G->adn, STYLE(C_NETDOWN, ATTR_BOLD));
    fmt_size(n->downPerSecond, v, sizeof v);
    scr_printf(s, vx + 1, y, 5, STYLE(n->downPerSecond.value > 0 ? C_TEXT : C_DIM, 0), "%5s", v);
    scr_puts(s, vx + 7, y, 1, G->aup, STYLE(C_NETUP, ATTR_BOLD));
    fmt_size(n->upPerSecond, v, sizeof v);
    scr_printf(s, vx + 8, y, 5, STYLE(n->upPerSecond.value > 0 ? C_TEXT : C_DIM, 0), "%5s", v);
}

/* availRows: how many rows may be used for interfaces (full mode: at most 3) */
static void draw_net(Ui *ui, Rect r, int full, int availRows) {
    Screen *s = ui->scr;
    NetworkList *nets = ui->nets;
    int k = (int)nets->count;

    draw_box(ui, r, C_NET, "Network", 0);
    Rect in = inset(r, 1);
    if (in.w < 30 || in.h < 1) return;

    int rows, pager, perPage;
    list_layout(k, imin(availRows, in.h), 2, &rows, &pager, &perPage);
    ui->netPerPage = perPage;
    ui->netPages = imax(1, (k + perPage - 1) / perPage);
    ui->netPage = clampi(ui->netPage, 0, ui->netPages - 1);

    int cellW = (in.w - 2) / 2;
    if (k == 0) {
        scr_puts(s, in.x + 1, in.y, in.w - 2, "no networks", STYLE(C_DIM, 0));
    } else {
        int first = ui->netPage * perPage;
        int last = imin(k, first + perPage);
        for (int i = first; i < last; i++) {
            int pos = i - first;
            /* two columns, filled row by row */
            draw_net_cell(ui, in.x + 1 + (pos % 2) * cellW, in.y + pos / 2, cellW - 2, &nets->items[i]);
        }
    }
    int used = rows;
    if (pager) {
        draw_pager_row(ui, in.x + 1, in.y + rows, in.w - 2, "( )", "Networks",
                       ui->netPage, ui->netPages, k, C_NET);
        used = rows + 1;
    }

    if (!full) return;

    /* the graph takes whatever the interface rows leave over */
    Rect gr = { in.x, in.y + used, in.w, in.h - used };
    if (gr.h < 3) return;

    GraphLine lines[2] = { { 0, C_NETDOWN }, { 1, C_NETUP } };
    double ymax = nice_ceil(hist_max(&ui->netHist, lines, 2));

    char top[32], v[16];
    fmt_size(size_from_bytes(ymax), v, sizeof v);
    snprintf(top, sizeof top, "%s/s", v);
    Rect ga = draw_graph_frame(ui, gr, C_NET, top);
    graph_draw(s, ga, &ui->netHist, lines, 2, ymax);
}

/* GPU0  <model name>  ~42%  4.0/16.0G  61°C */
static void draw_gpu_row(Ui *ui, int x, int y, int w, int idx) {
    Screen *s = ui->scr;
    const GpuInfo *g = &ui->gpus->items[idx];
    int single = ui->gpus->count == 1;
    Style dim = STYLE(C_DIM, 0);

    char label[16];
    snprintf(label, sizeof label, "GPU%d", idx);
    int labelW = (int)strlen(label) + 1;
    scr_puts(s, x, y, w, label, STYLE(gpu_color(idx), ATTR_BOLD));

    /* right-hand block, from the right edge: temperature | memory | usage */
    const int tempW = 5, memW = 10, useW = 5;
    int xr = x + w;
    int tempX = xr - tempW, memX = tempX - 1 - memW, useX = memX - 1 - useW;

    char b[48];
    if (g->suspended) {
        scr_puts_right(s, useX + useW, y, useW, "off", dim);
        scr_puts_right(s, memX + memW, y, memW, "-", dim);
        scr_puts_right(s, tempX + tempW, y, tempW, "-", dim);
    } else {
        if (g->hasUsage) {
            snprintf(b, sizeof b, "%s%.0f%%", g->usageEstimated ? "~" : "", g->usagePercent);
            scr_puts_right(s, useX + useW, y, useW, b, STYLE(usage_color(g->usagePercent), ATTR_BOLD));
        } else {
            scr_puts_right(s, useX + useW, y, useW, "n/a", dim);
        }
        if (g->hasMem) {
            fmt_pair(g->memUsed, g->memTotal, b, sizeof b);
            scr_puts_right(s, memX + memW, y, memW, b, single ? STYLE(C_SWAP, 0) : STYLE(C_TEXT, 0));
        } else {
            scr_puts_right(s, memX + memW, y, memW, "n/a", dim);
        }
        if (g->hasTemp) {
            snprintf(b, sizeof b, "%.0f%sC", g->tempC, ui->scr->unicode ? "°" : "");
            scr_puts_right(s, tempX + tempW, y, tempW, b, STYLE(temp_color(g->tempC), 0));
        } else {
            scr_puts_right(s, tempX + tempW, y, tempW, "n/a", dim);
        }
    }

    int nameW = useX - 1 - (x + labelW);
    if (nameW >= 6) {
        char nm[80];
        fit_text(g->name, nameW, G->ell, nm, sizeof nm);
        scr_puts(s, x + labelW, y, nameW, nm, g->suspended ? dim : STYLE(C_TEXT, 0));
    }
}

/* Same shape as the network box: a few GPU rows, a pager row if there are too many, then a graph */
static void draw_gpu(Ui *ui, Rect r, int full) {
    Screen *s = ui->scr;
    GpuList *gl = ui->gpus;
    int n = (int)gl->count;

    draw_box(ui, r, C_GPU, "GPU", 0);
    Rect in = inset(r, 1);
    if (in.w < 30 || in.h < 1) return;

    int rows, pager, perPage;
    list_layout(n, full ? imin(3, in.h) : in.h, 1, &rows, &pager, &perPage);
    ui->gpuPerPage = perPage;
    ui->gpuPages = imax(1, (n + perPage - 1) / perPage);
    ui->gpuPage = clampi(ui->gpuPage, 0, ui->gpuPages - 1);

    int first = ui->gpuPage * perPage;
    int last = imin(n, first + perPage);
    for (int i = first; i < last; i++) draw_gpu_row(ui, in.x + 1, in.y + (i - first), in.w - 2, i);

    int used = rows;
    if (pager) {
        draw_pager_row(ui, in.x + 1, in.y + rows, in.w - 2, "{ }", "GPUs",
                       ui->gpuPage, ui->gpuPages, n, C_GPU);
        used = rows + 1;
    }
    if (!full) return;

    Rect gr = { in.x, in.y + used, in.w, in.h - used };
    if (gr.h < 3) return;
    Rect ga = draw_graph_frame(ui, gr, C_GPU, "100%");

    int anyData = 0;                                 /* e.g. a WSL2 GPU with no counters has nothing to plot */
    for (int i = 0; i < n; i++)
        if (gl->items[i].hasUsage || gl->items[i].hasMem || gl->items[i].suspended) anyData = 1;
    if (!anyData) {
        const char *msg = "no statistics (run with --gpu-info)";
        scr_puts(s, ga.x + (ga.w - text_width(msg)) / 2, ga.y + ga.h / 2, ga.w, msg, STYLE(C_DIM, 0));
        return;
    }

    /* history layout: series 0..n-1 are usage, n..2n-1 are VRAM. One GPU shows both;
       several show just their usage lines, one colour per GPU. */
    GraphLine *lines = malloc((size_t)(2 * n) * sizeof *lines);
    int nl = 0;
    for (int i = 0; lines && i < n; i++) {
        lines[nl++] = (GraphLine){ i, gpu_color(i) };
        if (n == 1 && gl->items[i].hasMem) lines[nl++] = (GraphLine){ n + i, C_SWAP };
    }
    if (lines) graph_draw(s, ga, &ui->gpuHist, lines, nl, 100.0);
    free(lines);
}

static void draw_drives(Ui *ui, Rect r) {
    Screen *s = ui->scr;
    DiskList *disks = ui->disks;
    int n = (int)disks->count;

    draw_box(ui, r, C_DISK, "Drives", 0);
    Rect in = inset(r, 1);
    if (in.w < 36 || in.h < 2) return;

    int rows, pager, perPage;
    list_layout(n, in.h - 1, 1, &rows, &pager, &perPage);
    ui->drivePerPage = perPage;
    ui->drivePages = imax(1, (n + perPage - 1) / perPage);
    ui->drivePage = clampi(ui->drivePage, 0, ui->drivePages - 1);

    /* columns: name | use% | used/size | R/s | W/s | type */
    int x0 = in.x + 1, w = in.w - 2;
    int nameW = imax(3, w - 33);
    int cUse = x0 + nameW + 1;
    int cPair = cUse + 5;
    int cR = cPair + 11;
    int cW = cR + 6;
    int cType = cW + 6;
    Style dim = STYLE(C_DIM, 0);

    scr_puts(s, x0, in.y, nameW, "Name", dim);
    scr_puts_right(s, cUse + 4, in.y, 4, "Use", dim);
    scr_puts_right(s, cPair + 10, in.y, 10, "Used/Size", dim);
    scr_puts_right(s, cR + 5, in.y, 5, "R/s", dim);
    scr_puts_right(s, cW + 5, in.y, 5, "W/s", dim);
    scr_puts(s, cType, in.y, 4, "Type", dim);

    if (n == 0) {
        scr_puts(s, x0, in.y + 1, w, "no drives detected", dim);
        return;
    }

    int first = ui->drivePage * perPage;
    int last = imin(n, first + perPage);
    for (int i = first; i < last; i++) {
        const DiskInfo *d = &disks->items[i];
        int y = in.y + 1 + (i - first);
        char nm[48], pair[32], rv[16], wv[16];

        fit_text(d->name, nameW, G->ell, nm, sizeof nm);
        scr_puts(s, x0, y, nameW, nm, STYLE(C_TEXT, ATTR_BOLD));
        scr_printf(s, cUse, y, 4, STYLE(usage_color(d->usagePercent), 0), "%3.0f%%", d->usagePercent);
        fmt_pair(d->used, d->size, pair, sizeof pair);
        scr_printf(s, cPair, y, 10, STYLE(C_TEXT, 0), "%10s", pair);
        fmt_size(d->readPerSecond, rv, sizeof rv);
        fmt_size(d->writePerSecond, wv, sizeof wv);
        scr_printf(s, cR, y, 5, STYLE(d->readPerSecond.value > 0 ? C_TEXT : C_DIM, 0), "%5s", rv);
        scr_printf(s, cW, y, 5, STYLE(d->writePerSecond.value > 0 ? C_TEXT : C_DIM, 0), "%5s", wv);

        const char *type = d->type == SSD ? "SSD" : d->type == HDD ? "HDD" : d->type == VRT ? "virt" : "?";
        uint8_t tc = d->type == SSD ? C_DISK : d->type == HDD ? C_WARN : C_DIM;
        scr_puts(s, cType, y, 4, type, STYLE(tc, 0));
    }
    if (pager)
        draw_pager_row(ui, x0, in.y + 1 + rows, w, "[ ]", "Drives", ui->drivePage, ui->drivePages, n, C_DISK);
}

/* --- logo --------------------------------------------------------- */

/* "SYSMON" in a two-row block font */
static const char *const LOGO_TOP[6] = { "█▀",  "█▄█", "█▀",  "█▀▄▀█", "█▀█", "█▄ █" };
static const char *const LOGO_BOT[6] = { "▄█",  " █ ", "▄█",  "█ ▀ █", "█▄█", "█ ▀█" };

/* Only a 256-colour terminal can show a gradient, so that is the only place it animates */
static int logo_animated(const Ui *ui) { return ui->scr->color == COLOR_256 && ui->scr->unicode; }

/* Colour of the logo cell at (col, row): the gradient as a slightly diagonal wave drifting to the right */
static uint8_t logo_color(const Ui *ui, int col, int row) {
    /* the wave is defined for a 12-stop loop; scale it so more stops just mean finer steps */
    double v = (0.5 * col + 0.8 * row - ui->nowMs / 1000.0 * LOGO_SPEED) * (LOGO_STOPS / 12.0);
    int idx = (int)floor(v) % LOGO_STOPS;
    if (idx < 0) idx += LOGO_STOPS;
    return (uint8_t)(C_LOGO0 + idx);
}

/* One row of letters from..to, joined by single spaces, coloured cell by cell */
static void logo_row(Ui *ui, int x, int y, const char *const glyphs[6], int from, int to, int row) {
    int animate = logo_animated(ui);
    int col = 0;
    for (int i = from; i < to; i++) {
        if (i > from) col++;                                   /* gap between letters */
        for (const char *p = glyphs[i]; *p; col++) {
            unsigned char lead = (unsigned char)*p;
            int n = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (*p != ' ') {
                char ch[5];
                memcpy(ch, p, (size_t)n);
                ch[n] = '\0';
                uint8_t c = animate ? logo_color(ui, col, row) : (uint8_t)(C_LOGO0 + i);
                scr_puts(ui->scr, x + col, y, 1, ch, STYLE(c, ATTR_BOLD));
            }
            p += n;
        }
    }
}

static void logo_letters(Ui *ui, int x, int y, int from, int to, int rowBase) {
    logo_row(ui, x, y,     LOGO_TOP, from, to, rowBase);
    logo_row(ui, x, y + 1, LOGO_BOT, from, to, rowBase + 1);
    if (logo_animated(ui)) ui->animNext = ui->nowMs + LOGO_FRAME_MS;     /* keep the animation going */
}

static void draw_logo(Ui *ui, Rect r) {
    Screen *s = ui->scr;
    draw_box(ui, r, C_LOGO, NULL, 0);
    Rect in = inset(r, 1);
    if (in.w < 8 || in.h < 1) return;

    const char *tag = "linux system monitor";
    if (!s->unicode) {
        const char *t = "SYSMON";
        scr_puts(s, in.x + (in.w - 6) / 2, in.y + in.h / 2, 6, t, STYLE(C_LOGO, ATTR_BOLD));
        return;
    }
    if (in.w >= 26 && in.h >= 2) {
        int withTag = in.h >= 4 && in.w >= text_width(tag) + 2;
        int artH = withTag ? 4 : 2;
        int y = in.y + (in.h - artH) / 2;
        int wdt = 24;
        logo_letters(ui, in.x + (in.w - wdt) / 2, y, 0, 6, 0);
        if (withTag) scr_puts(s, in.x + (in.w - text_width(tag)) / 2, y + 3, in.w, tag, STYLE(C_DIM, 0));
    } else if (in.h >= 4) {
        /* narrow: stacked "SYS" over "MON" */
        int y = in.y + (in.h - 4) / 2;
        int w1 = 0;
        for (int i = 0; i < 3; i++) w1 += text_width(LOGO_TOP[i]) + 1;
        w1--;
        logo_letters(ui, in.x + (in.w - w1) / 2, y, 0, 3, 0);
        int w2 = 0;
        for (int i = 3; i < 6; i++) w2 += text_width(LOGO_TOP[i]) + 1;
        w2--;
        logo_letters(ui, in.x + (in.w - w2) / 2, y + 2, 3, 6, 2);
    } else {
        scr_puts(s, in.x + (in.w - 6) / 2, in.y + in.h / 2, 6, "SYSMON", STYLE(C_LOGO, ATTR_BOLD));
    }
}

/* The keys worth showing right now. Paging keys only appear while their list is actually
   split into pages, and "c" / "q" come first, so a short list stays short. Returns the count (max 5). */
static int build_hints(const Ui *ui, int compact, const char *arrows,
                       const char *keys[5], const char *descs[5]) {
    int n = 0;
#define ADD_HINT(k, d) do { keys[n] = (k); descs[n] = (d); n++; } while (0)
    if (ui->selMode) {
        ADD_HINT(arrows, "move");
        ADD_HINT("space/enter", "toggle");
        ADD_HINT("a", compact ? "all/none" : "select/clear all");
        if (ui->corePages > 1) ADD_HINT("< >", compact ? "page" : "change page");
        ADD_HINT("esc", "done");
    } else {
        ADD_HINT("c", "select cores");
        ADD_HINT("q", "quit");
        if (ui->corePages > 1)  ADD_HINT("< >", compact ? "cores" : "cores page");
        if (ui->drivePages > 1) ADD_HINT("[ ]", compact ? "drives" : "drives page");
        if (ui->netPages > 1)   ADD_HINT("( )", compact ? "networks" : "networks page");
    }
#undef ADD_HINT
    return n;
}

static void draw_controls(Ui *ui, Rect r) {
    Screen *s = ui->scr;
    draw_box(ui, r, C_LOGO, ui->selMode ? "Select cores" : "Controls", 0);
    Rect in = inset(r, 1);
    if (in.w < 16 || in.h < 2) return;

    char arrows[24];
    snprintf(arrows, sizeof arrows, "%s%s", G->ud, G->lr);

    const char *keys[5], *descs[5];
    int count = build_hints(ui, 0, arrows, keys, descs);
    int lines = imin(count, in.h - 1);
    for (int i = 0; i < lines; i++)
        draw_hints(ui, in.x + 1, in.y + i, in.w - 2, C_LOGO, &keys[i], &descs[i], 1);

    char foot[64];
    snprintf(foot, sizeof foot, "v%s | by %s", SYSMON_VERSION, SYSMON_AUTHOR);
    scr_puts(s, in.x + 1, in.y + in.h - 1, in.w - 2, foot, STYLE(C_DIM, 0));
}

/* --- processes ---------------------------------------------------- */

static int g_sortCol, g_sortDesc;

static int cmp_view(const void *a, const void *b) {
    const ProcessInfo *pa = *(ProcessInfo *const *)a;
    const ProcessInfo *pb = *(ProcessInfo *const *)b;
    int r = 0;
    switch (g_sortCol) {
        case COL_PID:  r = (pa->pid > pb->pid) - (pa->pid < pb->pid); break;
        case COL_NAME: r = strcasecmp(pa->name, pb->name); break;
        case COL_CPU:  r = (pa->CPUPercent > pb->CPUPercent) - (pa->CPUPercent < pb->CPUPercent); break;
        case COL_MEM:  r = (pa->memPercent > pb->memPercent) - (pa->memPercent < pb->memPercent); break;
    }
    if (g_sortDesc) r = -r;
    if (r == 0) r = (pa->pid > pb->pid) - (pa->pid < pb->pid);     /* stable order for ties */
    return r;
}

static int cmp_proc_pid(const void *a, const void *b) {
    int x = ((const ProcessInfo *)a)->pid, y = ((const ProcessInfo *)b)->pid;
    return (x > y) - (x < y);
}

static void remember_order(Ui *ui) {
    size_t n = ui->viewCount;
    if (n > ui->frozenCap) {
        int *f = realloc(ui->frozenPid, n * sizeof *f);
        if (!f) { ui->frozenActive = 0; return; }
        ui->frozenPid = f;
        ui->frozenCap = n;
    }
    for (size_t i = 0; i < n; i++) ui->frozenPid[i] = ui->view[i]->pid;
    ui->frozenN = n;
}

/* Keeps the rows in the order they were in when the user started scrolling.
   Exited processes drop out and new ones are added at the end. */
static void apply_freeze(Ui *ui) {
    int hold = ui->hoverPid && ui->hoverVisible &&
               (ui->nowMs - ui->lastListInteraction) < FREEZE_MS;
    if (!hold) { ui->frozenActive = 0; return; }

    if (!ui->frozenActive) {                       /* first build after scrolling began */
        ui->frozenActive = 1;
        remember_order(ui);
        return;
    }

    size_t n = ui->viewCount;
    ProcessInfo *items = ui->procs->items;         /* sorted by pid */
    ProcessInfo **out = malloc((n ? n : 1) * sizeof *out);
    unsigned char *seen = calloc(n ? n : 1, 1);
    if (!out || !seen) { free(out); free(seen); return; }

    size_t m = 0;
    for (size_t i = 0; i < ui->frozenN; i++) {
        ProcessInfo key;
        key.pid = ui->frozenPid[i];
        ProcessInfo *p = bsearch(&key, items, n, sizeof *items, cmp_proc_pid);
        if (p && !seen[p - items]) { seen[p - items] = 1; out[m++] = p; }
    }
    for (size_t i = 0; i < n; i++)                 /* newcomers, in the normal sort order */
        if (!seen[ui->view[i] - items]) out[m++] = ui->view[i];

    memcpy(ui->view, out, m * sizeof *out);
    free(out);
    free(seen);
    remember_order(ui);
}

/* The process list is replaced on every refresh, so the sorted view of it is
   rebuilt whenever it is needed and never kept across calls. */
static void build_view(Ui *ui) {
    size_t n = ui->procs->count;
    if (n > ui->viewCap) {
        ProcessInfo **v = realloc(ui->view, n * sizeof *v);
        if (!v) { ui->viewCount = 0; return; }
        ui->view = v;
        ui->viewCap = n;
    }
    for (size_t i = 0; i < n; i++) ui->view[i] = &ui->procs->items[i];
    g_sortCol = ui->sortCol;
    g_sortDesc = ui->sortDesc;
    qsort(ui->view, n, sizeof *ui->view, cmp_view);
    ui->viewCount = n;
    apply_freeze(ui);

    /* follow the hovered process to its new position, or stay on the same row if it exited */
    if (ui->hoverPid) {
        int found = 0;
        for (size_t i = 0; i < n; i++)
            if (ui->view[i]->pid == ui->hoverPid) { ui->hoverIdx = (int)i; found = 1; break; }
        if (!found) {
            if (n == 0) ui->hoverPid = 0;
            else {
                ui->hoverIdx = clampi(ui->hoverIdx, 0, (int)n - 1);
                ui->hoverPid = ui->view[ui->hoverIdx]->pid;
            }
        }
    }
    if (ui->expandedPid) {
        int found = 0;
        for (size_t i = 0; i < n; i++)
            if (ui->view[i]->pid == ui->expandedPid) { found = 1; break; }
        if (!found) ui->expandedPid = 0;
    }
}

static void keep_hover_in_view(Ui *ui) {
    int rows = imax(1, ui->procRows);
    int n = (int)ui->viewCount;
    if (ui->hoverPid) {
        if (ui->hoverIdx < ui->procTop) ui->procTop = ui->hoverIdx;
        if (ui->hoverIdx >= ui->procTop + rows) ui->procTop = ui->hoverIdx - rows + 1;
    }
    ui->procTop = clampi(ui->procTop, 0, imax(0, n - rows));
}

/* Row highlight: a background colour when colours are on, reverse video otherwise */
static Style hover_style(const Ui *ui, uint8_t fg) {
    switch (ui->scr->color) {
        case COLOR_256: return (Style){ fg, C_HOVER, ATTR_BOLD };
        case COLOR_8:   return (Style){ fg, C_HOVER, 0 };
        default:        return (Style){ fg, C_DEF, ATTR_REV };
    }
}

/* "some-very-long-na(...)" */
static void trunc_name(const char *name, int w, char *out, size_t n) {
    if (text_width(name) <= w) {
        snprintf(out, n, "%s", name);
        return;
    }
    if (w <= 5) {
        fit_text(name, w, "", out, n);
        return;
    }
    fit_text(name, w - 5, "", out, n);
    strncat(out, "(...)", n - strlen(out) - 1);
}

static uint8_t load_color(float pct) {
    return pct >= 50.0f ? C_BAD : (pct >= 15.0f ? C_WARN : (pct > 0.05f ? C_TEXT : C_DIM));
}

static void draw_procs(Ui *ui, Rect r) {
    Screen *s = ui->scr;
    draw_box(ui, r, C_PROC, "Processes", 0);
    Rect in = inset(r, 1);
    ui->procRows = 10;
    ui->procNameW = 12;
    if (in.h < 5 || in.w < 26) return;

    int rows = in.h - 3;                       /* header + rows + two footer lines */
    ui->procRows = rows;
    build_view(ui);
    keep_hover_in_view(ui);
    int n = (int)ui->viewCount;

    int cx = in.x + 1, cw = in.w - 2;
    int pidW = 6, cpuW = 5, memW = 5;
    int nameW = cw - (pidW + 1) - (cpuW + 1) - (memW + 1);
    ui->procNameW = nameW;
    int pidX = cx, nameX = cx + pidW + 1, cpuX = nameX + nameW + 1, memX = cpuX + cpuW + 1;

    /* header: the sorted column is highlighted and carries the direction arrow */
    static const char *labels[COL_COUNT] = { "PID", "Process", "CPU%", "MEM%" };
    int hx[COL_COUNT] = { pidX, nameX, cpuX, memX };
    int hw[COL_COUNT] = { pidW, nameW, cpuW, memW };
    for (int c = 0; c < COL_COUNT; c++) {
        char h[32];
        int sorted = (c == ui->sortCol);
        snprintf(h, sizeof h, "%s%s", labels[c], sorted ? (ui->sortDesc ? G->desc : G->asc) : "");
        Style st = STYLE(C_PROC, sorted ? (ATTR_BOLD | ATTR_REV) : ATTR_BOLD);
        int w = text_width(h);
        int x = (c == COL_NAME) ? hx[c] : hx[c] + hw[c] - w;
        scr_puts(s, x, in.y, hw[c], h, st);
    }

    if (n == 0) {
        const char *m = "no processes";
        scr_puts(s, in.x + (in.w - text_width(m)) / 2, in.y + 1 + rows / 2, in.w, m, STYLE(C_DIM, 0));
    }

    for (int i = ui->procTop; i < imin(n, ui->procTop + rows); i++) {
        const ProcessInfo *p = ui->view[i];
        int y = in.y + 1 + (i - ui->procTop);
        int hl = ui->hoverPid && ui->hoverVisible && i == ui->hoverIdx &&
                 (s->color != COLOR_NONE || ui->nowMs < ui->flashUntil);

        if (hl) scr_fill(s, (Rect){ in.x, y, in.w, 1 }, ' ', hover_style(ui, C_TEXT));
#define ROWST(fg) (hl ? hover_style(ui, (fg)) : STYLE((fg), 0))

        if (p->pid == ui->expandedPid) {
            /* expanded: the whole command line and nothing else */
            char full[512], txt[520];
            if (process_cmdline(p->pid, full, sizeof full) != 0)
                snprintf(full, sizeof full, "%s", p->name);
            fit_text(full, cw, G->ell, txt, sizeof txt);
            Style st = ROWST(C_PROC);
            st.attr |= ATTR_BOLD;
            scr_puts(s, cx, y, cw, txt, st);
            continue;
        }

        char nm[96];
        scr_printf(s, pidX, y, pidW, ROWST(C_DIM), "%6d", p->pid);
        trunc_name(p->name, nameW, nm, sizeof nm);
        scr_puts(s, nameX, y, nameW, nm, ROWST(C_TEXT));
        scr_printf(s, cpuX, y, cpuW, ROWST(load_color(p->CPUPercent)), "%5.1f", p->CPUPercent);
        scr_printf(s, memX, y, memW, ROWST(load_color(p->memPercent)), "%5.1f", p->memPercent);
#undef ROWST
    }

    /* footer: controls, or a short message when the end of the list was hit */
    const char *k1[2] = { G->lr, G->ud };
    const char *d1[2] = { "column", "sort" };
    draw_hints(ui, cx, in.y + in.h - 2, cw, C_PROC, k1, d1, 2);

    int by = in.y + in.h - 1;
    if (ui->msg && ui->nowMs < ui->msgUntil) {
        scr_puts(s, cx + (cw - text_width(ui->msg)) / 2, by, cw, ui->msg, STYLE(C_WARN, ATTR_BOLD));
    } else {
        const char *k2[2] = { "pgup/pgdn", "enter" };
        const char *d2[2] = { "scroll", "name" };
        draw_hints(ui, cx, by, cw, C_PROC, k2, d2, 2);
    }
}

/* ------------------------------------------------------------------ */
/* Layouts                                                            */
/* ------------------------------------------------------------------ */

/* Rows a GPU box needs for its list (up to 3, or 2 plus a pager row) */
static int gpu_list_rows(const Ui *ui) {
    int rows, pager, perPage;
    list_layout((int)ui->gpus->count, 3, 1, &rows, &pager, &perPage);
    return rows + pager;
}

/* Smallest GPU box that still has room for a graph: borders + list + a 1-row graph frame */
static int gpu_box_min(const Ui *ui) { return 2 + gpu_list_rows(ui) + 3; }

/* The lower part of the full layout must fit the left column (20), or the
   middle column: network + memory (+ GPU when there is one) */
static int full_lower_min(const Ui *ui) {
    int mid = NET_SECTION_H + (ui->gpus->count > 0 ? gpu_box_min(ui) + 7 : 9);
    return imax(20, mid);
}

static int full_min_h(const Ui *ui) { return 13 + full_lower_min(ui); }

static void layout_full(Ui *ui, int W, int H) {
    /* the CPU box grows with the core count, up to 40% of the height */
    int rowsWanted = (ui->coreN + 1) / 2;
    int cpuH = clampi(rowsWanted + 7, 13, imax(13, H * 40 / 100));
    if (H - cpuH < full_lower_min(ui)) cpuH = H - full_lower_min(ui);
    int h2 = H - cpuH;

    int extra = W - FULL_MIN_W;
    int lw = 44 + extra * 30 / 100;
    int rw = 36 + extra * 30 / 100;
    int mw = W - lw - rw;

    draw_cpu(ui, (Rect){ 0, 0, W, cpuH }, 1);

    /* left column: drives grow with the drive count, the logo takes what is left.
       If the drives need the room, logo and controls sit side by side instead. */
    int n = (int)ui->disks->count;
    int need = 3 + imax(n, 1);
    Rect drv, logo, ctl;
    if (need + 4 + CONTROLS_H <= h2) {
        drv  = (Rect){ 0, cpuH, lw, need };
        logo = (Rect){ 0, cpuH + need, lw, h2 - need - CONTROLS_H };
        ctl  = (Rect){ 0, cpuH + h2 - CONTROLS_H, lw, CONTROLS_H };
    } else {
        int dh = imin(need, h2 - CONTROLS_H);
        int band = h2 - dh;
        drv  = (Rect){ 0, cpuH, lw, dh };
        logo = (Rect){ 0, cpuH + dh, lw - CONTROLS_NARROW_W, band };
        ctl  = (Rect){ lw - CONTROLS_NARROW_W, cpuH + dh, CONTROLS_NARROW_W, band };
    }
    draw_drives(ui, drv);
    draw_logo(ui, logo);

    /* middle column: the network box is fixed; GPU and memory share the rest */
    int upper = h2 - NET_SECTION_H;
    if (ui->gpus->count > 0) {
        int gpuMin = gpu_box_min(ui);
        int gpuH = gpuMin + imax(0, upper - gpuMin - 7) * 2 / 5;
        draw_gpu(ui, (Rect){ lw, cpuH, mw, gpuH }, 1);
        draw_mem(ui, (Rect){ lw, cpuH + gpuH, mw, upper - gpuH }, 1);
    } else {
        draw_mem(ui, (Rect){ lw, cpuH, mw, upper }, 1);
    }
    draw_net(ui, (Rect){ lw, cpuH + h2 - NET_SECTION_H, mw, NET_SECTION_H }, 1, 3);

    draw_procs(ui, (Rect){ lw + mw, cpuH, rw, h2 });

    /* last, so it knows which lists ended up split into pages in this very frame */
    draw_controls(ui, ctl);
}

static void draw_compact_hint(Ui *ui, int y, int w) {
    char arrows[24];
    snprintf(arrows, sizeof arrows, "%s%s", G->ud, G->lr);
    const char *k[5], *d[5];
    int count = build_hints(ui, 1, arrows, k, d);
    draw_hints(ui, 1, y, w - 2, C_LOGO, k, d, count);
}

static int compact_gpu_min(const Ui *ui) { int n = (int)ui->gpus->count; return n == 0 ? 0 : (n <= 1 ? 3 : 4); }
static int compact_drv_min(const Ui *ui) { return ui->disks->count <= 1 ? 4 : 5; }
static int compact_net_min(const Ui *ui) { return ui->nets->count <= 2 ? 3 : 4; }

/* CPU 7 + memory 4 + the rest at their smallest, plus the key hint row */
static int compact_min_h(const Ui *ui) {
    int h = 1 + 7 + 4 + compact_drv_min(ui) + compact_net_min(ui) + compact_gpu_min(ui);
    return imax(COMPACT_MIN_H, h);
}

/* No logo, no graphs, no controls box: everything is squeezed into a small footprint */
static void layout_compact(Ui *ui, int W, int H) {
    W = imin(W, COMPACT_MAX_W);
    H = imin(H, COMPACT_MAX_H);
    int leftW = clampi(W * 58 / 100, 46, 64);
    int rightW = W - leftW;
    int Hl = H - 1;                                  /* last row is the key hint line */

    int ncols = imax(1, (leftW - 4 + 1) / 18);
    int cpuNeed = 2 + 3 + imax(1, (ui->coreN + ncols - 1) / ncols) + 1;
    int n = (int)ui->disks->count, k = (int)ui->nets->count, ng = (int)ui->gpus->count;
    int drvNeed = 3 + imax(n, 1);
    int netNeed = 2 + imax(1, (k + 1) / 2);
    int gpuNeed = ng > 0 ? 2 + ng : 0;

    int cpuH = 7, memH = 4, gpuH = compact_gpu_min(ui), drvH = compact_drv_min(ui), netH = compact_net_min(ui);
    int extra = Hl - (cpuH + memH + gpuH + drvH + netH);
    while (extra > 0 && (cpuH < cpuNeed || gpuH < gpuNeed || drvH < drvNeed || netH < netNeed)) {
        if (extra > 0 && cpuH < cpuNeed) { cpuH++; extra--; }
        if (extra > 0 && gpuH < gpuNeed) { gpuH++; extra--; }
        if (extra > 0 && drvH < drvNeed) { drvH++; extra--; }
        if (extra > 0 && netH < netNeed) { netH++; extra--; }
    }
    if (extra > 0) cpuH += extra;                    /* everything already fits: leftover is blank space */

    int y = 0;
    draw_cpu(ui, (Rect){ 0, y, leftW, cpuH }, 0);          y += cpuH;
    if (ng > 0) { draw_gpu(ui, (Rect){ 0, y, leftW, gpuH }, 0); y += gpuH; }
    draw_mem(ui, (Rect){ 0, y, leftW, memH }, 0);          y += memH;
    draw_drives(ui, (Rect){ 0, y, leftW, drvH });          y += drvH;
    draw_net(ui, (Rect){ 0, y, leftW, netH }, 0, netH - 2);
    draw_procs(ui, (Rect){ leftW, 0, rightW, Hl });
    draw_compact_hint(ui, H - 1, W);
}

static void draw_too_small(Ui *ui) {
    Screen *s = ui->scr;
    const char *m = "Terminal window too small!";
    char sz[64];
    snprintf(sz, sizeof sz, "(%dx%d, need %dx%d)", s->w, s->h, COMPACT_MIN_W, compact_min_h(ui));

    int y = s->h / 2 - (s->h > 1 ? 1 : 0);
    scr_puts(s, imax(0, (s->w - text_width(m)) / 2), y, s->w, m, STYLE(C_BAD, ATTR_BOLD));
    if (s->h > 1)
        scr_puts(s, imax(0, (s->w - text_width(sz)) / 2), y + 1, s->w, sz, STYLE(C_DIM, 0));
}

void ui_draw(Ui *ui, double nowMs) {
    Screen *s = ui->scr;
    scr_clear(s);

    ui->nowMs = nowMs;
    ui->animNext = 0;                                /* set again by the logo if it is on screen */
    if (ui->flashUntil && nowMs >= ui->flashUntil) ui->flashUntil = 0;
    if (ui->msgUntil && nowMs >= ui->msgUntil) { ui->msgUntil = 0; ui->msg = NULL; }

    int W = s->w, H = s->h;
    int layout;
    if (ui->sizeUnknown)                                   layout = LAYOUT_COMPACT;
    else if (!ui->forceCompact && W >= FULL_MIN_W && H >= full_min_h(ui)) layout = LAYOUT_FULL;
    else if (W >= COMPACT_MIN_W && H >= compact_min_h(ui)) layout = LAYOUT_COMPACT;
    else                                                   layout = LAYOUT_TOO_SMALL;

    switch (layout) {
        case LAYOUT_FULL:    layout_full(ui, W, H); break;
        case LAYOUT_COMPACT: layout_compact(ui, W, H); break;
        default:             draw_too_small(ui); break;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static void push_cpu_sample(Ui *ui, double t) {
    for (int i = 0; i < ui->coreN; i++) ui->scratch[i] = ui->cpu->cores[i].usagePercent;
    hist_push(&ui->cpuHist, t, ui->scratch);
}

static void exit_select_mode(Ui *ui, double nowMs) {
    if (ui->coreN > 0 && memcmp(ui->coreSel, ui->coreEdit, (size_t)ui->coreN) != 0) {
        /* the selection changed: redraw the graph and start tracking from 0 seconds */
        memcpy(ui->coreSel, ui->coreEdit, (size_t)ui->coreN);
        hist_clear(&ui->cpuHist);
        push_cpu_sample(ui, nowMs / 1000.0);
    }
    ui->selMode = 0;
}

static int page_step(int *page, int pages, int d) {
    if (pages < 1) pages = 1;
    int np = clampi(*page + d, 0, pages - 1);
    int changed = np != *page;
    *page = np;
    return changed;
}

static int key_select_mode(Ui *ui, const Key *k, double nowMs) {
    int n = ui->coreN;
    int cur = ui->selCursor;

    switch (k->code) {
        case K_ESC:   exit_select_mode(ui, nowMs); return UI_REDRAW;
        case K_UP:    cur -= 1; break;
        case K_DOWN:  cur += 1; break;
        case K_LEFT:  cur -= imax(1, ui->coreRows); break;
        case K_RIGHT: cur += imax(1, ui->coreRows); break;
        case K_ENTER:
            if (n > 0) ui->coreEdit[cur] = !ui->coreEdit[cur];
            return UI_REDRAW;
        case K_CHAR:
            if (k->ch == ' ') {
                if (n > 0) ui->coreEdit[cur] = !ui->coreEdit[cur];
                return UI_REDRAW;
            }
            if (k->ch == 'a' || k->ch == 'A') {
                int all = 1;
                for (int i = 0; i < n; i++) if (!ui->coreEdit[i]) { all = 0; break; }
                memset(ui->coreEdit, all ? 0 : 1, (size_t)n);      /* everything on -> clear, else select all */
                return UI_REDRAW;
            }
            return UI_NONE;
        default:
            return UI_NONE;
    }
    if (n > 0) {
        ui->selCursor = clampi(cur, 0, n - 1);
        ui->corePage = ui->selCursor / imax(1, ui->corePerPage);
    }
    return UI_REDRAW;
}

/* Moves the hover cursor. step rows up (-) or down (+). */
static int move_hover(Ui *ui, int step, double nowMs) {
    build_view(ui);
    int n = (int)ui->viewCount;
    if (n == 0) return UI_NONE;

    if (!ui->hoverPid) {
        /* first interaction: start on the top visible row */
        ui->hoverIdx = clampi(ui->procTop, 0, n - 1);
        ui->hoverPid = ui->view[ui->hoverIdx]->pid;
        ui->hoverVisible = 1;
    } else {
        int idx = ui->hoverIdx;
        int atEdge = (step < 0 && idx <= 0) || (step > 0 && idx >= n - 1);
        ui->hoverVisible = 1;
        if (atEdge) {
            /* tried to scroll past the end: say so, and hide the selection until next time */
            ui->msg = step < 0 ? "Start of list" : "End of list";
            ui->msgUntil = nowMs + MSG_MS;
            ui->hoverVisible = 0;
            return UI_REDRAW;
        }
        ui->hoverIdx = clampi(idx + step, 0, n - 1);
        ui->hoverPid = ui->view[ui->hoverIdx]->pid;
    }
    keep_hover_in_view(ui);
    ui->lastListInteraction = nowMs;
    if (ui->scr->color == COLOR_NONE) ui->flashUntil = nowMs + FLASH_MS;
    return UI_REDRAW;
}

static int key_process(Ui *ui, const Key *k, double nowMs) {
    int page = imax(1, ui->procRows - 1);
    switch (k->code) {
        case K_LEFT:
        case K_RIGHT: {
            int c = clampi(ui->sortCol + (k->code == K_RIGHT ? 1 : -1), 0, COL_COUNT - 1);
            if (c != ui->sortCol) {
                ui->sortCol = c;
                ui->sortDesc = (c == COL_CPU || c == COL_MEM);   /* biggest first for numbers */
            }
            ui->frozenActive = 0;                                /* re-sort right away */
            return UI_REDRAW;
        }
        case K_UP:   ui->sortDesc = 0; ui->frozenActive = 0; return UI_REDRAW;
        case K_DOWN: ui->sortDesc = 1; ui->frozenActive = 0; return UI_REDRAW;
        case K_PGUP: return move_hover(ui, -(k->mod ? page : PROC_SCROLL_STEP), nowMs);
        case K_PGDN: return move_hover(ui,  (k->mod ? page : PROC_SCROLL_STEP), nowMs);
        case K_HOME: return move_hover(ui, -(int)ui->procs->count - 1, nowMs);
        case K_END:  return move_hover(ui,  (int)ui->procs->count + 1, nowMs);
        case K_ENTER: {
            if (!ui->hoverPid || !ui->hoverVisible) return UI_NONE;
            build_view(ui);
            const ProcessInfo *p = (ui->hoverIdx < (int)ui->viewCount) ? ui->view[ui->hoverIdx] : NULL;
            if (!p) return UI_NONE;
            if (p->pid == ui->expandedPid) {
                ui->expandedPid = 0;                                  /* back to the normal row */
            } else if (text_width(p->name) > ui->procNameW) {
                ui->expandedPid = p->pid;                             /* only names that got cut */
            } else {
                return UI_NONE;
            }
            ui->lastListInteraction = nowMs;
            return UI_REDRAW;
        }
        default:
            return UI_NONE;
    }
}

int ui_key(Ui *ui, const Key *k, double nowMs) {
    ui->nowMs = nowMs;
    if (k->code == K_CHAR && (k->ch == 'q' || k->ch == 'Q')) return UI_QUIT;

    if (k->code == K_CHAR) {
        int changed = 0, handled = 1;
        switch (k->ch) {
            case '<': case ',':
                changed = page_step(&ui->corePage, ui->corePages, -1);
                if (ui->selMode) ui->selCursor = imin(ui->coreN - 1, ui->corePage * ui->corePerPage);
                break;
            case '>': case '.':
                changed = page_step(&ui->corePage, ui->corePages, 1);
                if (ui->selMode) ui->selCursor = imin(ui->coreN - 1, ui->corePage * ui->corePerPage);
                break;
            case '[': changed = page_step(&ui->drivePage, ui->drivePages, -1); break;
            case ']': changed = page_step(&ui->drivePage, ui->drivePages, 1); break;
            case '{': changed = page_step(&ui->gpuPage, ui->gpuPages, -1); break;
            case '}': changed = page_step(&ui->gpuPage, ui->gpuPages, 1); break;
            case '(': changed = page_step(&ui->netPage, ui->netPages, -1); break;
            case ')': changed = page_step(&ui->netPage, ui->netPages, 1); break;
            default:  handled = 0; break;
        }
        if (handled) return changed ? UI_REDRAW : UI_NONE;
    }

    if (ui->selMode) return key_select_mode(ui, k, nowMs);

    if (k->code == K_CHAR && (k->ch == 'c' || k->ch == 'C')) {
        if (ui->coreN == 0) return UI_NONE;
        memcpy(ui->coreEdit, ui->coreSel, (size_t)ui->coreN);
        ui->selMode = 1;
        ui->selCursor = 0;                       /* the cursor starts next to core0 */
        ui->corePage = 0;
        return UI_REDRAW;
    }
    return key_process(ui, k, nowMs);
}

long ui_timeout(const Ui *ui, double nowMs) {
    double best = -1;
    if (ui->scr->color == COLOR_NONE && ui->flashUntil > nowMs) best = ui->flashUntil - nowMs;
    if (ui->msgUntil > nowMs && (best < 0 || ui->msgUntil - nowMs < best)) best = ui->msgUntil - nowMs;
    if (ui->animNext > 0) {                                        /* the logo wants another frame */
        double d = ui->animNext - nowMs;
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    return best < 0 ? -1 : (long)ceil(best);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

Ui *ui_create(Screen *scr, CpuInfo *cpu, MemoryInfo *mem, DiskList *disks, NetworkList *nets,
              GpuList *gpus, ProcessList *procs, long intervalMs, int forceCompact) {
    Ui *ui = calloc(1, sizeof *ui);
    if (!ui) return NULL;

    ui->scr = scr;
    ui->cpu = cpu;
    ui->mem = mem;
    ui->disks = disks;
    ui->nets = nets;
    ui->gpus = gpus;
    ui->procs = procs;
    ui->forceCompact = forceCompact;

    ui->coreN = (int)cpu->coreCount;
    ui->coreSel  = calloc((size_t)imax(1, ui->coreN), 1);
    ui->coreEdit = calloc((size_t)imax(1, ui->coreN), 1);
    ui->scratchN = imax(2, ui->coreN);
    ui->scratch  = calloc((size_t)ui->scratchN, sizeof *ui->scratch);
    for (int i = 0; i < ui->coreN && i < DEFAULT_CORES_SELECTED; i++) ui->coreSel[i] = 1;
    int ng = (int)gpus->count;
    ui->gpuScratch = calloc((size_t)imax(2, 2 * ng), sizeof *ui->gpuScratch);

    int cap = (int)(GRAPH_WINDOW_SECONDS * 1000.0 / (double)intervalMs) + 16;
    if (!ui->coreSel || !ui->coreEdit || !ui->scratch || !ui->gpuScratch ||
        hist_init(&ui->gpuHist, 2 * ng, cap) != 0 ||
        hist_init(&ui->cpuHist, ui->coreN, cap) != 0 ||
        hist_init(&ui->memHist, 2, cap) != 0 ||
        hist_init(&ui->netHist, 2, cap) != 0) {
        ui_destroy(ui);
        return NULL;
    }

    ui->corePages = ui->drivePages = ui->netPages = ui->gpuPages = 1;    /* real values come with the first draw */
    ui->corePerPage = ui->drivePerPage = ui->netPerPage = ui->gpuPerPage = 1;
    ui->coreRows = 1;

    ui->sortCol = COL_CPU;                       /* CPU% descending by default */
    ui->sortDesc = 1;
    ui->procRows = 10;
    ui->procNameW = 12;
    return ui;
}

void ui_destroy(Ui *ui) {
    if (!ui) return;
    hist_free(&ui->cpuHist);
    hist_free(&ui->memHist);
    hist_free(&ui->netHist);
    hist_free(&ui->gpuHist);
    free(ui->gpuScratch);
    free(ui->coreSel);
    free(ui->coreEdit);
    free(ui->scratch);
    free(ui->view);
    free(ui->frozenPid);
    free(ui);
}

void ui_set_size(Ui *ui, int cols, int rows) {
    ui->sizeUnknown = (cols <= 0 || rows <= 0);
    if (ui->sizeUnknown) {                       /* unknown size: draw compact at 80x24 */
        cols = 80;
        rows = 24;
    }
    if (cols != ui->scr->w || rows != ui->scr->h) scr_resize(ui->scr, cols, rows);
    ui->scr->prevValid = 0;                      /* the terminal may have garbled the old frame */
}

void ui_push_samples(Ui *ui, double t) {
    if (ui->coreN > 0) push_cpu_sample(ui, t);

    float m[2] = { ui->mem->mainUsagePercent, ui->mem->swapUsagePercent };
    hist_push(&ui->memHist, t, m);

    double down = 0, up = 0;
    for (size_t i = 0; i < ui->nets->count; i++) {
        down += size_to_bytes(ui->nets->items[i].downPerSecond);
        up   += size_to_bytes(ui->nets->items[i].upPerSecond);
    }
    float nv[2] = { (float)down, (float)up };
    hist_push(&ui->netHist, t, nv);

    int ng = (int)ui->gpus->count;
    if (ng > 0) {
        for (int i = 0; i < ng; i++) {
            const GpuInfo *g = &ui->gpus->items[i];
            ui->gpuScratch[i] = g->hasUsage ? g->usagePercent : 0.0f;    /* asleep or unknown plots as idle */
            ui->gpuScratch[ng + i] = g->hasMem ? g->memPercent : 0.0f;
        }
        hist_push(&ui->gpuHist, t, ui->gpuScratch);
    }
}