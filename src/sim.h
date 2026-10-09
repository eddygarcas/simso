/*
 * sim.h - the scheduler core, as a self-contained reentrant value type.
 *
 * Nothing in sim.h/sim.c knows that ncurses exists. The engine never prints,
 * never sleeps and never reads the keyboard; when it reaches a point where
 * the original called imprimir_colas() it invokes the observer callback and
 * lets the caller decide what that means (draw a frame, or nothing at all).
 *
 * That single change is what makes the rest possible:
 *   - the UI can run on its own thread at its own frame rate (ui.c)
 *   - N engines can run in parallel with no shared state (bench.c)
 *   - a (seed, params) pair replays exactly (rng.h)
 */
#ifndef SIMSO_SIM_H
#define SIMSO_SIM_H

#include <stdint.h>
#include <stddef.h>
#include "rng.h"

#define SIM_NPROC        12
#define SIM_READY_SLOTS   5
#define SIM_IO_SLOTS      5
#define SIM_RES_SLOTS     5
#define SIM_MEM_CELLS    64
#define SIM_NRES          3

/*
 * Three distinct "nothing here" markers, inherited verbatim from the DOS
 * original. They are NOT interchangeable and the control flow depends on
 * telling them apart:
 *
 *   SLOT_NONE ('\0') - empty in wait_q and io_q; also parked briefly in
 *                      ready_q[4] by run_quantum() to mean "this slot's
 *                      process is on the CPU right now".
 *   SLOT_FREE ('z')  - empty in ready_q and res_q. The downward scans
 *                      (`while (ready_q[c] != SLOT_FREE) c--`) look for this
 *                      one specifically and will walk straight past a
 *                      SLOT_NONE, which is load-bearing behaviour.
 *
 * Normalising them to one value changes which slot those scans land on, so
 * they are preserved exactly.
 */
#define SLOT_NONE '\0'
#define SLOT_FREE 'z'

#define SIM_EVENT_RING  192   /* scrollback for the UI event log   */
#define SIM_GANTT_RING  768   /* one cell per CPU tick, for the timeline */

/* Process states. 4 (RUNNING) is deliberately absent: the original never
 * stored it, and `cpu` below carries that information instead, so the
 * state-counting predicates keep their original arithmetic. */
typedef enum {
    PS_TERMINATED  = 0,
    PS_NEW         = 1,   /* declared, no PCB filled in, not in memory */
    PS_READY       = 2,
    PS_BLOCKED_IO  = 3,
    PS_BLOCKED_RES = 5
} proc_state;

typedef struct {
    char name;                 /* 'A'..'L', stable for the life of the run */
    int  state;                /* proc_state */
    int  exec_left;
    int  exec_total;
    int  priority;
    int  max[SIM_NRES];        /* still-claimable maximum */
    int  alloc[SIM_NRES];      /* currently held */
    int  req[SIM_NRES];        /* this round's request */
    int  io_count;             /* pending I/O bursts, -1 = none ever */
    int  io_at[SIM_IO_SLOTS];  /* countdown to each burst, sorted ascending */
    int  io_wait;              /* ticks left of the current I/O service */
    int  mem_need;             /* cells owned while resident */

    /* per-process accounting, for the dashboard and the benchmark */
    int  cpu_ticks;
    int  switches;
    int  io_done;
    int  res_blocks;
    int  swapped;
} pcb_t;

typedef enum {
    EV_ADMIT, EV_DISPATCH, EV_PREEMPT, EV_IO_BLOCK, EV_IO_DONE,
    EV_RES_BLOCK, EV_RES_GRANT, EV_EXIT, EV_NOMEM, EV_DEADLOCK, EV_UNSAFE
} ev_kind;

typedef struct {
    uint32_t tick;
    uint8_t  kind;
    char     proc;
    int16_t  aux;
} sim_event;

typedef enum {
    SIM_RUNNING = 0,
    SIM_DONE_ALL,      /* every process terminated */
    SIM_DONE_COUNT,    /* mode 1: the requested N completed */
    SIM_DEADLOCK,      /* error_sistema(): nothing can progress */
    SIM_STALLED,       /* safety valve: no progress for a long time */
    SIM_BAD_PCB,       /* original "Error N.1" */
    SIM_ABORTED        /* observer asked us to stop */
} sim_outcome;

typedef struct {
    uint64_t tick;             /* scheduler steps elapsed */
    uint64_t cpu_busy;         /* ticks with a process on the CPU */
    uint32_t switches;
    uint32_t admissions;
    uint32_t io_completions;
    uint32_t res_blocks;
    uint32_t res_grants;
    uint32_t completions;
    uint32_t nomem_bounces;
    uint32_t unsafe_rejects;   /* only non-zero with --banker */
    uint32_t ready_depth_max;
    uint32_t repairs;          /* lost-process invariant repairs; must stay 0 */
    uint64_t deadlock_tick;
    uint64_t mem_used_sum;     /* /mem_samples = mean occupancy */
    uint32_t mem_samples;
} sim_stats;

typedef struct sim_s sim_t;

/*
 * Called wherever the original called imprimir_colas(). The engine makes no
 * assumption about what happens inside; returning non-zero asks it to stop,
 * which surfaces as SIM_ABORTED.
 */
typedef int (*sim_observer)(sim_t *s, void *user);

struct sim_s {
    pcb_t pcb[SIM_NPROC];
    char  wait_q[SIM_NPROC];          /* SLOT_NONE-terminated, FIFO     */
    char  ready_q[SIM_READY_SLOTS];   /* head is index 4; SLOT_FREE pad */
    char  io_q[SIM_IO_SLOTS];         /* SLOT_NONE pad                  */
    char  res_q[SIM_RES_SLOTS];       /* head is index 0; SLOT_FREE pad */
    char  mem[SIM_MEM_CELLS];         /* owning process letter, or 0    */

    int   avail[SIM_NRES];
    int   total[SIM_NRES];

    int   quantum;
    int   io_time;
    int   target;          /* completions still required in count mode */
    int   run_all;         /* mode 2 */
    int   use_banker;      /* real safety-sequence check vs. plain fit */
    int   admit_throttle;  /* the original's nou_proces counter */
    int   cpu;             /* PCB index on the CPU, -1 when idle */

    rng_t       rng;
    sim_stats   st;
    sim_outcome outcome;

    sim_event ev[SIM_EVENT_RING];
    uint32_t  ev_head, ev_count;
    char      gantt[SIM_GANTT_RING];
    uint32_t  gantt_head, gantt_count;

    sim_observer observe;
    void        *observer_user;
};

void sim_init(sim_t *s, int quantum, int io_time, int target, int run_all,
              int use_banker, uint64_t seed);
void sim_run(sim_t *s);

int         sim_mem_used(const sim_t *s);
int         sim_mem_fragments(const sim_t *s);
int         sim_find(const sim_t *s, char letter);
const char *sim_outcome_name(sim_outcome o);
const char *sim_state_name(int state);
const char *sim_event_verb(int kind);

/* ring accessor: i in [0, ev_count), 0 = oldest retained event */
const sim_event *sim_event_at(const sim_t *s, uint32_t i);
char             sim_gantt_at(const sim_t *s, uint32_t i);

#endif /* SIMSO_SIM_H */
