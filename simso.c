/*
 * OPERATING SYSTEM SIMULATION  (ncurses port)
 * Original DOS/Turbo-C + BGI version by Eduard Garcia and Lloren Llado.
 *
 * The scheduling logic (round-robin CPU, Banker's-style deadlock
 * avoidance, memory allocation, I/O queue) is preserved verbatim.
 * Only the presentation/input layer was rewritten:
 *   - Borland BGI graphics  -> ncurses text dashboard
 *   - mouse-driven dialog    -> keyboard prompts
 *   - conio/dos (clrscr, gotoxy, delay, randomize) -> ncurses/std equivalents
 *
 * Build:  gcc -O2 -Wall simso.c -o simso -lncurses
 * Run  :  ./simso      (needs a terminal of at least 80x24)
 */

#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Process Control Block (unchanged)                                 */
/* ------------------------------------------------------------------ */
struct P_C_B
{
    char nombre[2];
    int  tiempoejecucion;
    int  estadoactual;
    int  prioridad;
    int  maximos[3];
    int  asignados[3];
    int  liberados[3];
    int  solicitados[3];
    int  es;
    int  tiempoes[5];   /* was [4]: es can reach 4 and the I/O shift reads
                         * tiempoes[i+1] up to index 4 (originally spilled
                         * into espera_es on DOS). */
    int  espera_es;
    int  memoria;
};

/* ------------------------------------------------------------------ */
/*  Prototypes                                                        */
/* ------------------------------------------------------------------ */
void nombre_procesos(void);
void escojer_cinco(void);
void compactar(int);
void compactar_cola_preparados(void);
void creacion_pcb(int, char);
int  evitacion(int z);
int  solicitud(void);
void round_robin(int z);
void resta_es(void);
int  comprobar_final(void);
void compactar_cola_es(void);
void compactar_recursos(void);
int  comprobar_entrades(void);
int  recursos(void);
int  error_sistema(void);
int  qua(void);
int  es(void);
int  procesos(void);

void imprimir_colas(void);
void imprimir_cpu(int);

/* new presentation helpers */
static void init_ui(void);
static void cleanup_exit(int code);
static int  prompt_int(const char *label, int lo, int hi);
static void show_message_wait(const char *m);
static void render_frame(int pause);
static int  safe_rr(void);

/* ------------------------------------------------------------------ */
/*  Global arrays / state (unchanged; NULL sentinels replaced by 0)   */
/* ------------------------------------------------------------------ */
char cola_espera[12]   = {'A','B','C','D','E','F','G','H','I','J','K','L'};
char cola_es[5]        = {0};
char cola_recursos[5]  = {'z','z','z','z','z'};
struct P_C_B PCB[12];
char cola_preparados[5]= {0};
char memoria[64]       = {0};

int A = 20;
int B = 30;
int C = 25;
int quantum;
int auxes;
int numero;
int nou_proces = 3;

/* index of the PCB currently "in the CPU", for the dashboard */
static int cpu_proc = -1;

/* how long each animated frame lingers, in ms */
#define STEP_DELAY_MS 110

/* ================================================================== */
/*  PRESENTATION LAYER (ncurses)                                      */
/* ================================================================== */

/* BGI colour index 0..7 -> ncurses base colour; bit 3 => bright(bold) */
static const short bgi_ncurses[8] = {
    COLOR_BLACK, COLOR_BLUE,  COLOR_GREEN, COLOR_CYAN,
    COLOR_RED,   COLOR_MAGENTA, COLOR_YELLOW, COLOR_WHITE
};

/*
 * attr_of - translate a 16-colour BGI index into an ncurses attribute.
 *   Low 3 bits pick the colour pair (1..8); bit 3 (the old "bright" flag)
 *   adds A_BOLD. Every coloured draw in render_frame() goes through this.
 */
static int attr_of(int bgi)
{
    int a = COLOR_PAIR((bgi & 7) + 1);
    if (bgi & 8) a |= A_BOLD;
    return a;
}

/*
 * color_of_letter - map a process letter 'A'..'L' to its stable
 *   colour-pair index (1..12), so each process keeps one colour in the
 *   memory bar. Unknown letters fall back to white.
 */
static int color_of_letter(char c)
{
    switch (c) {
        case 'A': return 1;  case 'B': return 2;  case 'C': return 3;
        case 'D': return 4;  case 'E': return 5;  case 'F': return 6;
        case 'G': return 7;  case 'H': return 8;  case 'I': return 9;
        case 'J': return 10; case 'K': return 11; case 'L': return 12;
        default:  return 7;
    }
}

/*
 * init_ui - bring up the ncurses screen: cbreak + noecho input, hidden
 *   cursor, keypad, and the 8 colour pairs built from bgi_ncurses[]. Aborts
 *   with a stderr message if the terminal is smaller than 80x24. Replaces
 *   the BGI graphics-mode init.
 */
static void init_ui(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        for (int i = 0; i < 8; i++)
            init_pair(i + 1, bgi_ncurses[i], -1);
    }

    if (LINES < 24 || COLS < 80) {
        endwin();
        fprintf(stderr,
                "A terminal at least 80x24 is required (current %dx%d).\n",
                COLS, LINES);
        exit(1);
    }
}

/*
 * cleanup_exit - endwin() then exit(code). Used at every exit point so the
 *   terminal is always restored first (the original called raw exit() from
 *   graphics mode).
 */
static void cleanup_exit(int code)
{
    endwin();
    exit(code);
}

/*
 * draw_box - draw a titled rectangle in ACS line-drawing characters at
 *   (y,x) with height h and width w. Replaces the BGI cuadro()/panel()
 *   frames.
 */
static void draw_box(int y, int x, int h, int w, const char *title)
{
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    if (title) {
        attron(A_BOLD);
        mvprintw(y, x + 2, " %s ", title);
        attroff(A_BOLD);
    }
}

/*
 * prompt_int - modal keyboard prompt that reads an integer in [lo,hi],
 *   re-asking on invalid input. Temporarily re-enables echo and the cursor.
 *   Replaces the old mouse + checkbox data-entry dialog.
 */
static int prompt_int(const char *label, int lo, int hi)
{
    int v;
    char buf[32];

    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);

    for (;;) {
        erase();
        attron(attr_of(9) | A_BOLD);
        mvprintw(1, 2, "OPERATING SYSTEM SIMULATION");
        attroff(attr_of(9) | A_BOLD);
        mvprintw(4, 2, "%s", label);
        mvprintw(5, 2, "Range [%d - %d]: ", lo, hi);
        clrtoeol();
        refresh();

        if (getnstr(buf, sizeof(buf) - 1) == ERR)
            continue;
        v = atoi(buf);
        if (v >= lo && v <= hi)
            break;

        mvprintw(7, 2, "Invalid value, press a key...");
        refresh();
        getch();
    }

    noecho();
    curs_set(0);
    return v;
}

/*
 * show_message_wait - print a bold status line (row 23, clipped to 78 cols)
 *   and block until a key is pressed. Drives the two error screens and the
 *   end-of-run message.
 */
static void show_message_wait(const char *m)
{
    nodelay(stdscr, FALSE);
    attron(A_BOLD);
    mvprintw(23, 0, "%.78s", m);
    attroff(A_BOLD);
    clrtoeol();
    refresh();
    getch();
}

/*
 * render_frame - the heart of the presentation layer: redraw the whole
 *   dashboard from global state - memory bar, the four queue boxes, the
 *   A/B/C resource counters, and the running-process PCB panel. When pause!=0
 *   the frame lingers ~STEP_DELAY_MS and 'q' aborts cleanly (mirrors the
 *   old delay(500) inside imprimir_colas). Replaces every BGI draw call.
 */
static void render_frame(int pause)
{
    int i;
    char cell;

    erase();

    /* ---- title ---- */
    attron(attr_of(9) | A_BOLD);
    mvprintw(0, 2, "OPERATING SYSTEM SIMULATION");
    attroff(attr_of(9) | A_BOLD);
    attron(attr_of(9));
    mvprintw(1, 2, "Eduard Garcia and Lloren Llado");
    attroff(attr_of(9));

    /* ---- memory bar (64 units) ---- */
    mvprintw(3, 0, "Memory:");
    for (i = 0; i < 64; i++) {
        cell = memoria[i];
        if (cell >= 'A' && cell <= 'L') {
            int a = attr_of(color_of_letter(cell));
            attron(a);
            mvaddch(3, 10 + i, ACS_CKBOARD);
            attroff(a);
        } else {
            attron(A_DIM);
            mvaddch(3, 10 + i, '.');
            attroff(A_DIM);
        }
    }

    /* ---- available resources ---- */
    mvprintw(4, 0, "Available resources    A:%-4d B:%-4d C:%-4d", A, B, C);

    /* ---- Waiting queue ---- */
    draw_box(6, 0, 3, 38, "Waiting queue");
    {
        int col = 2;
        for (i = 0; i < 12; i++) {
            char c = cola_espera[i];
            if (c >= 'A' && c <= 'L') {
                attron(attr_of(color_of_letter(c)) | A_BOLD);
                mvaddch(7, col, c);
                attroff(attr_of(color_of_letter(c)) | A_BOLD);
            } else {
                attron(A_DIM);
                mvaddch(7, col, '.');
                attroff(A_DIM);
            }
            col += 2;
        }
    }

    /* ---- Ready ---- */
    draw_box(6, 40, 3, 38, "Ready");
    {
        int col = 42;
        for (i = 0; i < 5; i++) {
            char c = cola_preparados[i];
            if (c >= 'A' && c <= 'L') {
                attron(attr_of(1) | A_BOLD);   /* BGI blue */
                mvprintw(7, col, "P%c", c);
                attroff(attr_of(1) | A_BOLD);
            } else {
                attron(A_DIM);
                mvprintw(7, col, "..");
                attroff(A_DIM);
            }
            col += 6;
        }
    }

    /* ---- I/O queue ---- */
    draw_box(10, 0, 3, 38, "I/O queue");
    {
        int col = 2;
        for (i = 0; i < 5; i++) {
            char c = cola_es[i];
            if (c >= 'A' && c <= 'L') {
                attron(attr_of(color_of_letter(c)) | A_BOLD);
                mvaddch(11, col, c);
                attroff(attr_of(color_of_letter(c)) | A_BOLD);
            } else {
                attron(A_DIM);
                mvaddch(11, col, '.');
                attroff(A_DIM);
            }
            col += 3;
        }
    }

    /* ---- Resources (resource wait queue) ---- */
    draw_box(10, 40, 3, 38, "Resources");
    {
        int col = 42;
        for (i = 0; i < 5; i++) {
            char c = cola_recursos[i];
            if (c >= 'A' && c <= 'L') {
                attron(attr_of(color_of_letter(c)) | A_BOLD);
                mvaddch(11, col, c);
                attroff(attr_of(color_of_letter(c)) | A_BOLD);
            } else {
                attron(A_DIM);
                mvaddch(11, col, '.');
                attroff(A_DIM);
            }
            col += 3;
        }
    }

    /* ---- Running process PCB (CPU) ---- */
    draw_box(14, 0, 8, 78, "Running PCB  (CPU)");
    if (cpu_proc >= 0 && cpu_proc < 12) {
        struct P_C_B *p = &PCB[cpu_proc];
        mvprintw(15, 2, "Process: %c%c        State: %d        Priority: %d",
                 p->nombre[0], p->nombre[1], p->estadoactual, p->prioridad);
        mvprintw(16, 2, "Remaining execution time: %d", p->tiempoejecucion);
        mvprintw(17, 2, "Maximum resources:  A:%-3d B:%-3d C:%-3d",
                 p->maximos[0], p->maximos[1], p->maximos[2]);
        mvprintw(18, 2, "Assigned resources: A:%-3d B:%-3d C:%-3d",
                 p->asignados[0], p->asignados[1], p->asignados[2]);
        if (p->es != -1) {
            mvprintw(19, 2, "I/O count: %d", p->es);
            mvprintw(20, 2, "I/O times: %d %d %d %d",
                     p->tiempoes[0], p->tiempoes[1],
                     p->tiempoes[2], p->tiempoes[3]);
        } else {
            mvprintw(19, 2, "I/O count: 0");
            mvprintw(20, 2, "I/O times: -");
        }
        mvprintw(15, 52, "Memory: %d u.", p->memoria);
    } else {
        attron(A_DIM);
        mvprintw(17, 2, "(CPU idle)");
        attroff(A_DIM);
    }

    mvprintw(22, 0, "'q' = abort simulation");

    refresh();

    if (pause)
        napms(STEP_DELAY_MS);

    /* non-blocking abort check */
    nodelay(stdscr, TRUE);
    int ch = getch();
    nodelay(stdscr, FALSE);
    if (ch == 'q' || ch == 'Q')
        cleanup_exit(0);
}

/* rand()%rand() from the original, guarded against divide-by-zero
 * (Borland's RNG never crashed here by luck; glibc will raise SIGFPE
 *  the moment the divisor rand() returns 0). Same distribution. */
/*
 * safe_rr - the original's "rand() % rand()" distribution, but returns 0
 *   instead of dividing by a zero divisor (which would SIGFPE on Linux).
 *   Used wherever the sim picks letters, memory sizes and burst times.
 */
static int safe_rr(void)
{
    int d = rand();
    return d ? (rand() % d) : 0;
}

/* ================================================================== */
/*  MAIN                                                              */
/* ================================================================== */
/*
 * main - program entry point and top-level scheduler loop. Seeds the RNG,
 *   builds the UI, prompts for quantum and I/O time, then the finish mode
 *   (1 = stop after N processes, 2 = run until all finish). Sets up the five
 *   initial processes, then loops: dispatch ready processes through
 *   solicitud/evitacion/round_robin/recursos while any exist, else service
 *   I/O via resta_es(). Ends when comprobar_final() reaches 0, or breaks
 *   early if error_sistema() reports an unresolvable resource deadlock.
 */
int main(void)
{
    int error, z, fi = 1, entrades = 5, cert = 1;
    int syserror = 0;
    int modo_todos = 0;   /* true = "run until all executed" (mode 2).
                           * numero==0 means two different things depending
                           * on mode (countdown-reached-zero vs. no-limit),
                           * so the early-exit check below must not fire
                           * for mode 2. */
    char point = 'y';

    srand((unsigned)time(NULL));
    init_ui();

    /* ---- data-entry dialog (was mouse + checkboxes) ---- */
    quantum = prompt_int("Enter the QUANTUM:", 1, 50);
    auxes   = prompt_int("Enter the I/O TIME:", 1, 50);

    for (;;) {
        erase();
        attron(attr_of(9) | A_BOLD);
        mvprintw(1, 2, "OPERATING SYSTEM SIMULATION");
        attroff(attr_of(9) | A_BOLD);
        mvprintw(4, 2, "Termination mode:");
        mvprintw(6, 4, "1) Finish after N processes");
        mvprintw(7, 4, "2) When all have been executed");
        mvprintw(9, 2, "Choose (1/2): ");
        refresh();

        int c = getch();
        if (c == '1') {
            numero = prompt_int("Number of processes to finish:", 1, 12);
            break;
        }
        if (c == '2') {
            numero = 0;
            modo_todos = 1;
            break;
        }
    }

    /* ---- simulation setup (unchanged) ---- */
    nombre_procesos();
    escojer_cinco();
    compactar(5);
    imprimir_colas();
    creacion_pcb(5, point);

    while (fi != 0) {
        if (entrades > 0) {
            do {
                if (cola_preparados[4] != 'z') {
                    z = 0;
                    z = solicitud();
                    if (z == 12) {
                        show_message_wait(
                            "Error N.1: access to an invalid or uncreated PCB. "
                            "Press a key.");
                        cleanup_exit(1);
                    }
                    cert = evitacion(z);
                } else {
                    z = recursos();
                    cert = 1;
                }
            } while (cert == 0);

            do {
                if (z <= 11) {
                    round_robin(z);
                    if (!modo_todos && numero == 0) {
                        cleanup_exit(0);
                    }
                    z = recursos();
                }
            } while (z != 12);
        } else {
            resta_es();
        }

        fi       = comprobar_final();
        entrades = comprobar_entrades();
        error    = error_sistema();
        if (error == 1) {
            syserror = 1;
            break;
        }
    }

    if (syserror)
        show_message_wait("Error N.2: the system does not have enough "
                          "resources. Press a key.");
    else
        show_message_wait("Simulation finished. Press a key to exit.");

    cleanup_exit(0);
    return 0;
}

/* ================================================================== */
/*  SIMULATION CORE  (logic identical to the original)               */
/* ================================================================== */

/*
 * nombre_procesos - initialise all 12 PCBs with letters A..L, state 1
 *   (declared, not yet loaded), tag nombre[1]='X', and prime each I/O
 *   wait counter with the user's I/O time.
 */
void nombre_procesos(void)
{
    int i;
    for (i = 0; i < 12; i++) {
        switch (i) {
            case 0:  PCB[i].nombre[0]='A'; break;
            case 1:  PCB[i].nombre[0]='B'; break;
            case 2:  PCB[i].nombre[0]='C'; break;
            case 3:  PCB[i].nombre[0]='D'; break;
            case 4:  PCB[i].nombre[0]='E'; break;
            case 5:  PCB[i].nombre[0]='F'; break;
            case 6:  PCB[i].nombre[0]='G'; break;
            case 7:  PCB[i].nombre[0]='H'; break;
            case 8:  PCB[i].nombre[0]='I'; break;
            case 9:  PCB[i].nombre[0]='J'; break;
            case 10: PCB[i].nombre[0]='K'; break;
            case 11: PCB[i].nombre[0]='L'; break;
        }
        PCB[i].nombre[1]  = 'X';
        PCB[i].estadoactual = 1;
        PCB[i].espera_es  = auxes;
    }
}

/*
 * escojer_cinco - randomly pick the five distinct process letters for the
 *   initial ready queue (cola_preparados), re-rolling until no duplicates.
 */
void escojer_cinco(void)
{
    int i = 0, valor = 0, j = 0, aux;

    for (i = 0; i < 5; i++)
        cola_preparados[i] = 65 + (safe_rr() % 12);

    do {
        aux = 0;
        for (i = 0; i < 5; i++) {
            valor = cola_preparados[i];
            for (j = 0; j < 5; j++) {
                if ((valor == cola_preparados[j]) && (i != j)) {
                    cola_preparados[j] = 65 + (safe_rr() % 12);
                    aux = 1;
                }
            }
        }
    } while (aux != 0);
}

/*
 * compactar - keep the waiting queue (cola_espera) dense.
 *   n>1: remove the n admitted processes from the waiting queue and squeeze
 *   out the gaps. n==1: shift the queue down by one (used when a single new
 *   process is admitted later). The param shadows the global name; harmless.
 */
void compactar(int compactar)
{
    int i, j;
    char valor;

    if (compactar > 1) {
        for (i = 0; i < compactar; i++) {
            valor = cola_preparados[i];
            for (j = 0; j < 12; j++) {
                if (valor == cola_espera[j])
                    cola_espera[j] = 0;
            }
        }

        i = 0;
        while (i < 12) {
            j = 11;
            if (cola_espera[i] == 0) {
                while ((cola_espera[j] == 0) && (i < j)) j--;
                if (i < j) {
                    valor = cola_espera[j];
                    cola_espera[j] = cola_espera[i];
                    cola_espera[i] = valor;
                } else {
                    i = 12;
                }
            }
            i++;
        }
    } else {
        for (i = 0; i < 12; i++) {
            cola_espera[i] = cola_espera[i + 1];
            if (cola_espera[i] == 0) break;
        }
    }
}

/*
 * imprimir_colas - render one animated dashboard frame (with the linger +
 *   'q'-to-abort behaviour). Kept as its own name because the core
 *   calls it in dozens of places after every state change.
 */
void imprimir_colas(void)
{
    render_frame(1);
}

/*
 * creacion_pcb - turn ready-queue letters into fully populated processes.
 *   Allocates memory (bailing the process back to the waiting queue if there
 *   isn't enough), then randomises priority, resource maxima, execution
 *   time and sorted I/O burst times, renames nombre[0] to 'P', and sets
 *   state 2 (ready). Handles both the initial batch (procesos>1, driven by
 *   cola_preparados) and later single admissions (procesos==1, via apunta).
 *   Hardened vs the original: free-cell counters reset each pass and the
 *   burst array is fully initialised - no read past memoria[63]/tiempoes[4].
 */
void creacion_pcb(int procesos, char apunta)
{
    int j, i, k = 0, p = 0, valor, memo = 0;
    char aux;

    while (p < procesos) {
        i = 0;
        if (procesos > 1) {
            while (cola_preparados[p] != PCB[i].nombre[0]) i++;
        } else {
            while (PCB[i].nombre[0] != apunta) i++;
        }

        do {
            PCB[i].memoria = safe_rr() % 32;
        } while (PCB[i].memoria == 0);

        k = 0; memo = 0;   /* reset added: do-while would read memoria[64]
                            * when a prior iteration left k==64 */
        do {
            if (memoria[k] == 0) memo++;
            k++;
        } while (k < 64);
        k = 0;

        if (memo >= PCB[i].memoria) {
            memo = PCB[i].memoria;
            while ((k < 64) && (memo != 0)) {   /* was k<=64: writes memoria[64] */
                if (memoria[k] == 0) { memoria[k] = PCB[i].nombre[0]; memo--; }
                k++;
            }

            PCB[i].estadoactual = 2;

            PCB[i].asignados[0] = 0;
            PCB[i].asignados[1] = 0;
            PCB[i].asignados[2] = 0;

            aux = PCB[i].nombre[0];
            PCB[i].nombre[0] = 'P';
            PCB[i].nombre[1] = aux;

            do {
                PCB[i].tiempoejecucion = safe_rr() % 100;
            } while (PCB[i].tiempoejecucion <= 5);

            PCB[i].prioridad = rand() % 255;

            for (j = 0; j < 3; j++) {
                switch (j) {
                    case 0: PCB[i].maximos[j] = rand() % 10; break;
                    case 1: PCB[i].maximos[j] = rand() % 15; break;
                    case 2: PCB[i].maximos[j] = rand() % 12; break;
                }
            }

            PCB[i].es = rand() % 5;

            for (j = 0; j < 5; j++) PCB[i].tiempoes[j] = -1;  /* clear bursts */

            if (PCB[i].es > 0) {
                for (j = 0; j < PCB[i].es; j++)
                    PCB[i].tiempoes[j] = rand() % PCB[i].tiempoejecucion;

                for (j = 0; j < PCB[i].es; j++) {
                    k = 0;
                    valor = PCB[i].tiempoes[j];
                    do {
                        if ((valor == PCB[i].tiempoes[k]) && (j != k))
                            PCB[i].tiempoes[k] = rand() % PCB[i].tiempoejecucion;
                        else
                            k++;
                    } while (k < PCB[i].es);
                }

                for (j = 0; j < PCB[i].es; j++) {
                    for (k = 0; k < PCB[i].es; k++) {
                        if ((PCB[i].tiempoes[j] < PCB[i].tiempoes[k]) && (j != k)) {
                            valor = PCB[i].tiempoes[j];
                            PCB[i].tiempoes[j] = PCB[i].tiempoes[k];
                            PCB[i].tiempoes[k] = valor;
                        }
                    }
                }
            } else {
                PCB[i].es = -1;
                for (j = 0; j < 3; j++)
                    PCB[i].tiempoes[j] = -1;
            }
        } else {
            k = 0;
            while ((cola_espera[k] != 0) && (k < 12)) k++;
            if (procesos > 1) {
                cola_espera[k] = PCB[i].nombre[0];
                cola_preparados[p] = 'z';
            } else {
                cola_espera[k] = apunta;
                j = 0;
                while (j < 5) {
                    if (cola_preparados[j] == apunta)
                        cola_preparados[j] = 'z';
                    j++;
                }
            }
        }
        p++;
    }

    i = 0;
    for (j = 0; j <= 4; j++) {
        if (cola_preparados[j] == 'z') {
            aux = cola_preparados[i];
            cola_preparados[i] = cola_preparados[j];
            cola_preparados[j] = aux;
            i++;
        }
    }
}

/*
 * imprimir_cpu - mark PCB[proces] as the process in the CPU and redraw one
 *   frame (no linger) so the PCB panel shows its live fields.
 */
void imprimir_cpu(int proces)
{
    cpu_proc = proces;
    render_frame(0);
}

/*
 * qua / es / procesos - legacy prompt wrappers (quantum / I/O time / process
 *   count). The main dialog now inlines prompt_int(); these remain only for
 *   source compatibility with the original.
 */
int qua(void)
{
    return prompt_int("Enter the QUANTUM:", 1, 50);
}

int es(void)
{
    return prompt_int("Enter the I/O TIME:", 1, 50);
}

int procesos(void)
{
    return prompt_int("Number of processes to finish:", 1, 12);
}

/*
 * solicitud - request step for the process at the head of the ready queue
 *   (cola_preparados[4]). Randomly releases some held resources back to the
 *   A/B/C pool, then rolls a fresh request (solicitados[]) bounded by each
 *   maximum. Returns the PCB index, or 12 if the head can't be found.
 *   Search is bounded so it never dereferences PCB[12].
 */
int solicitud(void)
{
    int j = 0, selec = 0, x, y, w;

    /* bound must be first operand: && short-circuits left-to-right, so the
     * original (deref && selec<=12) read PCB[12] before the range check. */
    while ((selec < 12) && (cola_preparados[4] != PCB[selec].nombre[1])) selec++;

    if (selec < 12) {
        for (j = 0; j < 3; j++) {
            if (PCB[selec].asignados[j] > 0) {
                switch (j) {
                    case 0: x = rand() % PCB[selec].asignados[j];
                            A += x; PCB[selec].asignados[j] -= x; break;
                    case 1: y = rand() % PCB[selec].asignados[j];
                            B += y; PCB[selec].asignados[j] -= y; break;
                    case 2: w = rand() % PCB[selec].asignados[j];
                            C += w; PCB[selec].asignados[j] -= w; break;
                }
            }
        }

        for (j = 0; j < 3; j++) {
            if (PCB[selec].maximos[j] > 0)
                PCB[selec].solicitados[j] = rand() % PCB[selec].maximos[j];
            else
                PCB[selec].solicitados[j] = 0;
        }
    } else {
        selec = 12;
    }
    return selec;
}

/*
 * evitacion - Banker's-style deadlock avoidance for PCB[selec]'s request.
 *   If it fits within the available A/B/C, resources are deducted and
 *   assigned and it returns 1 (granted). Otherwise it rolls the state back,
 *   moves the process to the resource-wait queue (state 5), drops it from the
 *   ready queue, and returns 0 (blocked).
 */
int evitacion(int selec)
{
    int k = 0, j, disponible = 0, da = 0, db = 0, dc = 0, valor = 1;

    da = A; db = B; dc = C;

    for (j = 0; j < 3; j++) {
        switch (j) {
            case 0: disponible = A; break;
            case 1: disponible = B; break;
            case 2: disponible = C; break;
        }
        if (PCB[selec].solicitados[j] <= PCB[selec].maximos[j]) {
            if (PCB[selec].solicitados[j] <= disponible) {
                switch (j) {
                    case 0: A = disponible - PCB[selec].solicitados[j]; break;
                    case 1: B = disponible - PCB[selec].solicitados[j]; break;
                    case 2: C = disponible - PCB[selec].solicitados[j]; break;
                }
                PCB[selec].asignados[j] += PCB[selec].solicitados[j];
                PCB[selec].maximos[j]   -= PCB[selec].solicitados[j];
                valor = 1;
            } else {
                A = da; B = db; C = dc;
                for (k = j - 1; k >= 0; k--) {
                    PCB[selec].asignados[j] -= PCB[selec].solicitados[j];
                    PCB[selec].maximos[j]   += PCB[selec].solicitados[j];
                }
                k = 0;
                while (cola_recursos[k] != 'z') k++;
                cola_recursos[k] = PCB[selec].nombre[1];
                PCB[selec].estadoactual = 5;
                cola_preparados[4] = 'z';
                compactar_cola_preparados();
                valor = 0; j = 3;
            }
        }
    }
    return valor;
}

/*
 * round_robin - run PCB[selec] for up to one quantum. Rotates it to the head
 *   of the ready queue, then each cycle ticks down its execution time and
 *   every I/O countdown. On exit it dispatches by outcome: move to the I/O
 *   queue (a burst hit 0), retire it and free its resources and memory when
 *   execution finishes (decrementing numero), or re-queue it if the quantum
 *   expired mid-run. Every few rounds (the nou_proces throttle) it also
 *   admits one process from the waiting queue.
 */
void round_robin(int selec)
{
    int i, k = 0, aux = 0, c = 4, entra = 0;
    char proceso;

    if (PCB[selec].nombre[1] == cola_preparados[4]) {
        cola_preparados[4] = 0;
    } else {
        PCB[selec].estadoactual = 2;
        for (i = 4; i != 0; i--) {
            if (cola_preparados[i] == 'z') {
                cola_preparados[i] = cola_preparados[4];
                break;
            }
        }
        cola_preparados[4] = PCB[selec].nombre[1];
        compactar_recursos();
    }

    imprimir_cpu(selec);

    i = quantum;

    while ((i > 0) && (PCB[selec].tiempoejecucion > 0) && (PCB[selec].tiempoes[0] != 0)) {
        i--;
        PCB[selec].tiempoejecucion--;

        if (PCB[selec].es > 0) {
            for (aux = 0; aux < PCB[selec].es; aux++)
                PCB[selec].tiempoes[aux]--;
        } else {
            PCB[selec].es = -1;
        }
        resta_es();
    }

    compactar_cola_preparados();
    nou_proces--;
    if (nou_proces <= 0) {
        for (c = 0; c < 12; c++)
            if (PCB[c].estadoactual > 1) entra++;
        c = 4;
        if ((cola_espera[0] != 0) && (entra < 5)) {
            while (cola_preparados[c] != 'z') c--;
            if (c < 4) {
                cola_preparados[c] = cola_espera[0];
                proceso = cola_espera[0];
                creacion_pcb(1, proceso);
                compactar(1);
            }
        }
        nou_proces = 3;
    }

    if ((PCB[selec].tiempoes[0] == 0) && (PCB[selec].es > 0)) {
        for (c = 0; c < 64; c++)   /* was c<65: reads/writes memoria[64] */
            if (memoria[c] == PCB[selec].nombre[1]) memoria[c] = 0;

        while ((cola_es[k] != 0) && (k != 4)) k++;
        if ((cola_es[k] == 0) && (k < 5)) {
            PCB[selec].estadoactual = 3;
            cola_es[k] = PCB[selec].nombre[1];
        }
    } else {
        if (PCB[selec].tiempoejecucion <= 0) {
            numero--;
            A += PCB[selec].asignados[0];
            B += PCB[selec].asignados[1];
            C += PCB[selec].asignados[2];
            PCB[selec].estadoactual = 0;
            for (c = 0; c < 64; c++)   /* was c<65: reads/writes memoria[64] */
                if (memoria[c] == PCB[selec].nombre[1]) memoria[c] = 0;
            for (c = 0; c < 12; c++)
                if (PCB[c].estadoactual > 1) entra++;
            c = 4;
            if ((cola_espera[0] != 0) && (entra < 5)) {
                while (cola_preparados[c] != 'z') c--;
                if (c < 4) {
                    cola_preparados[c] = cola_espera[0];
                    proceso = cola_espera[0];
                    creacion_pcb(1, proceso);
                    compactar(1);
                }
            }
        } else {
            if (i <= 0) {
                aux = 4;
                while (cola_preparados[aux] != 'z') aux--;
                cola_preparados[aux] = PCB[selec].nombre[1];
            }
        }
    }
    imprimir_colas();
}

/*
 * compactar_cola_preparados - shift the ready queue up by one and put a
 *   'z' (empty marker) at the front, then redraw. Called after a process
 *   leaves the head slot.
 */
void compactar_cola_preparados(void)
{
    int i;
    for (i = 3; i >= 0; i--)
        cola_preparados[i + 1] = cola_preparados[i];
    cola_preparados[0] = 'z';
    imprimir_colas();
}

/*
 * compactar_cola_es - drop the just-serviced entry from the I/O queue by
 *   shifting the remaining entries down, then redraw.
 */
void compactar_cola_es(void)
{
    int i = 0;
    while ((cola_es[i + 1] != 0) && (i < 4)) {
        cola_es[i] = cola_es[i + 1];
        i++;
    }
    cola_es[i] = 0;
    imprimir_colas();
}

/*
 * resta_es - advance every process in the I/O queue by one tick. When a
 *   process finishes its I/O wait it is re-loaded into memory, its burst list
 *   is shifted down, and it returns to the ready queue (state 2). main() runs
 *   this branch whenever the ready queue is empty. Free-cell counting resets
 *   each pass and is bounded to memoria[0..63].
 */
void resta_es(void)
{
    int aux = 0, auxdos = 0, auxtres, i, memo = 0, k = 0;

    if (cola_es[0] != 0) {
        while ((cola_es[aux] != 0) && (aux < 4)) {
            auxdos = 0;
            while (PCB[auxdos].nombre[1] != cola_es[aux]) auxdos++;
            PCB[auxdos].espera_es--;
            if (PCB[auxdos].espera_es <= 0) {
                k = 0; memo = 0;   /* reset added: same do-while / memoria[64]
                                    * hazard as in creacion_pcb */
                do {
                    if (memoria[k] == 0) memo++;
                    k++;
                } while (k < 64);

                if (memo >= PCB[auxdos].memoria) {
                    k = 0; memo = PCB[auxdos].memoria;
                    while ((k < 64) && (memo != 0)) {   /* was k<65: writes memoria[64] */
                        if (memoria[k] == 0) { memoria[k] = PCB[auxdos].nombre[1]; memo--; }
                        k++;
                    }
                    for (i = 0; i < PCB[auxdos].es; i++) {
                        if (PCB[auxdos].es > 1)
                            PCB[auxdos].tiempoes[i] = PCB[auxdos].tiempoes[i + 1];
                        else
                            PCB[auxdos].tiempoes[0] = -1;
                    }
                    PCB[auxdos].es--;
                    PCB[auxdos].espera_es = auxes;
                    auxtres = 4;
                    while (cola_preparados[auxtres] != 'z') auxtres--;
                    cola_preparados[auxtres] = cola_es[aux];
                    PCB[auxdos].estadoactual = 2;
                    compactar_cola_es();
                }
            }
            aux++;
        }
    }
}

/*
 * comprobar_final - count PCBs still alive (state != 0). Zero means every
 *   process has finished; it is the outer loop's termination test.
 */
int comprobar_final(void)
{
    int i, valor = 0;
    for (i = 0; i < 12; i++)
        if (PCB[i].estadoactual != 0) valor++;
    return valor;
}

/*
 * comprobar_entrades - count PCBs in the ready state (2). Zero tells main()
 *   to service I/O (resta_es) instead of dispatching a process.
 */
int comprobar_entrades(void)
{
    int i, contador = 0;
    for (i = 0; i < 12; i++)
        if (PCB[i].estadoactual == 2) contador++;
    return contador;
}

/*
 * recursos - retry granting resources to the process at the head of the
 *   resource-wait queue (cola_recursos[0]), using the same Banker's check as
 *   evitacion(). Returns the PCB index on success, or 12 when nothing can be
 *   granted (which unwinds the dispatch loop in main). Bounded + guarded so
 *   it never touches PCB[12].
 */
int recursos(void)
{
    int j = 0, disponible = 0, selec = 0, valor, da, db, dc, k = 0;

    da = A; db = B; dc = C;

    /* bound first: see note in solicitud() about left-to-right short-circuit */
    while ((selec < 12) && (cola_recursos[0] != PCB[selec].nombre[1])) selec++;

    /* guard added: original dereferenced PCB[12] when no match was found
     * (solicitud() has the same search but already guards with selec<12).
     * Short-circuit falls through to the existing valor=12 path. */
    if (selec < 12 && cola_recursos[0] == PCB[selec].nombre[1]) {
        for (j = 0; j < 3; j++) {
            switch (j) {
                case 0: disponible = A; break;
                case 1: disponible = B; break;
                case 2: disponible = C; break;
            }
            if (PCB[selec].solicitados[j] <= PCB[selec].maximos[j]) {
                if (PCB[selec].solicitados[j] <= disponible) {
                    switch (j) {
                        case 0: A = disponible - PCB[selec].solicitados[j]; break;
                        case 1: B = disponible - PCB[selec].solicitados[j]; break;
                        case 2: C = disponible - PCB[selec].solicitados[j]; break;
                    }
                    PCB[selec].asignados[j] += PCB[selec].solicitados[j];
                    PCB[selec].maximos[j]   -= PCB[selec].solicitados[j];
                    valor = selec;
                } else {
                    A = da; B = db; C = dc;
                    for (k = j - 1; k >= 0; k--) {
                        PCB[selec].asignados[j] -= PCB[selec].solicitados[j];
                        PCB[selec].maximos[j]   += PCB[selec].solicitados[j];
                    }
                    valor = 12; j = 3;
                }
            }
        }
    } else {
        valor = 12;
    }
    return valor;
}

/*
 * compactar_recursos - shift the resource-wait queue up by one, mark the
 *   tail empty ('z'), and redraw. Called once the head process is finally
 *   granted its resources.
 */
void compactar_recursos(void)
{
    int i;
    for (i = 0; i < 4; i++)
        cola_recursos[i] = cola_recursos[i + 1];
    cola_recursos[4] = 'z';
    imprimir_colas();
}

/*
 * error_sistema - deadlock detector. Returns 1 only when nothing can make
 *   progress: no ready processes, no I/O pending, no waiting processes, yet
 *   the resource-wait queue is non-empty. That triggers the Error N.2
 *   (not enough resources) screen.
 */
int error_sistema(void)
{
    int i, contador = 0, error;
    for (i = 0; i < 5; i++)
        if (cola_preparados[i] != 'z') contador++;

    if ((cola_es[0] == 0) && (contador == 0) &&
        (cola_espera[0] == 0) && (cola_recursos[0] != 'z'))
        error = 1;
    else
        error = 0;

    return error;
}
