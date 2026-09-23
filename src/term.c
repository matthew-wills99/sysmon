#define _GNU_SOURCE

#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "term.h"

/* ------------------------------------------------------------------ */
/* Glyph sets                                                         */
/* ------------------------------------------------------------------ */

static const Glyphs G_UNICODE = {
    .tl = "╭", .tr = "╮", .bl = "╰", .br = "╯", .h = "─", .v = "│",
    .bar_full = "█", .bar_empty = "·",
    .bar_part = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" },
    .lr = "←→", .ud = "↑↓", .aup = "↑", .adn = "↓", .asc = "▲", .desc = "▼", .cursor = "▸", .ell = "…",
};

static const Glyphs G_ASCII = {
    .tl = "+", .tr = "+", .bl = "+", .br = "+", .h = "-", .v = "|",
    .bar_full = "#", .bar_empty = "-",
    .bar_part = { "", "", "", "", "", "", "", "" },
    .lr = "</>", .ud = "^/v", .aup = "^", .adn = "v", .asc = "^", .desc = "v", .cursor = ">", .ell = "~",
};

const Glyphs *G = &G_UNICODE;

/* ------------------------------------------------------------------ */
/* Palette                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    short c256;              /* xterm-256 colour code, -1 = terminal default */
    signed char c8;          /* 0-7, -1 = terminal default */
    unsigned char attr8;     /* extra attributes in 8-colour mode (bright via bold) */
    unsigned char attrLow;   /* extra attributes in 8-colour and no-colour modes */
} PalEntry;

static PalEntry pal[C_COUNT];

static const short core256[CORE_COLORS] = {
    203, 214, 227, 118,  84,  49,  45,  75,
    111, 141, 177, 213, 167, 172, 185, 149,
     71,  43,  38,  68, 105, 134, 169, 175,
    209, 221, 191, 156, 121,  87, 123, 147,
};

/* ------------------------------------------------------------------ */
/* Themes                                                             */
/* ------------------------------------------------------------------ */

/* Field order: text dim | cpu disk mem net proc gpu logo | good warn bad | swap netdown netup | hover | logo gradient.
   The gradient is a loop of LOGO_STOPS colours that ends where it starts, so it can drift round forever without a seam.
   The section colours were picked to stay easy to tell apart (each pair well separated in CIE Lab). */
const Theme THEMES[] = {
    { "default", 252, 244,   141,  79, 208, 220,  75, 196, 210,    78, 221, 203,   222, 114, 209,   238,
      { 224, 224, 218, 218, 217, 217, 211, 211, 210, 210, 204, 204,
        203, 203, 204, 204, 210, 210, 211, 211, 217, 217, 218, 218 } },                                   /* pale pink <-> coral */

    { "ocean",   253, 103,   135, 149,  45,  86,  33, 209, 183,    84, 222, 203,   222, 121, 213,    24,
      { 123, 123, 87, 81, 75, 69, 63, 99, 105, 141, 147, 183,
        183, 183, 147, 141, 105, 99, 63, 69, 75, 81, 87, 123 } },                                   /* cyan <-> blue <-> lilac */

    { "neon",    255, 245,   135,  46, 226,  51,  39, 196, 199,    46, 226, 196,   214,  46, 199,    54,
      { 51, 45, 39, 33, 69, 63, 99, 135, 171, 207, 201, 200,
        199, 200, 201, 207, 171, 135, 99, 63, 69, 33, 39, 45 } },                                   /* cyan <-> blue <-> magenta <-> hot pink */
};
const int THEME_COUNT = (int)(sizeof THEMES / sizeof THEMES[0]);

const Theme *theme_find(const char *name) {
    for (int i = 0; i < THEME_COUNT; i++)
        if (strcasecmp(name, THEMES[i].name) == 0) return &THEMES[i];
    return NULL;
}

_Static_assert(C_LOGO_LAST - C_LOGO0 + 1 == LOGO_STOPS, "C_LOGO_LAST and LOGO_STOPS disagree");

static void apply_theme(const Theme *t) {
    pal[C_TEXT].c256    = t->text;    pal[C_DIM].c256     = t->dim;
    pal[C_CPU].c256     = t->cpu;     pal[C_DISK].c256    = t->disk;
    pal[C_MEM].c256     = t->mem;     pal[C_NET].c256     = t->net;
    pal[C_PROC].c256    = t->proc;    pal[C_GPU].c256     = t->gpu;
    pal[C_LOGO].c256    = t->logo;
    pal[C_GOOD].c256    = t->good;    pal[C_WARN].c256    = t->warn;    pal[C_BAD].c256 = t->bad;
    pal[C_SWAP].c256    = t->swap;    pal[C_NETDOWN].c256 = t->netdown; pal[C_NETUP].c256 = t->netup;
    pal[C_HOVER].c256   = t->hover;
    for (int i = 0; i < LOGO_STOPS; i++) pal[C_LOGO0 + i].c256 = t->logoStops[i];
}

/* The 8-colour hues and attributes never change; the 256-colour codes come from the theme */
static void pal_init(void) {
    static int done;
    if (done) return;
    done = 1;

    pal[C_DEF]     = (PalEntry){ -1, -1, 0, 0 };
    pal[C_TEXT]    = (PalEntry){ -1,  7, 0, 0 };
    pal[C_DIM]     = (PalEntry){ -1, -1, 0, ATTR_DIM };
    pal[C_CPU]     = (PalEntry){ -1,  5, 0, 0 };
    pal[C_DISK]    = (PalEntry){ -1,  2, 0, 0 };
    pal[C_MEM]     = (PalEntry){ -1,  3, 0, 0 };
    pal[C_NET]     = (PalEntry){ -1,  6, 0, 0 };
    pal[C_PROC]    = (PalEntry){ -1,  4, 0, 0 };
    pal[C_GPU]     = (PalEntry){ -1,  1, 0, 0 };
    pal[C_LOGO]    = (PalEntry){ -1,  7, ATTR_BOLD, 0 };     /* red belongs to the GPU in 8-colour mode */
    pal[C_GOOD]    = (PalEntry){ -1,  2, 0, 0 };
    pal[C_WARN]    = (PalEntry){ -1,  3, 0, 0 };
    pal[C_BAD]     = (PalEntry){ -1,  1, ATTR_BOLD, 0 };
    pal[C_SWAP]    = (PalEntry){ -1,  7, 0, 0 };
    pal[C_NETDOWN] = (PalEntry){ -1,  2, 0, 0 };
    pal[C_NETUP]   = (PalEntry){ -1,  5, 0, 0 };
    pal[C_HOVER]   = (PalEntry){ -1, -1, 0, 0 };              /* background only */

    for (int i = 0; i < LOGO_STOPS; i++)
        pal[C_LOGO0 + i] = (PalEntry){ -1, 7, (i % 2) ? ATTR_BOLD : 0, 0 };

    /* Unique per-core colours. With only 6 hues available in 8-colour mode the
       colours repeat, so alternate plain/bold to at least get 12 combinations. */
    for (int i = 0; i < CORE_COLORS; i++)
        pal[C_CORE0 + i] = (PalEntry){ core256[i], (signed char)((i % 6) + 1),
                                       ((i / 6) % 2) ? ATTR_BOLD : 0, 0 };

    apply_theme(&THEMES[0]);
}

void term_set_theme(const Theme *theme) {
    pal_init();
    apply_theme(theme ? theme : &THEMES[0]);
}

/* ------------------------------------------------------------------ */
/* Capability detection                                               */
/* ------------------------------------------------------------------ */

ColorMode term_detect_color(int noColorFlag, int *sgrOk) {
    *sgrOk = 1;

    const char *term = getenv("TERM");
    if (!term || !*term || strcmp(term, "dumb") == 0) {
        *sgrOk = 0;
        return COLOR_NONE;
    }
    if (noColorFlag) return COLOR_NONE;

    const char *nc = getenv("NO_COLOR");
    if (nc && *nc) return COLOR_NONE;

    /* Truecolor terminals are given the 256-colour palette */
    const char *ct = getenv("COLORTERM");
    if (ct && (strcmp(ct, "truecolor") == 0 || strcmp(ct, "24bit") == 0))
        return COLOR_256;

    FILE *p = popen("tput colors 2>/dev/null", "r");
    if (!p) return COLOR_NONE;

    char buf[32];
    char *got = fgets(buf, sizeof buf, p);
    int rc = pclose(p);
    if (!got || rc != 0) return COLOR_NONE;             /* tput failed or printed nothing */

    char *end;
    long n = strtol(buf, &end, 10);
    if (end == buf) return COLOR_NONE;
    if (n < 8)      return COLOR_NONE;
    if (n < 256)    return COLOR_8;
    return COLOR_256;
}

int term_is_utf8(void) {
    const char *cs = nl_langinfo(CODESET);
    return cs && (strcmp(cs, "UTF-8") == 0 || strcmp(cs, "utf8") == 0);
}

void term_get_size(int *cols, int *rows) {
    struct winsize ws;
    *cols = *rows = 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

/* ------------------------------------------------------------------ */
/* Raw mode                                                           */
/* ------------------------------------------------------------------ */

static struct termios g_saved;
static int g_raw;

static void write_all(const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

int term_enter(void) {
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return -1;

    struct termios t = g_saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    t.c_cc[VSUSP] = _POSIX_VDISABLE;          /* Ctrl+Z would leave the terminal in raw mode */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return -1;

    g_raw = 1;
    atexit(term_leave);
    /* alternate screen, hide cursor, no auto-wrap */
    const char seq[] = "\033[?1049h\033[?25l\033[?7l\033[0m\033[2J";
    write_all(seq, sizeof seq - 1);
    return 0;
}

void term_leave(void) {
    if (!g_raw) return;
    g_raw = 0;
    const char seq[] = "\033[0m\033[?7h\033[?25h\033[?1049l";
    write_all(seq, sizeof seq - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
}

/* ------------------------------------------------------------------ */
/* UTF-8 and cell widths                                              */
/* ------------------------------------------------------------------ */

/* Returns bytes consumed (0 at the terminating NUL) */
static int utf8_decode(const unsigned char *s, uint32_t *cp) {
    if (s[0] == 0) { *cp = 0; return 0; }
    if (s[0] < 0x80) { *cp = s[0]; return 1; }

    int n;
    uint32_t c;
    if      ((s[0] & 0xE0) == 0xC0) { n = 2; c = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { n = 3; c = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { n = 4; c = s[0] & 0x07; }
    else { *cp = 0xFFFD; return 1; }

    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c;
    return n;
}

static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 0 = don't draw (control / combining), 1 or 2 cells otherwise */
static int cp_width(uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (cp < 0x7F) return 1;
    int w = wcwidth((wchar_t)cp);
    if (w < 0) return 1;                        /* unknown to the C locale: assume 1 */
    return w > 2 ? 2 : w;
}

int text_width(const char *str) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)str;
    uint32_t cp;
    int n;
    while ((n = utf8_decode(p, &cp)) > 0) {
        p += n;
        w += cp_width(cp);
    }
    return w;
}

void fit_text(const char *str, int w, const char *ell, char *out, size_t outn) {
    if (outn == 0) return;
    if (w < 0) w = 0;

    if (text_width(str) <= w) {
        snprintf(out, outn, "%s", str);
        return;
    }

    int ellw = text_width(ell);
    int budget = (w >= ellw) ? w - ellw : w;
    int useEll = (w >= ellw);

    size_t o = 0;
    int used = 0;
    const unsigned char *p = (const unsigned char *)str;
    uint32_t cp;
    int n;
    while ((n = utf8_decode(p, &cp)) > 0) {
        int cw = cp_width(cp);
        if (used + cw > budget) break;
        if (o + (size_t)n + 1 >= outn) break;
        memcpy(out + o, p, (size_t)n);
        o += (size_t)n;
        used += cw;
        p += n;
    }
    out[o] = '\0';
    if (useEll && o + strlen(ell) + 1 <= outn) strcat(out, ell);
}

/* ------------------------------------------------------------------ */
/* Screen buffer                                                      */
/* ------------------------------------------------------------------ */

void scr_init(Screen *s, int w, int h, ColorMode mode, int unicode, int sgrOk) {
    pal_init();
    memset(s, 0, sizeof *s);
    s->color = mode;
    s->unicode = unicode;
    s->sgr = sgrOk;
    G = unicode ? &G_UNICODE : &G_ASCII;
    scr_resize(s, w, h);
}

void scr_resize(Screen *s, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    free(s->cells);
    free(s->prev);
    s->w = w;
    s->h = h;
    s->cells = calloc((size_t)w * (size_t)h, sizeof(Cell));
    s->prev  = calloc((size_t)w * (size_t)h, sizeof(Cell));
    if (!s->cells || !s->prev) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    s->prevValid = 0;
    scr_clear(s);
}

void scr_free(Screen *s) {
    free(s->cells);
    free(s->prev);
    s->cells = s->prev = NULL;
}

void scr_clear(Screen *s) {
    for (int i = 0; i < s->w * s->h; i++) {
        s->cells[i].ch = ' ';
        s->cells[i].fg = C_DEF;
        s->cells[i].bg = C_DEF;
        s->cells[i].attr = 0;
    }
}

static void set_cell(Screen *s, int x, int y, uint32_t ch, Style st) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    Cell *c = &s->cells[y * s->w + x];
    c->ch = ch;
    c->fg = st.fg;
    c->bg = st.bg;
    c->attr = st.attr;
}

void scr_putc(Screen *s, int x, int y, uint32_t ch, Style st) {
    if (!s->unicode && ch > 0x7E) ch = '?';
    set_cell(s, x, y, ch, st);
}

int scr_puts(Screen *s, int x, int y, int maxw, const char *str, Style st) {
    if (y < 0 || y >= s->h) return 0;

    int drawn = 0;
    const unsigned char *p = (const unsigned char *)str;
    uint32_t cp;
    int n;
    while (drawn < maxw && (n = utf8_decode(p, &cp)) > 0) {
        p += n;
        if (!s->unicode && cp > 0x7E) cp = '?';
        int w = cp_width(cp);
        if (w == 0) continue;
        if (drawn + w > maxw) break;

        set_cell(s, x + drawn, y, cp, st);
        if (w == 2) set_cell(s, x + drawn + 1, y, 0, st);   /* continuation cell */
        drawn += w;
    }
    return drawn;
}

int scr_printf(Screen *s, int x, int y, int maxw, Style st, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return scr_puts(s, x, y, maxw, buf, st);
}

int scr_puts_right(Screen *s, int xEnd, int y, int maxw, const char *str, Style st) {
    int w = text_width(str);
    if (w > maxw) w = maxw;
    return scr_puts(s, xEnd - w, y, w, str, st);
}

void scr_fill(Screen *s, Rect r, uint32_t ch, Style st) {
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            set_cell(s, x, y, ch, st);
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

static char *ob;
static size_t ocap, olen;

static void oreserve(size_t n) {
    if (olen + n <= ocap) return;
    ocap = (olen + n) * 2;
    ob = realloc(ob, ocap);
    if (!ob) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
}

static void oputs(const char *str) {
    size_t n = strlen(str);
    oreserve(n);
    memcpy(ob + olen, str, n);
    olen += n;
}

static void oprintf(const char *fmt, ...) {
    char tmp[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    oputs(tmp);
}

static int cell_eq(const Cell *a, const Cell *b) {
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

static void emit_sgr(const Screen *s, uint8_t fg, uint8_t bg, uint8_t attr) {
    if (!s->sgr) return;

    unsigned a = attr;
    const PalEntry *pf = &pal[fg];
    if (s->color != COLOR_256) a |= pf->attrLow;
    if (s->color == COLOR_8)   a |= pf->attr8;

    oputs("\033[0");
    if (a & ATTR_BOLD) oputs(";1");
    if (a & ATTR_DIM)  oputs(";2");
    if (a & ATTR_UL)   oputs(";4");
    if (a & ATTR_REV)  oputs(";7");

    if (s->color == COLOR_256) {
        if (fg != C_DEF && pf->c256 >= 0) oprintf(";38;5;%d", pf->c256);
        if (bg != C_DEF && pal[bg].c256 >= 0) oprintf(";48;5;%d", pal[bg].c256);
    } else if (s->color == COLOR_8) {
        if (fg != C_DEF && pf->c8 >= 0) oprintf(";%d", 30 + pf->c8);
        if (bg != C_DEF) oputs(";7");         /* no background colours: use reverse video */
    }
    oputs("m");
}

void scr_flush(Screen *s) {
    olen = 0;
    int cx = -1, cy = -1;
    int cf = -1, cb = -1, ca = -1;

    if (!s->prevValid) oputs("\033[0m\033[2J");

    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            const Cell *c = &s->cells[y * s->w + x];
            const Cell *p = &s->prev[y * s->w + x];
            if (s->prevValid && cell_eq(c, p)) continue;
            if (c->ch == 0) continue;                        /* second half of a wide char */

            if (cy != y || cx != x) {
                oprintf("\033[%d;%dH", y + 1, x + 1);
                cy = y;
                cx = x;
            }
            if (c->fg != cf || c->bg != cb || c->attr != ca) {
                emit_sgr(s, c->fg, c->bg, c->attr);
                cf = c->fg;
                cb = c->bg;
                ca = c->attr;
            }

            char u[4];
            int n = utf8_encode(c->ch, u);
            oreserve((size_t)n);
            memcpy(ob + olen, u, (size_t)n);
            olen += (size_t)n;

            int w = cp_width(c->ch);
            cx += (w > 0) ? w : 1;
        }
    }

    if (olen > 0) {
        oputs("\033[0m");
        write_all(ob, olen);
    }
    memcpy(s->prev, s->cells, sizeof(Cell) * (size_t)s->w * (size_t)s->h);
    s->prevValid = 1;
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                           */
/* ------------------------------------------------------------------ */

static unsigned char inbuf[128];
static int inlen;

int term_fill_input(void) {
    if (inlen >= (int)sizeof inbuf) return 0;
    ssize_t n = read(STDIN_FILENO, inbuf + inlen, sizeof inbuf - (size_t)inlen);
    if (n > 0) {
        inlen += (int)n;
        return 1;
    }
    if (n == 0) return -1;
    return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
}

static void consume(int n) {
    if (n >= inlen) { inlen = 0; return; }
    memmove(inbuf, inbuf + n, (size_t)(inlen - n));
    inlen -= n;
}

/* Waits briefly for the rest of an escape sequence */
static int wait_more(int ms) {
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&p, 1, ms) > 0) return term_fill_input() > 0;
    return 0;
}

int term_parse_key(Key *k) {
    k->code = K_NONE;
    k->ch = 0;
    k->mod = 0;

    while (inlen > 0) {
        unsigned char c = inbuf[0];

        if (c == 0x1B) {
            if (inlen == 1 && !wait_more(30)) {          /* a lone ESC is the Escape key */
                consume(1);
                k->code = K_ESC;
                return 1;
            }
            if (inlen < 2 || (inbuf[1] != '[' && inbuf[1] != 'O')) {
                consume(1);                              /* Alt+key etc: treat as Escape */
                k->code = K_ESC;
                return 1;
            }

            int i = 2, np = 0, cur = 0, have = 0, mod = 0;
            int params[4] = {0};
            unsigned char fin = 0;
            for (;;) {
                if (i >= inlen) {
                    if (!wait_more(30)) break;           /* truncated sequence: drop it */
                    continue;
                }
                unsigned char d = inbuf[i++];
                if (d >= '0' && d <= '9') { cur = cur * 10 + (d - '0'); have = 1; }
                else if (d == ';')        { if (np < 4) params[np++] = cur; cur = 0; have = 0; mod = 1; }
                else                      { fin = d; break; }
            }
            if (have && np < 4) params[np++] = cur;
            consume(fin ? i : inlen);

            int code = K_NONE;
            switch (fin) {
                case 'A': code = K_UP; break;
                case 'B': code = K_DOWN; break;
                case 'C': code = K_RIGHT; break;
                case 'D': code = K_LEFT; break;
                case 'H': code = K_HOME; break;
                case 'F': code = K_END; break;
                case '~':
                    switch (params[0]) {
                        case 1: case 7: code = K_HOME; break;
                        case 4: case 8: code = K_END; break;
                        case 5: code = K_PGUP; break;
                        case 6: code = K_PGDN; break;
                    }
                    break;
            }
            if (code != K_NONE) {
                k->code = code;
                k->mod = mod;
                return 1;
            }
            continue;                                    /* unknown sequence: ignore */
        }

        consume(1);
        if (c == '\r' || c == '\n') { k->code = K_ENTER; return 1; }
        if (c >= 0x20 && c < 0x7F)  { k->code = K_CHAR; k->ch = c; return 1; }
        /* other control bytes and UTF-8 continuation bytes are ignored */
    }
    return 0;
}