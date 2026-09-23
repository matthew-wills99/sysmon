#ifndef UI_H
#define UI_H

#include "cpu.h"
#include "disk.h"
#include "gpu.h"
#include "mem.h"
#include "net.h"
#include "process.h"
#include "term.h"

typedef struct Ui Ui;

enum { UI_NONE = 0, UI_REDRAW = 1, UI_QUIT = 2 };

Ui  *ui_create(Screen *scr, CpuInfo *cpu, MemoryInfo *mem, DiskList *disks, NetworkList *nets,
               GpuList *gpus, ProcessList *procs, long intervalMs, int forceCompact);
void ui_destroy(Ui *ui);

/* Call with the size the terminal reports; 0 x 0 means "unknown" and gives compact mode */
void ui_set_size(Ui *ui, int cols, int rows);

/* Records the latest readings in the graph histories (t in seconds) */
void ui_push_samples(Ui *ui, double t);

int  ui_key(Ui *ui, const Key *k, double nowMs);

/* Milliseconds until the UI needs redrawing on its own (flash / message
   expiry), or -1 if nothing is pending */
long ui_timeout(const Ui *ui, double nowMs);

void ui_draw(Ui *ui, double nowMs);

#endif /* UI_H */