#ifndef GRAPH_H
#define GRAPH_H

#include "term.h"

#define GRAPH_WINDOW_SECONDS 60.0

/* A ring buffer of time-stamped samples, several series per sample */
typedef struct {
    int nseries;
    int cap;
    int head;            /* index of the oldest sample */
    int count;
    double *t;           /* [cap]            seconds */
    float  *v;           /* [cap * nseries]  values  */
} History;

typedef struct {
    int series;
    uint8_t color;
} GraphLine;

int    hist_init(History *h, int nseries, int cap);
void   hist_free(History *h);
void   hist_clear(History *h);
void   hist_push(History *h, double t, const float *vals);
double hist_last_time(const History *h);
/* Linearly interpolated value at time t. Returns 0 if t is before the first sample. */
int    hist_value_at(const History *h, int series, double t, float *out);
double hist_max(const History *h, const GraphLine *lines, int nlines);

/* Plots lines over the last GRAPH_WINDOW_SECONDS ending at the newest sample,
   using braille dots (2x4 per cell) or one '*' per cell on ASCII terminals.
   Values are scaled so that ymax sits on the top row. */
void   graph_draw(Screen *s, Rect area, const History *h,
                  const GraphLine *lines, int nlines, double ymax);

#endif /* GRAPH_H */