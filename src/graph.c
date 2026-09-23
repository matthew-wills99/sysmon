#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"

int hist_init(History *h, int nseries, int cap) {
    memset(h, 0, sizeof *h);
    if (nseries < 1) nseries = 1;
    if (cap < 8) cap = 8;
    h->nseries = nseries;
    h->cap = cap;
    h->t = calloc((size_t)cap, sizeof *h->t);
    h->v = calloc((size_t)cap * (size_t)nseries, sizeof *h->v);
    if (!h->t || !h->v) {
        hist_free(h);
        return -1;
    }
    return 0;
}

void hist_free(History *h) {
    free(h->t);
    free(h->v);
    memset(h, 0, sizeof *h);
}

void hist_clear(History *h) {
    h->head = 0;
    h->count = 0;
}

static int phys(const History *h, int i) {
    return (h->head + i) % h->cap;
}

void hist_push(History *h, double t, const float *vals) {
    int slot;
    if (h->count < h->cap) {
        slot = phys(h, h->count);
        h->count++;
    } else {
        slot = h->head;                        /* overwrite the oldest */
        h->head = (h->head + 1) % h->cap;
    }
    h->t[slot] = t;
    memcpy(h->v + (size_t)slot * (size_t)h->nseries, vals, (size_t)h->nseries * sizeof *vals);
}

double hist_last_time(const History *h) {
    return h->count ? h->t[phys(h, h->count - 1)] : 0.0;
}

static float value_at(const History *h, int i, int series) {
    return h->v[(size_t)phys(h, i) * (size_t)h->nseries + (size_t)series];
}

int hist_value_at(const History *h, int series, double t, float *out) {
    if (h->count == 0 || series < 0 || series >= h->nseries) return 0;

    double t0 = h->t[phys(h, 0)];
    double tn = h->t[phys(h, h->count - 1)];
    if (t < t0) return 0;
    if (t >= tn) {
        *out = value_at(h, h->count - 1, series);
        return 1;
    }

    int lo = 0, hi = h->count - 1;               /* find the samples either side of t */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (h->t[phys(h, mid)] <= t) lo = mid;
        else hi = mid;
    }
    double ta = h->t[phys(h, lo)], tb = h->t[phys(h, hi)];
    float va = value_at(h, lo, series), vb = value_at(h, hi, series);
    double f = (tb > ta) ? (t - ta) / (tb - ta) : 0.0;
    *out = (float)(va + (vb - va) * f);
    return 1;
}

double hist_max(const History *h, const GraphLine *lines, int nlines) {
    double m = 0;
    for (int l = 0; l < nlines; l++)
        for (int i = 0; i < h->count; i++) {
            double v = value_at(h, i, lines[l].series);
            if (v > m) m = v;
        }
    return m;
}

/* Braille dot bit for a (column, row) position within a 2x4 cell */
static const uint8_t braille_bit[2][4] = { { 0x01, 0x02, 0x04, 0x40 },
                                           { 0x08, 0x10, 0x20, 0x80 } };

void graph_draw(Screen *s, Rect a, const History *h,
                const GraphLine *lines, int nlines, double ymax) {
    if (a.w < 1 || a.h < 1 || h->count == 0 || nlines < 1 || ymax <= 0) return;

    int dx = s->unicode ? 2 : 1;
    int dy = s->unicode ? 4 : 1;
    int W2 = a.w * dx, H4 = a.h * dy;

    uint8_t *bits = calloc((size_t)a.w * (size_t)a.h, 1);
    uint8_t *col  = calloc((size_t)a.w * (size_t)a.h, 1);
    if (!bits || !col) { free(bits); free(col); return; }

    double tEnd = hist_last_time(h);

    for (int l = 0; l < nlines; l++) {
        int prevY = -1;
        for (int x = 0; x < W2; x++) {
            /* x = W2-1 is "0 seconds" (newest), x = 0 is "-60 seconds" */
            double t = tEnd - GRAPH_WINDOW_SECONDS * (double)(W2 - 1 - x) / (W2 > 1 ? (double)(W2 - 1) : 1.0);
            float v;
            if (!hist_value_at(h, lines[l].series, t, &v)) { prevY = -1; continue; }

            double f = v / ymax;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            int y = (H4 - 1) - (int)lround(f * (H4 - 1));

            /* join this dot to the previous column so steep changes stay connected */
            int y0 = y, y1 = y;
            if (prevY >= 0) {
                y0 = prevY < y ? prevY : y;
                y1 = prevY < y ? y : prevY;
            }
            for (int yy = y0; yy <= y1; yy++) {
                int cell = (yy / dy) * a.w + (x / dx);
                bits[cell] |= s->unicode ? braille_bit[x % dx][yy % dy] : 1;
                col[cell] = lines[l].color;
            }
            prevY = y;
        }
    }

    for (int cy = 0; cy < a.h; cy++)
        for (int cx = 0; cx < a.w; cx++) {
            int cell = cy * a.w + cx;
            if (!bits[cell]) continue;
            uint32_t ch = s->unicode ? (0x2800u + bits[cell]) : '*';
            scr_putc(s, a.x + cx, a.y + cy, ch, STYLE(col[cell], 0));
        }

    free(bits);
    free(col);
}