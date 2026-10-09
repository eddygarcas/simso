/*
 * ui.c - the ncurses dashboard.
 *
 * Runs on the main thread and owns the screen outright; it only ever reads
 * a private copy of the engine state taken under the lock in shared_take().
 * Nothing here calls into sim.c, so the renderer can be rewritten, or
 * dropped entirely (see bench.c), without touching the scheduler.
 *
 * The layout is computed per frame from LINES/COLS, so resizing works and
 * panels degrade gracefully: on an 80x24 terminal the CPU timeline is
 * dropped and the tables shrink; given room they come back.
 */
#include "ui.h"

#include <langinfo.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- glyphs -------------------------------------------------------- */

/* ncurses spells its attributes as unsigned chtype; everything in this
 * file passes attributes around as plain int, so alias the few we use. */
#define A_NONE ((int)A_NORMAL)
#define A_HI   ((int)A_BOLD)
#define A_LO   ((int)A_DIM)

/*
 * Unicode block glyphs are multi-byte, and only the wide-character ncurses
 * (ncursesw) accounts for their column width correctly -- a narrow build
 * renders each one as several garbled columns. build-filc.sh probes for the
 * library actually available and defines SIMSO_FORCE_ASCII when it is the
 * narrow one, so the fallback is chosen at build time rather than producing
 * a broken display at run time.
 */
#ifdef SIMSO_FORCE_ASCII
static int  g_ascii = 1;
#else
static int  g_ascii = 0;
#endif
static const char *G_FULL, *G_SHADE, *G_DOT, *G_ARROW, *G_PAUSE;

static void pick_glyphs(void)
{
    if (g_ascii) {
        G_FULL = "#"; G_SHADE = "-"; G_DOT = "."; G_ARROW = ">"; G_PAUSE = "||";
    } else {
        G_FULL = "█";   /* full block   */
        G_SHADE = "░";  /* light shade  */
        G_DOT = "·";    /* middle dot   */
        G_ARROW = "▶";  /* play         */
        G_PAUSE = "‖";  /* double bar   */
    }
}

void ui_set_ascii(int on) { g_ascii = on; pick_glyphs(); }

/* ---- colour pairs --------------------------------------------------- */

#define PAIR_PROC(i)  (1 + (i))   /* 1..12 */
#define PAIR_FRAME    20
#define PAIR_TITLE    21
#define PAIR_DIM      22
#define PAIR_GOOD     23
#define PAIR_WARN     24
#define PAIR_BAD      25
#define PAIR_HEAD     26
#define PAIR_ACCENT   27

/* twelve hues that stay distinguishable next to each other */
static const short pal256[SIM_NPROC] = {
    39, 208, 41, 201, 226, 51, 196, 141, 118, 215, 81, 170
};
static const short pal8[SIM_NPROC] = {
    COLOR_BLUE, COLOR_YELLOW, COLOR_GREEN, COLOR_MAGENTA,
    COLOR_YELLOW, COLOR_CYAN, COLOR_RED, COLOR_MAGENTA,
    COLOR_GREEN, COLOR_RED, COLOR_BLUE, COLOR_CYAN
};
static int pal8_bold[SIM_NPROC] = { 0,0,0,0,1,0,0,1,1,1,1,1 };

static int g_color = 0;

static int proc_attr(char letter)
{
    int i = letter - 'A';
    if (i < 0 || i >= SIM_NPROC) return A_NONE;
    if (!g_color) return (i & 1) ? A_HI : A_NONE;
    int a = (int)COLOR_PAIR(PAIR_PROC(i));
    if (COLORS < 256 && pal8_bold[i]) a |= A_BOLD;
    return a;
}

static int pair_attr(int p) { return g_color ? (int)COLOR_PAIR(p) : A_NONE; }

/* ---- primitives ----------------------------------------------------- */

static void panel(int y, int x, int h, int w, const char *title)
{
    if (h < 2 || w < 4) return;
    int fa = pair_attr(PAIR_FRAME) | A_DIM;

    attron(fa);
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    attroff(fa);

    if (title && w > (int)strlen(title) + 6) {
        int a = pair_attr(PAIR_TITLE) | A_BOLD;
        attron(a);
        mvprintw(y, x + 2, " %s ", title);
        attroff(a);
    }
}

/* proportional bar: `w` cells, `frac` filled, drawn in `attr` */
static void hbar(int y, int x, int w, double frac, int attr)
{
    if (w <= 0) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int on = (int)(frac * w + 0.5);

    for (int i = 0; i < w; i++) {
        if (i < on) {
            attron(attr);
            mvaddstr(y, x + i, G_FULL);
            attroff(attr);
        } else {
            attron(pair_attr(PAIR_DIM) | A_DIM);
            mvaddstr(y, x + i, G_SHADE);
            attroff(pair_attr(PAIR_DIM) | A_DIM);
        }
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- lifecycle ------------------------------------------------------ */

int ui_start(void)
{
    setlocale(LC_ALL, "");
    const char *cs = nl_langinfo(CODESET);
    if (!cs || (strcmp(cs, "UTF-8") != 0 && strcmp(cs, "utf8") != 0))
        g_ascii = 1;
    pick_glyphs();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        g_color = 1;
        for (int i = 0; i < SIM_NPROC; i++)
            init_pair(PAIR_PROC(i), COLORS >= 256 ? pal256[i] : pal8[i], -1);
        init_pair(PAIR_FRAME,  COLOR_WHITE,   -1);
        init_pair(PAIR_TITLE,  COLOR_CYAN,    -1);
        init_pair(PAIR_DIM,    COLOR_WHITE,   -1);
        init_pair(PAIR_GOOD,   COLOR_GREEN,   -1);
        init_pair(PAIR_WARN,   COLOR_YELLOW,  -1);
        init_pair(PAIR_BAD,    COLOR_RED,     -1);
        init_pair(PAIR_HEAD,   COLOR_BLACK,   COLOR_CYAN);
        init_pair(PAIR_ACCENT, COLOR_MAGENTA, -1);
    }

    if (LINES < 24 || COLS < 80) {
        endwin();
        fprintf(stderr, "simso needs a terminal of at least 80x24 "
                        "(this one is %dx%d).\n", COLS, LINES);
        return -1;
    }
    return 0;
}

void ui_stop(void)
{
    if (!isendwin()) endwin();
}

/* ---- modal prompts -------------------------------------------------- */

static void prompt_chrome(const char *title)
{
    erase();
    int a = pair_attr(PAIR_HEAD) | A_BOLD;
    attron(a);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 2, " SIMSO  -  operating system scheduler simulator ");
    attroff(a);

    attron(pair_attr(PAIR_TITLE) | A_BOLD);
    mvprintw(3, 4, "%s", title);
    attroff(pair_attr(PAIR_TITLE) | A_BOLD);
}

int ui_prompt_int(const char *label, const char *help, int lo, int hi)
{
    char buf[32];
    int v;

    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);

    for (;;) {
        prompt_chrome(label);
        attron(pair_attr(PAIR_DIM) | A_DIM);
        mvprintw(5, 4, "%s", help);
        attroff(pair_attr(PAIR_DIM) | A_DIM);
        mvprintw(7, 4, "value [%d-%d]: ", lo, hi);
        clrtoeol();
        refresh();

        if (getnstr(buf, sizeof buf - 1) == ERR) continue;
        v = atoi(buf);
        if (v >= lo && v <= hi) break;

        attron(pair_attr(PAIR_BAD));
        mvprintw(9, 4, "out of range - press any key");
        attroff(pair_attr(PAIR_BAD));
        refresh();
        getch();
    }

    noecho();
    curs_set(0);
    return v;
}

int ui_prompt_choice(const char *title, const char *a, const char *b)
{
    nodelay(stdscr, FALSE);
    for (;;) {
        prompt_chrome(title);
        mvprintw(6, 6, "1)  %s", a);
        mvprintw(7, 6, "2)  %s", b);
        attron(pair_attr(PAIR_DIM) | A_DIM);
        mvprintw(9, 4, "press 1 or 2");
        attroff(pair_attr(PAIR_DIM) | A_DIM);
        refresh();

        int c = getch();
        if (c == '1') return 1;
        if (c == '2') return 2;
    }
}

/* ---- panels --------------------------------------------------------- */

static void draw_header(const sim_t *s, const shared_t *sh, int paused,
                        int speed_ms, int finished)
{
    int a = pair_attr(PAIR_HEAD) | A_BOLD;
    (void)sh;

    attron(a);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 1, " SIMSO ");
    attroff(a);

    mvprintw(0, 10, "tick %-9llu", (unsigned long long)s->st.tick);

    int x = 26;

    if (finished) {
        attron(pair_attr(PAIR_WARN) | A_BOLD);
        mvprintw(0, x, "[ finished ]");
        attroff(pair_attr(PAIR_WARN) | A_BOLD);
    } else if (paused) {
        attron(pair_attr(PAIR_WARN) | A_BOLD);
        mvprintw(0, x, "%s paused  ", G_PAUSE);
        attroff(pair_attr(PAIR_WARN) | A_BOLD);
    } else {
        attron(pair_attr(PAIR_GOOD) | A_BOLD);
        mvprintw(0, x, "%s running ", G_ARROW);
        attroff(pair_attr(PAIR_GOOD) | A_BOLD);
    }

    x = 40;
    if (speed_ms <= 0) mvprintw(0, x, "speed max ");
    else               mvprintw(0, x, "speed %3dms", speed_ms);

    if (COLS >= 92)
        mvprintw(0, 54, "done %-3u   ctx %-5u   io %-4u",
                 s->st.completions, s->st.switches, s->st.io_completions);
    else if (COLS >= 70)
        mvprintw(0, 54, "done %-3u ctx %-5u", s->st.completions, s->st.switches);

    if (COLS >= 100) {
        if (s->cpu >= 0) {
            mvprintw(0, COLS - 14, "cpu ");
            attron(proc_attr(s->pcb[s->cpu].name) | A_BOLD);
            printw("%c", s->pcb[s->cpu].name);
            attroff(proc_attr(s->pcb[s->cpu].name) | A_BOLD);
        } else {
            attron(A_DIM);
            mvprintw(0, COLS - 14, "cpu idle");
            attroff(A_DIM);
        }
    }
}

static void draw_memory(const sim_t *s, int y, int x, int h, int w)
{
    panel(y, x, h, w, "Memory");

    int inner = w - 4;
    int cells = SIM_MEM_CELLS < inner ? SIM_MEM_CELLS : inner;
    int used = sim_mem_used(s);

    for (int i = 0; i < cells; i++) {
        char c = s->mem[i];
        if (c >= 'A' && c <= 'L') {
            int a = proc_attr(c);
            attron(a);
            mvaddstr(y + 1, x + 2 + i, G_FULL);
            attroff(a);
        } else {
            attron(pair_attr(PAIR_DIM) | A_DIM);
            mvaddstr(y + 1, x + 2 + i, G_SHADE);
            attroff(pair_attr(PAIR_DIM) | A_DIM);
        }
    }

    if (h >= 4) {
        double frac = (double)used / SIM_MEM_CELLS;
        int a = frac > 0.9 ? pair_attr(PAIR_BAD)
              : frac > 0.7 ? pair_attr(PAIR_WARN) : pair_attr(PAIR_GOOD);
        attron(a);
        mvprintw(y + 2, x + 2, "%d/%d", used, SIM_MEM_CELLS);
        attroff(a);
        attron(A_DIM);
        printw("  %2.0f%% used   %d free regions",
               frac * 100.0, sim_mem_fragments(s));
        attroff(A_DIM);
    }
}

static void draw_resources(const sim_t *s, int y, int x, int h, int w)
{
    panel(y, x, h, w, "Resources");
    static const char *names = "ABC";

    /* cap the bar: at full width a 64-cell bar for a 20-unit pool reads as
     * noise, and on a narrow terminal it pushed the figures off the edge */
    int barw = w - 16;
    if (barw > 24) barw = 24;
    if (barw < 4)  barw = 4;

    for (int j = 0; j < SIM_NRES && j + 1 < h - 1; j++) {
        int used = s->total[j] - s->avail[j];
        double frac = s->total[j] ? (double)used / s->total[j] : 0.0;
        int a = frac > 0.85 ? pair_attr(PAIR_BAD)
              : frac > 0.6  ? pair_attr(PAIR_WARN) : pair_attr(PAIR_GOOD);

        mvprintw(y + 1 + j, x + 2, "%c", names[j]);
        hbar(y + 1 + j, x + 4, barw, frac, a);
        attron(A_DIM);
        mvprintw(y + 1 + j, x + 5 + barw, "%2d/%-2d", s->avail[j], s->total[j]);
        attroff(A_DIM);
    }
}

static void draw_processes(const sim_t *s, int y, int x, int h, int w)
{
    panel(y, x, h, w, "Processes");

    int rows = h - 3;              /* header row + two borders */
    if (rows < 1) return;

    int wide = w >= 68;

    attron(pair_attr(PAIR_DIM) | A_DIM);
    if (wide)
        mvprintw(y + 1, x + 2, "P  STATE  PRI  REMAINING          MEM   A   B   C  IO  CPU");
    else
        mvprintw(y + 1, x + 2, "P  STATE  PRI  REMAINING      MEM  IO");
    attroff(pair_attr(PAIR_DIM) | A_DIM);

    int r = 0;
    for (int i = 0; i < SIM_NPROC && r < rows; i++) {
        const pcb_t *p = &s->pcb[i];
        if (p->state == PS_TERMINATED && p->cpu_ticks == 0) continue;

        /* A PS_NEW process has no PCB filled in yet, so exec_total is 0 and
         * a "remaining" bar would be meaningless (and reads as 100% done).
         * Show it as queued instead. */
        int fresh = (p->state == PS_NEW);

        int row = y + 2 + r++;
        int running = (s->cpu == i);
        int base = p->state == PS_TERMINATED ? (pair_attr(PAIR_DIM) | A_LO)
                                             : A_NONE;

        attron(proc_attr(p->name) | A_BOLD);
        mvprintw(row, x + 2, "%c", p->name);
        attroff(proc_attr(p->name) | A_BOLD);

        int sa = running                      ? (pair_attr(PAIR_GOOD) | A_HI)
               : p->state == PS_BLOCKED_RES   ? pair_attr(PAIR_BAD)
               : p->state == PS_BLOCKED_IO    ? pair_attr(PAIR_WARN)
               : p->state == PS_TERMINATED    ? (pair_attr(PAIR_DIM) | A_LO)
               : A_NONE;
        attron(sa);
        mvprintw(row, x + 5, "%-6s", running ? "RUN" : sim_state_name(p->state));
        attroff(sa);

        attron(base);
        if (fresh) mvprintw(row, x + 12, "  -");
        else       mvprintw(row, x + 12, "%3d", p->priority);
        attroff(base);

        int barw = wide ? 10 : 9;
        double frac = p->exec_total
                    ? (double)(p->exec_total - p->exec_left) / p->exec_total
                    : 0.0;

        if (fresh) {
            attron(pair_attr(PAIR_DIM) | A_LO);
            mvprintw(row, x + 17, "%-*s", barw + 5, "  not admitted");
            attroff(pair_attr(PAIR_DIM) | A_LO);
        } else {
            hbar(row, x + 17, barw, frac,
                 p->state == PS_TERMINATED ? (pair_attr(PAIR_DIM) | A_LO)
                                           : proc_attr(p->name));
            attron(base);
            mvprintw(row, x + 18 + barw, "%3.0f%%", frac * 100.0);
            attroff(base);
        }

        attron(base);

        if (fresh) {
            attroff(base);
            continue;
        }
        if (wide) {
            mvprintw(row, x + 35, "%4d", p->mem_need);
            mvprintw(row, x + 40, "%3d %3d %3d",
                     p->alloc[0], p->alloc[1], p->alloc[2]);
            mvprintw(row, x + 53, "%3d", p->io_count > 0 ? p->io_count : 0);
            mvprintw(row, x + 57, "%5d", p->cpu_ticks);
        } else {
            mvprintw(row, x + 31, "%4d", p->mem_need);
            mvprintw(row, x + 36, "%3d", p->io_count > 0 ? p->io_count : 0);
        }
        attroff(base);
    }
}

static void draw_timeline(const sim_t *s, int y, int x, int h, int w)
{
    panel(y, x, h, w, "CPU timeline");
    (void)h;

    int inner = w - 4;
    if (inner < 4) return;

    uint32_t n = s->gantt_count;
    uint32_t start = n > (uint32_t)inner ? n - (uint32_t)inner : 0;

    for (uint32_t i = start; i < n; i++) {
        char c = sim_gantt_at(s, i);
        int col = x + 2 + (int)(i - start);
        if (c >= 'A' && c <= 'L') {
            attron(proc_attr(c));
            mvaddstr(y + 1, col, G_FULL);
            attroff(proc_attr(c));
        } else {
            attron(pair_attr(PAIR_DIM) | A_DIM);
            mvaddstr(y + 1, col, G_DOT);
            attroff(pair_attr(PAIR_DIM) | A_DIM);
        }
    }
}

static void draw_slot_row(int row, int x, int labelw, const char *label,
                          const char *q, int n, char empty, int stride)
{
    attron(A_DIM);
    mvprintw(row, x, "%-*s", labelw, label);
    attroff(A_DIM);

    for (int i = 0; i < n; i++) {
        int col = x + labelw + i * stride;
        char c = q[i];
        if (c >= 'A' && c <= 'L') {
            attron(proc_attr(c) | A_BOLD);
            mvprintw(row, col, "%c", c);
            attroff(proc_attr(c) | A_BOLD);
        } else {
            (void)empty;
            attron(A_DIM);
            mvprintw(row, col, ".");
            attroff(A_DIM);
        }
    }
}

static void draw_queues(const sim_t *s, int y, int x, int h, int w)
{
    panel(y, x, h, w, "Queues");
    if (h < 6) return;

    int lw = 9;
    draw_slot_row(y + 1, x + 2, lw, "ready",   s->ready_q, SIM_READY_SLOTS, SLOT_FREE, 2);
    draw_slot_row(y + 2, x + 2, lw, "i/o",     s->io_q,    SIM_IO_SLOTS,    SLOT_NONE, 2);
    draw_slot_row(y + 3, x + 2, lw, "res-wait",s->res_q,   SIM_RES_SLOTS,   SLOT_FREE, 2);

    int waitn = SIM_NPROC;
    int avail = (w - 4 - lw) / 2;
    if (waitn > avail) waitn = avail;
    draw_slot_row(y + 4, x + 2, lw, "waiting", s->wait_q, waitn, SLOT_NONE, 2);

    /* the ready queue dispatches from its highest slot - say so */
    if (w >= 34) {
        attron(A_DIM);
        mvprintw(y + 1, x + 2 + lw + SIM_READY_SLOTS * 2 + 1, "<- head");
        attroff(A_DIM);
    }
}

static void draw_log(const sim_t *s, int y, int x, int h, int w)
{
    panel(y, x, h, w, "Events");

    int rows = h - 2;
    if (rows < 1) return;

    uint32_t n = s->ev_count;
    uint32_t start = n > (uint32_t)rows ? n - (uint32_t)rows : 0;

    for (uint32_t i = start; i < n; i++) {
        const sim_event *e = sim_event_at(s, i);
        if (!e) continue;
        int row = y + 1 + (int)(i - start);

        attron(A_DIM);
        mvprintw(row, x + 2, "%6u", e->tick);
        attroff(A_DIM);

        attron(proc_attr(e->proc) | A_BOLD);
        mvprintw(row, x + 9, "%c", e->proc >= 'A' && e->proc <= 'L' ? e->proc : '-');
        attroff(proc_attr(e->proc) | A_BOLD);

        int a = e->kind == EV_DEADLOCK ? (pair_attr(PAIR_BAD) | A_HI)
              : e->kind == EV_RES_BLOCK || e->kind == EV_NOMEM ||
                e->kind == EV_UNSAFE   ? pair_attr(PAIR_WARN)
              : e->kind == EV_EXIT     ? pair_attr(PAIR_GOOD)
              : A_NONE;
        attron(a);
        mvprintw(row, x + 11, "%.*s", w - 14, sim_event_verb(e->kind));
        attroff(a);
    }
}

static void draw_footer(int finished)
{
    int y = LINES - 1;
    attron(pair_attr(PAIR_DIM) | A_DIM);
    mvhline(y, 0, ' ', COLS);
    if (finished)
        mvprintw(y, 1, "simulation finished   -   q / enter : exit");
    else
        mvprintw(y, 1, "space pause   n step   +/- speed   0 max   1 slow   q quit");
    attroff(pair_attr(PAIR_DIM) | A_DIM);
}

/* ---- frame ---------------------------------------------------------- */

static void draw_frame(const sim_t *s, shared_t *sh, int paused, int speed_ms,
                       int finished)
{
    erase();
    draw_header(s, sh, paused, speed_ms, finished);

    int top = 1, bottom = LINES - 1;
    int wide = COLS >= 100;

    /* memory + resources band */
    int band_h = 5;
    int res_w = wide ? 30 : 0;
    draw_memory(s, top, 0, band_h, COLS - res_w);
    if (wide) draw_resources(s, top, COLS - res_w, band_h, res_w);
    top += band_h;

    if (!wide) {                      /* narrow: resources get their own band */
        draw_resources(s, top, 0, 5, COLS);
        top += 5;
    }

    int left = bottom - top;          /* rows still unallocated */

    /* reserve the bottom band for queues + events */
    int bottom_h = clampi(left - 6, 6, 12);
    int show_timeline = (left - bottom_h) >= 9;
    int tl_h = show_timeline ? 3 : 0;

    int proc_h = left - bottom_h - tl_h;
    if (proc_h > 15) { bottom_h += proc_h - 15; proc_h = 15; }
    if (proc_h >= 5) {
        draw_processes(s, top, 0, proc_h, COLS);
        top += proc_h;
    }

    if (show_timeline) {
        draw_timeline(s, top, 0, tl_h, COLS);
        top += tl_h;
    }

    int rest = bottom - top;
    if (rest >= 6) {
        if (COLS >= 104) {
            int qw = 44;
            draw_queues(s, top, 0, rest, qw);
            draw_log(s, top, qw, rest, COLS - qw);
        } else {
            int qh = 6;
            draw_queues(s, top, 0, qh, COLS);
            if (rest - qh >= 3)
                draw_log(s, top + qh, 0, rest - qh, COLS);
        }
    }

    draw_footer(finished);
    refresh();
}

/* ---- summary -------------------------------------------------------- */

static void draw_summary(const sim_t *s, sim_outcome o)
{
    erase();
    int a = pair_attr(PAIR_HEAD) | A_BOLD;
    attron(a);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 2, " SIMSO  -  run complete ");
    attroff(a);

    int y = 2;
    attron(pair_attr(o == SIM_DEADLOCK || o == SIM_BAD_PCB ? PAIR_BAD : PAIR_GOOD)
           | A_BOLD);
    mvprintw(y, 4, "%s", sim_outcome_name(o));
    attroff(pair_attr(o == SIM_DEADLOCK || o == SIM_BAD_PCB ? PAIR_BAD : PAIR_GOOD)
            | A_BOLD);
    y += 2;

    double mem = s->st.mem_samples
               ? (double)s->st.mem_used_sum / s->st.mem_samples : 0.0;
    double util = s->st.tick ? (double)s->st.cpu_busy / s->st.tick : 0.0;

    mvprintw(y++, 4, "ticks elapsed        %llu", (unsigned long long)s->st.tick);
    mvprintw(y++, 4, "cpu utilisation      %.1f%%", util * 100.0);
    mvprintw(y++, 4, "processes completed  %u", s->st.completions);
    mvprintw(y++, 4, "context switches     %u", s->st.switches);
    mvprintw(y++, 4, "i/o completions      %u", s->st.io_completions);
    mvprintw(y++, 4, "resource blocks      %u  (granted later: %u)",
             s->st.res_blocks, s->st.res_grants);
    mvprintw(y++, 4, "admissions           %u  (deferred, no memory: %u)",
             s->st.admissions, s->st.nomem_bounces);
    if (s->use_banker)
        mvprintw(y++, 4, "unsafe-state refusals %u", s->st.unsafe_rejects);
    mvprintw(y++, 4, "mean memory in use   %.1f / %d cells (%.0f%%)",
             mem, SIM_MEM_CELLS, mem / SIM_MEM_CELLS * 100.0);
    mvprintw(y++, 4, "peak ready depth     %u", s->st.ready_depth_max);

    y++;
    attron(pair_attr(PAIR_DIM) | A_DIM);
    mvprintw(y++, 4, "P    cpu   switches   i/o   res-blocks   swaps");
    attroff(pair_attr(PAIR_DIM) | A_DIM);

    for (int i = 0; i < SIM_NPROC && y < LINES - 2; i++) {
        const pcb_t *p = &s->pcb[i];
        if (p->cpu_ticks == 0 && p->state == PS_NEW) continue;
        attron(proc_attr(p->name) | A_BOLD);
        mvprintw(y, 4, "%c", p->name);
        attroff(proc_attr(p->name) | A_BOLD);
        mvprintw(y, 6, "%6d %9d %6d %12d %7d",
                 p->cpu_ticks, p->switches, p->io_done, p->res_blocks,
                 p->swapped);
        y++;
    }

    draw_footer(1);
    refresh();
}

/* ---- main loop ------------------------------------------------------ */

void ui_loop(shared_t *sh)
{
    sim_t view;
    uint64_t seq = 0;

    nodelay(stdscr, FALSE);
    timeout(16);                 /* ~60 fps and the input tick in one call */

    for (;;) {
        pthread_mutex_lock(&sh->mu);
        int paused   = sh->paused;
        int speed    = sh->speed_ms;
        int finished = sh->finished;
        sim_outcome o = sh->outcome;
        view = sh->snap;
        seq  = sh->seq;
        pthread_mutex_unlock(&sh->mu);
        (void)seq;

        if (finished) {
            draw_summary(&view, o);
            timeout(-1);
            for (;;) {
                int c = getch();
                if (c == 'q' || c == 'Q' || c == '\n' || c == KEY_ENTER) return;
            }
        }

        draw_frame(&view, sh, paused, speed, 0);

        int c = getch();
        switch (c) {
            case 'q': case 'Q':
                shared_quit(sh);
                return;
            case ' ':
                shared_toggle_pause(sh);
                break;
            case 'n': case 'N': case KEY_RIGHT:
                shared_step(sh);
                break;
            case '+': case '=':
                shared_nudge_speed(sh, -20);
                break;
            case '-': case '_':
                shared_nudge_speed(sh, +20);
                break;
            case '0':
                shared_set_speed(sh, 0);
                break;
            case '1':
                shared_set_speed(sh, 200);
                break;
            default:
                break;
        }
    }
}
