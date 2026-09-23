#ifndef TERM_H
#define TERM_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Colour                                                             */
/* ------------------------------------------------------------------ */

typedef enum { COLOR_NONE = 0, COLOR_8, COLOR_256 } ColorMode;

/* Semantic colours. Each one maps to a 256-colour code and an 8-colour
   fallback in term.c, so drawing code never deals with raw escape codes. */
enum {
    C_DEF = 0,
    C_TEXT, C_DIM,
    C_CPU, C_DISK, C_MEM, C_NET, C_PROC, C_GPU, C_LOGO,
    C_GOOD, C_WARN, C_BAD,
    C_SWAP, C_NETDOWN, C_NETUP,
    C_HOVER,                                   /* background of the hovered process row */
    C_LOGO0, C_LOGO_LAST = C_LOGO0 + 23,        /* LOGO_STOPS colours forming a loop, for the logo's gradient */
    C_CORE0,                                   /* first of CORE_COLORS per-core colours */
    C_COUNT = C_CORE0 + 32
};
#define CORE_COLORS 32
#define LOGO_STOPS  24                 /* keep in step with C_LOGO_LAST above */

#define ATTR_BOLD 1
#define ATTR_DIM  2
#define ATTR_REV  4
#define ATTR_UL   8

/* A colour theme. It only sets the 256-colour palette: the per-core colours stay fixed so
   cores remain distinguishable in every theme, and the 8-colour fallback has too few hues to vary. */
typedef struct {
    const char *name;
    short text, dim;
    short cpu, disk, mem, net, proc, gpu, logo;        /* section colours (xterm-256 codes) */
    short good, warn, bad;                             /* usage: low / medium / high */
    short swap, netdown, netup;
    short hover;                                       /* background of the hovered process row */
    short logoStops[LOGO_STOPS];                       /* the logo's animated gradient, as a seamless loop */
} Theme;

extern const Theme THEMES[];                           /* THEMES[0] is the default */
extern const int THEME_COUNT;
const Theme *theme_find(const char *name);             /* case-insensitive, NULL if unknown */
void term_set_theme(const Theme *theme);               /* call before drawing */

typedef struct { uint8_t fg, bg, attr; } Style;
#define STYLE(fg, attr) ((Style){ (uint8_t)(fg), C_DEF, (uint8_t)(attr) })

typedef struct { uint32_t ch; uint8_t fg, bg, attr; } Cell;
typedef struct { int x, y, w, h; } Rect;

/* Box drawing and symbols, with an ASCII set for non-UTF-8 terminals */
typedef struct {
    const char *tl, *tr, *bl, *br, *h, *v;
    const char *bar_full, *bar_empty, *bar_part[8];
    const char *lr, *ud;                       /* "←→" and "↑↓" */
    const char *aup, *adn;                     /* single arrows: "↑" and "↓" */
    const char *asc, *desc;                    /* sort direction markers */
    const char *cursor;
    const char *ell;                           /* single-character ellipsis */
} Glyphs;
extern const Glyphs *G;

typedef struct {
    int w, h;
    Cell *cells;                               /* frame being drawn */
    Cell *prev;                                /* what the terminal currently shows */
    int prevValid;
    ColorMode color;
    int unicode;
    int sgr;                                   /* 0 for TERM=dumb: emit no SGR at all */
} Screen;

/* ------------------------------------------------------------------ */
/* Terminal                                                           */
/* ------------------------------------------------------------------ */

/* Decides the colour mode from --no-color, NO_COLOR, TERM, COLORTERM and `tput colors`.
   *sgrOk is set to 0 when the terminal is "dumb" (no attributes either). */
ColorMode term_detect_color(int noColorFlag, int *sgrOk);
int  term_is_utf8(void);
void term_get_size(int *cols, int *rows);      /* 0, 0 when the size can't be read */
int  term_enter(void);                         /* raw mode + alternate screen */
void term_leave(void);                         /* safe to call more than once */

/* ------------------------------------------------------------------ */
/* Screen buffer                                                      */
/* ------------------------------------------------------------------ */

void scr_init(Screen *s, int w, int h, ColorMode mode, int unicode, int sgrOk);
void scr_resize(Screen *s, int w, int h);
void scr_free(Screen *s);
void scr_clear(Screen *s);
void scr_flush(Screen *s);                     /* writes only what changed */

void scr_putc(Screen *s, int x, int y, uint32_t ch, Style st);
/* Draws UTF-8 text clipped to maxw cells. Returns the cells used. */
int  scr_puts(Screen *s, int x, int y, int maxw, const char *utf8, Style st);
int  scr_printf(Screen *s, int x, int y, int maxw, Style st, const char *fmt, ...)
         __attribute__((format(printf, 6, 7)));
int  scr_puts_right(Screen *s, int xEnd, int y, int maxw, const char *utf8, Style st);
void scr_fill(Screen *s, Rect r, uint32_t ch, Style st);

int  text_width(const char *utf8);
/* Copies s into out, cut to w cells with `ell` appended when it doesn't fit */
void fit_text(const char *s, int w, const char *ell, char *out, size_t outn);

/* ------------------------------------------------------------------ */
/* Keyboard                                                           */
/* ------------------------------------------------------------------ */

typedef struct { int code; int ch; int mod; } Key;
enum { K_NONE = 0, K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN,
       K_HOME, K_END, K_ENTER, K_ESC };

int term_fill_input(void);                     /* reads pending bytes; -1 on EOF/error */
int term_parse_key(Key *k);                    /* 1 if a key was produced */

#endif /* TERM_H */