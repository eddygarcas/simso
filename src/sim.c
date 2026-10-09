/*
 * sim.c - scheduler core. No ncurses, no printf, no sleep, no globals.
 *
 * The control flow is a faithful port of the DOS original; what changed is
 * that every piece of state now hangs off a sim_t* instead of a global, and
 * the drawing calls became observer callbacks. Deliberate behaviour changes
 * are marked CHANGE: and listed in README.md.
 */
#include "sim.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Event ring + timeline                                             */
/* ------------------------------------------------------------------ */

static void ev_push(sim_t *s, int kind, char proc, int aux)
{
    sim_event *e = &s->ev[(s->ev_head + s->ev_count) % SIM_EVENT_RING];
    e->tick = (uint32_t)s->st.tick;
    e->kind = (uint8_t)kind;
    e->proc = proc;
    e->aux  = (int16_t)aux;

    if (s->ev_count < SIM_EVENT_RING)
        s->ev_count++;
    else
        s->ev_head = (s->ev_head + 1) % SIM_EVENT_RING;
}

const sim_event *sim_event_at(const sim_t *s, uint32_t i)
{
    if (i >= s->ev_count) return NULL;
    return &s->ev[(s->ev_head + i) % SIM_EVENT_RING];
}

static void gantt_push(sim_t *s, char c)
{
    s->gantt[(s->gantt_head + s->gantt_count) % SIM_GANTT_RING] = c;
    if (s->gantt_count < SIM_GANTT_RING)
        s->gantt_count++;
    else
        s->gantt_head = (s->gantt_head + 1) % SIM_GANTT_RING;
}

char sim_gantt_at(const sim_t *s, uint32_t i)
{
    if (i >= s->gantt_count) return ' ';
    return s->gantt[(s->gantt_head + i) % SIM_GANTT_RING];
}

/* ------------------------------------------------------------------ */
/*  Observer                                                          */
/* ------------------------------------------------------------------ */

/* Returns 1 if the caller asked us to stop. Every loop that can spin for a
 * while checks this, so an abort is honoured promptly. */
static int publish(sim_t *s)
{
    if (s->outcome == SIM_ABORTED) return 1;
    s->st.mem_used_sum += (uint64_t)sim_mem_used(s);
    s->st.mem_samples++;
    if (s->observe && s->observe(s, s->observer_user)) {
        s->outcome = SIM_ABORTED;
        return 1;
    }
    return 0;
}

static int could_grant(sim_t *s, int i);
static int count_resident(const sim_t *s);
static int ready_free_slot_down(const sim_t *s, int from);

/* ------------------------------------------------------------------ */
/*  Small helpers                                                     */
/* ------------------------------------------------------------------ */

int sim_find(const sim_t *s, char letter)
{
    for (int i = 0; i < SIM_NPROC; i++)
        if (s->pcb[i].name == letter) return i;
    return -1;
}

/* The original searched PCB[].nombre[1], which held 'X' until the PCB was
 * filled in -- so a not-yet-created process was invisible to these scans and
 * produced "Error N.1". Matching on name + "already created" keeps that. */
static int find_live(const sim_t *s, char letter)
{
    int i = sim_find(s, letter);
    if (i < 0 || s->pcb[i].state == PS_NEW) return -1;
    return i;
}

int sim_mem_used(const sim_t *s)
{
    int n = 0;
    for (int i = 0; i < SIM_MEM_CELLS; i++)
        if (s->mem[i]) n++;
    return n;
}

int sim_mem_fragments(const sim_t *s)
{
    int runs = 0;
    for (int i = 0; i < SIM_MEM_CELLS; i++)
        if (!s->mem[i] && (i == 0 || s->mem[i - 1]))
            runs++;
    return runs;
}

static int mem_free_cells(const sim_t *s)
{
    return SIM_MEM_CELLS - sim_mem_used(s);
}

/* First-fit scatter: the original filled any free cell, not a contiguous
 * run, so a process can own a fragmented set of cells. Preserved. */
static void mem_claim(sim_t *s, char letter, int cells)
{
    for (int k = 0; k < SIM_MEM_CELLS && cells > 0; k++)
        if (!s->mem[k]) { s->mem[k] = letter; cells--; }
}

static void mem_release(sim_t *s, char letter)
{
    for (int k = 0; k < SIM_MEM_CELLS; k++)
        if (s->mem[k] == letter) s->mem[k] = 0;
}

/* Lowest free slot in the waiting queue, or -1 when full. */
static int wait_free_slot(const sim_t *s)
{
    for (int k = 0; k < SIM_NPROC; k++)
        if (s->wait_q[k] == SLOT_NONE) return k;
    return -1;
}

/* Highest-indexed SLOT_FREE at or below `from`, scanning down.
 * CHANGE: bounded at 0. The original ran off the bottom of the array when
 * the invariant it assumed did not hold. */
static int ready_free_slot_down(const sim_t *s, int from)
{
    for (int c = from; c >= 0; c--)
        if (s->ready_q[c] == SLOT_FREE) return c;
    return -1;
}

static int count_state(const sim_t *s, int state)
{
    int n = 0;
    for (int i = 0; i < SIM_NPROC; i++)
        if (s->pcb[i].state == state) n++;
    return n;
}

static int count_alive(const sim_t *s)            /* comprobar_final */
{
    int n = 0;
    for (int i = 0; i < SIM_NPROC; i++)
        if (s->pcb[i].state != PS_TERMINATED) n++;
    return n;
}

static int count_resident(const sim_t *s)         /* the original's `entra` */
{
    int n = 0;
    for (int i = 0; i < SIM_NPROC; i++)
        if (s->pcb[i].state > PS_NEW) n++;
    return n;
}

/* The dispatcher only ever looks at the last slot. ready_q[4] can hold a
 * letter, SLOT_FREE, or the SLOT_NONE that run_quantum() parks there, so
 * "is there a process to dispatch" is a letter test, not a != SLOT_FREE
 * test -- the latter would send SLOT_NONE into roll_request() and surface
 * as a bogus "invalid PCB" error. */
static int ready_head_live(const sim_t *s)
{
    char c = s->ready_q[SIM_READY_SLOTS - 1];
    return c >= 'A' && c <= 'L';
}

static int ready_has_any(const sim_t *s)
{
    for (int i = 0; i < SIM_READY_SLOTS; i++)
        if (s->ready_q[i] >= 'A' && s->ready_q[i] <= 'L') return 1;
    return 0;
}

static int ready_depth(const sim_t *s)
{
    int n = 0;
    for (int i = 0; i < SIM_READY_SLOTS; i++)
        if (s->ready_q[i] >= 'A' && s->ready_q[i] <= 'L') n++;
    return n;
}

/*
 * Can anything in the system still move?
 *
 * CHANGE: this replaces error_sistema(), which asked a much narrower
 * question -- "is every queue empty except the resource queue?" -- and so
 * only recognised deadlock in its most degenerate shape. The interesting
 * case it misses is a circular wait that runs through *memory* rather than
 * through A/B/C alone:
 *
 *     P1, P2 have finished their I/O but cannot be swapped back in,
 *            because there is not enough free memory;
 *     P3, P4 hold that memory but are blocked on resources,
 *            which are held by P1 and P2, which cannot run to release them.
 *
 * Every queue is non-empty there, so error_sistema() reports no error and
 * the simulation idles forever. Enumerating the ways forward instead of
 * pattern-matching on empty queues catches both shapes. The old condition
 * is a strict subset of this one.
 */
static int no_progress_possible(sim_t *s)
{
    for (int k = 0; k < SIM_READY_SLOTS; k++)
        if (s->ready_q[k] >= 'A' && s->ready_q[k] <= 'L')
            return 0;                       /* something is dispatchable */

    int freemem = SIM_MEM_CELLS - sim_mem_used(s);
    int slot    = ready_free_slot_down(s, SIM_READY_SLOTS - 1);

    for (int i = 0; i < SIM_NPROC; i++) {
        const pcb_t *p = &s->pcb[i];
        if (p->state != PS_BLOCKED_IO) continue;
        if (p->io_wait > 0) return 0;       /* a burst is still draining */
        if (slot >= 0 && freemem >= p->mem_need)
            return 0;                       /* can be swapped back in */
    }

    for (int i = 0; i < SIM_NPROC; i++)
        if (s->pcb[i].state == PS_BLOCKED_RES && could_grant(s, i))
            return 0;                       /* a request can be satisfied */

    /* a waiting process could still be admitted (its size is rolled at
     * admission, so any free cell at all leaves the door open) */
    if (s->wait_q[0] != SLOT_NONE && slot >= 0 && freemem >= 1 &&
        count_resident(s) < SIM_READY_SLOTS)
        return 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Queue compaction                                                  */
/* ------------------------------------------------------------------ */

static int compact_ready(sim_t *s)        /* compactar_cola_preparados */
{
    for (int i = 3; i >= 0; i--)
        s->ready_q[i + 1] = s->ready_q[i];
    s->ready_q[0] = SLOT_FREE;
    return publish(s);
}

/*
 * Remove io_q[slot] and close the gap.
 *
 * CHANGE: removes the entry that actually completed. compactar_cola_es()
 * always dropped the head no matter which slot had finished its service --
 * correct only while bursts complete in strict arrival order, and silently
 * wrong (losing one process, servicing another twice) when they do not.
 */
static int io_remove(sim_t *s, int slot)
{
    for (int i = slot; i < SIM_IO_SLOTS - 1; i++)
        s->io_q[i] = s->io_q[i + 1];
    s->io_q[SIM_IO_SLOTS - 1] = SLOT_NONE;
    return publish(s);
}

/* Remove one named process from the resource-wait queue and close the gap.
 * compactar_recursos() always dropped slot 0; now that the dispatcher can
 * grant a request from anywhere in the queue, the entry that was actually
 * satisfied is the one that has to leave. */
static int res_remove(sim_t *s, char letter)
{
    int k = 0;
    while (k < SIM_RES_SLOTS && s->res_q[k] != letter) k++;
    if (k == SIM_RES_SLOTS) return publish(s);

    for (int i = k; i < SIM_RES_SLOTS - 1; i++)
        s->res_q[i] = s->res_q[i + 1];
    s->res_q[SIM_RES_SLOTS - 1] = SLOT_FREE;
    return publish(s);
}

/*
 * Drop `letters` from the waiting queue and close the gaps.
 *
 * CHANGE: stable. The original closed gaps by swapping in entries from the
 * tail, which silently reordered the queue -- so the "waiting" queue was not
 * actually FIFO and admission order depended on how many gaps had opened.
 * Order-preserving compaction is what the rest of the code assumes
 * (wait_q[0] is always treated as "next in line").
 */
static void wait_remove_admitted(sim_t *s, const char *letters, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < SIM_NPROC; j++)
            if (s->wait_q[j] == letters[i]) s->wait_q[j] = SLOT_NONE;

    char tmp[SIM_NPROC];
    int w = 0;
    for (int j = 0; j < SIM_NPROC; j++)
        if (s->wait_q[j] != SLOT_NONE) tmp[w++] = s->wait_q[j];
    for (int j = 0; j < w; j++) s->wait_q[j] = tmp[j];
    for (int j = w; j < SIM_NPROC; j++) s->wait_q[j] = SLOT_NONE;
}

static void wait_pop_head(sim_t *s)       /* compactar(1) */
{
    for (int i = 0; i < SIM_NPROC - 1; i++) {
        s->wait_q[i] = s->wait_q[i + 1];
        if (s->wait_q[i] == SLOT_NONE) return;
    }
    s->wait_q[SIM_NPROC - 1] = SLOT_NONE;
}

/* ------------------------------------------------------------------ */
/*  Process creation                                                  */
/* ------------------------------------------------------------------ */

static void fill_pcb(sim_t *s, int i)
{
    pcb_t *p = &s->pcb[i];

    p->state = PS_READY;
    p->alloc[0] = p->alloc[1] = p->alloc[2] = 0;
    p->req[0] = p->req[1] = p->req[2] = 0;

    p->exec_total = p->exec_left = rng_range(&s->rng, 6, 99);
    p->priority   = rng_int(&s->rng, 255);

    p->max[0] = rng_int(&s->rng, 10);
    p->max[1] = rng_int(&s->rng, 15);
    p->max[2] = rng_int(&s->rng, 12);

    for (int j = 0; j < SIM_IO_SLOTS; j++) p->io_at[j] = -1;

    p->io_count = rng_int(&s->rng, SIM_IO_SLOTS);
    if (p->io_count > 0) {
        /* distinct burst offsets, then sorted ascending so io_at[0] is
         * always the next one due. The original rolled-and-retried; a
         * partial selection sort over distinct draws is the same result
         * without the unbounded retry loop. */
        for (int j = 0; j < p->io_count; j++) {
            int v, dup;
            int guard = 0;
            do {
                v = rng_int(&s->rng, p->exec_total);
                dup = 0;
                for (int k = 0; k < j; k++)
                    if (p->io_at[k] == v) { dup = 1; break; }
            } while (dup && ++guard < 64);
            p->io_at[j] = v;
        }
        for (int j = 0; j < p->io_count; j++)
            for (int k = j + 1; k < p->io_count; k++)
                if (p->io_at[k] < p->io_at[j]) {
                    int t = p->io_at[j]; p->io_at[j] = p->io_at[k]; p->io_at[k] = t;
                }
    } else {
        p->io_count = -1;
    }
}

/*
 * Admit `letter`: size it, try to place it in memory, fill in the PCB.
 * Returns 1 on success; 0 means "not enough memory", and the caller is
 * responsible for parking the letter back on the waiting queue.
 */
static int admit(sim_t *s, char letter)
{
    int i = sim_find(s, letter);
    if (i < 0) return 0;

    pcb_t *p = &s->pcb[i];
    p->mem_need = rng_range(&s->rng, 1, 31);

    if (mem_free_cells(s) < p->mem_need) {
        s->st.nomem_bounces++;
        ev_push(s, EV_NOMEM, letter, p->mem_need);
        return 0;
    }

    mem_claim(s, letter, p->mem_need);
    fill_pcb(s, i);
    s->st.admissions++;
    ev_push(s, EV_ADMIT, letter, p->mem_need);
    return 1;
}

/* The initial batch: whatever is already sitting in ready_q. */
static void admit_initial_batch(sim_t *s)
{
    for (int slot = 0; slot < SIM_READY_SLOTS; slot++) {
        char letter = s->ready_q[slot];
        if (letter < 'A' || letter > 'L') continue;

        if (!admit(s, letter)) {
            int k = wait_free_slot(s);
            if (k >= 0) s->wait_q[k] = letter;
            s->ready_q[slot] = SLOT_FREE;
        }
    }

    /* push the SLOT_FREE holes to the front, keeping live entries at the
     * high end where the dispatcher expects to find them */
    int w = 0;
    for (int j = 0; j < SIM_READY_SLOTS; j++)
        if (s->ready_q[j] == SLOT_FREE) {
            char t = s->ready_q[w]; s->ready_q[w] = s->ready_q[j]; s->ready_q[j] = t;
            w++;
        }
}

/*
 * The "let one more process in" step. Admits wait_q[0] if a ready slot and
 * enough memory are available.
 *
 * allow_head controls whether the dispatch slot (index 4) may be filled.
 * run_quantum() passes 0, matching the original's `if (c < 4)` guard: there,
 * slot 4 belongs to the process being dispatched. The idle path passes 1,
 * because when the ready queue is completely empty slot 4 is just another
 * free slot, and refusing it would leave the queue permanently empty.
 */
static void try_admit_one(sim_t *s, int allow_head)
{
    if (s->wait_q[0] == SLOT_NONE) return;
    if (count_resident(s) >= SIM_READY_SLOTS) return;

    int c = ready_free_slot_down(s, SIM_READY_SLOTS - 1);
    if (c < 0) return;
    if (!allow_head && c >= SIM_READY_SLOTS - 1) return;

    char letter = s->wait_q[0];
    s->ready_q[c] = letter;

    if (admit(s, letter)) {
        wait_pop_head(s);
    } else {
        /* could not place it after all: undo the ready-slot claim and leave
         * it at the head of the waiting queue for the next attempt */
        s->ready_q[c] = SLOT_FREE;
    }
}

/* ------------------------------------------------------------------ */
/*  Resource allocation                                               */
/* ------------------------------------------------------------------ */

/*
 * Classic Banker's safety check, used only with --banker.
 *
 * The original called itself "Banker's-style" but only ever tested
 * request <= available, which is the admission test, not the avoidance
 * algorithm -- it cannot prevent the deadlock it was named after. This is
 * the real thing: pretend the request is granted, then look for an ordering
 * in which every remaining process can finish.
 */
static int is_safe_state(const sim_t *s)
{
    int work[SIM_NRES];
    int done[SIM_NPROC] = {0};

    for (int j = 0; j < SIM_NRES; j++) work[j] = s->avail[j];

    for (int pass = 0; pass < SIM_NPROC; pass++) {
        int progressed = 0;

        for (int i = 0; i < SIM_NPROC; i++) {
            if (done[i] || s->pcb[i].state == PS_TERMINATED ||
                s->pcb[i].state == PS_NEW)
                continue;

            int fits = 1;
            for (int j = 0; j < SIM_NRES; j++)
                if (s->pcb[i].max[j] > work[j]) { fits = 0; break; }

            if (fits) {
                for (int j = 0; j < SIM_NRES; j++)
                    work[j] += s->pcb[i].alloc[j];
                done[i] = 1;
                progressed = 1;
            }
        }
        if (!progressed) break;
    }

    for (int i = 0; i < SIM_NPROC; i++)
        if (!done[i] && s->pcb[i].state != PS_TERMINATED &&
            s->pcb[i].state != PS_NEW)
            return 0;
    return 1;
}

/*
 * Apply pcb[i]'s pending request if it fits. Returns 1 when granted.
 * On refusal the caller-visible state is exactly as it was on entry.
 *
 * CHANGE: the rollback is correct now. The original unwound with
 *     for (k = j-1; k >= 0; k--) { alloc[j] -= req[j]; max[j] += req[j]; }
 * -- indexing by j inside a loop over k, so it repeatedly adjusted the one
 * resource that had NOT been granted, j times, and left the resources that
 * HAD been granted applied. That corrupted alloc[]/max[] on every refusal.
 */
static int grant_request(sim_t *s, int i, int commit)
{
    pcb_t *p = &s->pcb[i];
    int saved_avail[SIM_NRES], saved_alloc[SIM_NRES], saved_max[SIM_NRES];

    for (int j = 0; j < SIM_NRES; j++) {
        saved_avail[j] = s->avail[j];
        saved_alloc[j] = p->alloc[j];
        saved_max[j]   = p->max[j];
    }

    int ok = 1;
    for (int j = 0; j < SIM_NRES; j++) {
        if (p->req[j] > p->max[j]) continue;      /* original skipped these */
        if (p->req[j] > s->avail[j]) { ok = 0; break; }

        s->avail[j] -= p->req[j];
        p->alloc[j] += p->req[j];
        p->max[j]   -= p->req[j];
    }

    if (ok && s->use_banker && !is_safe_state(s)) {
        ok = 0;
        if (commit) {
            s->st.unsafe_rejects++;
            ev_push(s, EV_UNSAFE, p->name, 0);
        }
    }

    if (!ok || !commit) {
        for (int j = 0; j < SIM_NRES; j++) {
            s->avail[j] = saved_avail[j];
            p->alloc[j] = saved_alloc[j];
            p->max[j]   = saved_max[j];
        }
        return ok;
    }

    s->st.res_grants++;
    return 1;
}

/* Would this request succeed right now? Leaves no trace. */
static int could_grant(sim_t *s, int i) { return grant_request(s, i, 0); }

static void block_on_resources(sim_t *s, int i)
{
    int k = 0;
    while (k < SIM_RES_SLOTS && s->res_q[k] != SLOT_FREE) k++;
    if (k < SIM_RES_SLOTS) s->res_q[k] = s->pcb[i].name;

    s->pcb[i].state = PS_BLOCKED_RES;
    s->pcb[i].res_blocks++;
    s->st.res_blocks++;
    ev_push(s, EV_RES_BLOCK, s->pcb[i].name, 0);
}

/*
 * solicitud(): the ready-queue head releases a random slice of what it holds
 * back to the pool, then rolls a fresh request bounded by its maximum.
 * Returns the PCB index, or -1 when the head is not a live process.
 */
static int roll_request(sim_t *s)
{
    int i = find_live(s, s->ready_q[SIM_READY_SLOTS - 1]);
    if (i < 0) return -1;

    pcb_t *p = &s->pcb[i];

    for (int j = 0; j < SIM_NRES; j++)
        if (p->alloc[j] > 0) {
            int give = rng_int(&s->rng, p->alloc[j]);
            s->avail[j] += give;
            p->alloc[j] -= give;
        }

    for (int j = 0; j < SIM_NRES; j++)
        p->req[j] = p->max[j] > 0 ? rng_int(&s->rng, p->max[j]) : 0;

    return i;
}

/*
 * recursos(): retry the resource-wait queue. Returns the PCB index of a
 * process whose request was granted, or -1 when none can be satisfied.
 *
 * CHANGE: scans the whole queue instead of only res_q[0]. The original
 * retried the head alone, so a process whose request could never be met
 * blocked every process behind it indefinitely -- classic head-of-line
 * blocking, and in a system this small it stalled the entire simulation
 * even when the process one slot back was perfectly runnable. Order is
 * still respected: the earliest satisfiable request wins.
 */
static int retry_res_queue(sim_t *s)
{
    for (int k = 0; k < SIM_RES_SLOTS; k++) {
        int i = find_live(s, s->res_q[k]);
        if (i < 0) continue;
        if (!grant_request(s, i, 1)) continue;

        ev_push(s, EV_RES_GRANT, s->pcb[i].name, 0);
        return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  I/O service                                                       */
/* ------------------------------------------------------------------ */

/*
 * resta_es(): advance every process in the I/O queue by one tick. A process
 * whose service completes is swapped back into memory, has its burst list
 * shifted down, and rejoins the ready queue.
 */
static int service_io(sim_t *s)
{
    if (s->io_q[0] == SLOT_NONE) return 0;

    for (int slot = 0; slot < SIM_IO_SLOTS && s->io_q[slot] != SLOT_NONE; ) {
        int i = find_live(s, s->io_q[slot]);
        if (i < 0) { slot++; continue; }

        pcb_t *p = &s->pcb[i];
        if (--p->io_wait > 0) { slot++; continue; }

        /* service is done, but it can only come back if it can be resident
         * again AND the ready queue has room; otherwise leave it queued and
         * retry on a later tick (io_wait stays <= 0) */
        if (mem_free_cells(s) < p->mem_need) { slot++; continue; }

        int c = ready_free_slot_down(s, SIM_READY_SLOTS - 1);
        if (c < 0) { slot++; continue; }

        mem_claim(s, p->name, p->mem_need);

        for (int j = 0; j < p->io_count; j++)
            p->io_at[j] = (p->io_count > 1 && j + 1 < SIM_IO_SLOTS)
                        ? p->io_at[j + 1] : -1;
        p->io_count--;
        p->io_wait = s->io_time;
        p->io_done++;
        s->st.io_completions++;

        s->ready_q[c] = p->name;
        p->state = PS_READY;

        ev_push(s, EV_IO_DONE, p->name, 0);
        if (io_remove(s, slot)) return 1;
        /* io_remove shifted the tail down over `slot`; do not advance */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                          */
/* ------------------------------------------------------------------ */

/*
 * round_robin(): run pcb[i] for up to one quantum, then dispatch on the
 * outcome -- block for I/O, terminate, or get preempted back onto the queue.
 */
static int run_quantum(sim_t *s, int i)
{
    pcb_t *p = &s->pcb[i];

    if (p->name == s->ready_q[SIM_READY_SLOTS - 1]) {
        s->ready_q[SIM_READY_SLOTS - 1] = SLOT_NONE;
    } else {
        /*
         * Arrived from the resource queue: park the current head somewhere
         * free, then take the head slot.
         *
         * CHANGE: the parking scan covers slot 0. The original wrote
         *     for (i = 4; i != 0; i--)
         * which stops at 1, so when slot 0 was the only free one the old
         * head was not parked anywhere -- and the very next line overwrote
         * it with the incoming process. The displaced process stayed
         * PS_READY while belonging to no queue: the dispatcher kept counting
         * it as runnable and kept failing to find it, and the run idled to
         * infinity with that process never finishing. Slot 4 is excluded
         * because it holds the old head itself.
         */
        p->state = PS_READY;
        char displaced = s->ready_q[SIM_READY_SLOTS - 1];
        if (displaced >= 'A' && displaced <= 'L') {
            int k = ready_free_slot_down(s, SIM_READY_SLOTS - 2);
            if (k >= 0) {
                s->ready_q[k] = displaced;
            } else {
                /* no room at all: hand it back to the resource queue rather
                 * than drop it (see the invariant repair in sim_run) */
                int r = 0;
                while (r < SIM_RES_SLOTS && s->res_q[r] != SLOT_FREE) r++;
                if (r < SIM_RES_SLOTS) {
                    s->res_q[r] = displaced;
                    int di = sim_find(s, displaced);
                    if (di >= 0) s->pcb[di].state = PS_BLOCKED_RES;
                }
            }
        }
        s->ready_q[SIM_READY_SLOTS - 1] = p->name;
        if (res_remove(s, p->name)) return 1;
    }

    s->cpu = i;
    p->switches++;
    s->st.switches++;
    ev_push(s, EV_DISPATCH, p->name, p->exec_left);
    if (publish(s)) return 1;

    int left = s->quantum;
    while (left > 0 && p->exec_left > 0 && p->io_at[0] != 0) {
        left--;
        p->exec_left--;
        p->cpu_ticks++;
        s->st.tick++;
        s->st.cpu_busy++;
        gantt_push(s, p->name);

        if (p->io_count > 0) {
            for (int k = 0; k < p->io_count; k++) p->io_at[k]--;
        } else {
            p->io_count = -1;
        }

        if (service_io(s)) return 1;

        int d = ready_depth(s);
        if ((uint32_t)d > s->st.ready_depth_max) s->st.ready_depth_max = (uint32_t)d;
    }

    s->cpu = -1;
    if (compact_ready(s)) return 1;

    /*
     * Dispatch on the outcome first, then admit.
     *
     * CHANGE: the original ran its admission step here, between
     * compactar_cola_preparados() and the outcome dispatch below. Admission
     * takes the highest free ready slot, which is exactly the slot
     * compact_ready() had just freed for a preempted process to return to --
     * so when admission won the race, the preempted process was written
     * nowhere and vanished: still PS_READY, in no queue, invisible to the
     * dispatcher but still counted by it. The run then idled forever.
     * Placing the running process first makes the slot it needs reserved by
     * construction.
     */
    if (p->io_at[0] == 0 && p->io_count > 0) {
        /* swap out and queue for I/O */
        mem_release(s, p->name);
        p->swapped++;

        int k = 0;
        while (k < SIM_IO_SLOTS - 1 && s->io_q[k] != SLOT_NONE) k++;
        if (s->io_q[k] == SLOT_NONE) {
            p->state = PS_BLOCKED_IO;
            s->io_q[k] = p->name;
            ev_push(s, EV_IO_BLOCK, p->name, p->io_count);
        }
    } else if (p->exec_left <= 0) {
        s->target--;
        for (int j = 0; j < SIM_NRES; j++) {
            s->avail[j] += p->alloc[j];
            p->alloc[j] = 0;
        }
        p->state = PS_TERMINATED;
        mem_release(s, p->name);
        s->st.completions++;
        ev_push(s, EV_EXIT, p->name, (int)s->st.tick);
        try_admit_one(s, 0);
    } else {
        /* quantum expired mid-run: back onto the queue */
        int c = ready_free_slot_down(s, SIM_READY_SLOTS - 1);
        if (c >= 0) {
            s->ready_q[c] = p->name;
        } else {
            /* cannot happen now that admission runs after this point, but
             * losing a runnable process is unrecoverable, so fall back to
             * swapping it out rather than dropping it */
            mem_release(s, p->name);
            p->swapped++;
            p->state = PS_NEW;
            int k = wait_free_slot(s);
            if (k >= 0) s->wait_q[k] = p->name;
        }
        ev_push(s, EV_PREEMPT, p->name, p->exec_left);
    }

    if (--s->admit_throttle <= 0) {
        try_admit_one(s, 0);
        s->admit_throttle = 3;
    }

    return publish(s);
}

/* ------------------------------------------------------------------ */
/*  Setup + main loop                                                 */
/* ------------------------------------------------------------------ */

void sim_init(sim_t *s, int quantum, int io_time, int target, int run_all,
              int use_banker, uint64_t seed)
{
    memset(s, 0, sizeof(*s));
    rng_seed(&s->rng, seed);

    s->quantum        = quantum;
    s->io_time        = io_time;
    s->target         = target;
    s->run_all        = run_all;
    s->use_banker     = use_banker;
    s->admit_throttle = 3;
    s->cpu            = -1;
    s->outcome        = SIM_RUNNING;

    s->total[0] = s->avail[0] = 20;
    s->total[1] = s->avail[1] = 30;
    s->total[2] = s->avail[2] = 25;

    for (int i = 0; i < SIM_NPROC; i++) {
        s->pcb[i].name    = (char)('A' + i);
        s->pcb[i].state   = PS_NEW;
        s->pcb[i].io_wait = io_time;
        s->wait_q[i]      = (char)('A' + i);
    }
    for (int i = 0; i < SIM_READY_SLOTS; i++) s->ready_q[i] = SLOT_FREE;
    for (int i = 0; i < SIM_RES_SLOTS;   i++) s->res_q[i]   = SLOT_FREE;
    for (int i = 0; i < SIM_IO_SLOTS;    i++) s->io_q[i]    = SLOT_NONE;

    /* five distinct starters. The original rolled 5 letters and re-rolled
     * until no duplicates remained; a partial shuffle is the same uniform
     * draw without the retry loop. */
    char bag[SIM_NPROC];
    for (int i = 0; i < SIM_NPROC; i++) bag[i] = (char)('A' + i);
    for (int i = 0; i < SIM_READY_SLOTS; i++) {
        int j = i + rng_int(&s->rng, SIM_NPROC - i);
        char t = bag[i]; bag[i] = bag[j]; bag[j] = t;
        s->ready_q[i] = bag[i];
    }

    wait_remove_admitted(s, s->ready_q, SIM_READY_SLOTS);
}

void sim_run(sim_t *s)
{
    admit_initial_batch(s);
    if (publish(s)) return;

    int alive  = count_alive(s);
    int ready  = count_state(s, PS_READY);
    uint64_t last_progress_tick = 0;
    uint32_t last_completions = 0;
    uint32_t idle_rounds = 0;

    while (alive != 0) {
        int dispatched = 0;

        if (ready > 0) {
            int i = -1;

            /* pick something to run: the ready head if there is one,
             * otherwise rotate the queue, otherwise retry the resource
             * queue */
            for (int spin = 0; ; spin++) {
                if (ready_head_live(s)) {
                    i = roll_request(s);
                    if (i < 0) { s->outcome = SIM_BAD_PCB; return; }
                    if (grant_request(s, i, 1)) break;

                    block_on_resources(s, i);
                    s->ready_q[SIM_READY_SLOTS - 1] = SLOT_FREE;
                    if (compact_ready(s)) return;
                } else if (ready_has_any(s)) {
                    /*
                     * The head slot is empty but processes are queued behind
                     * it. Rotate them up one place and look again.
                     *
                     * CHANGE: the original had no such case. Its dispatch
                     * loop tested only cola_preparados[4], so a ready queue
                     * whose head had been vacated -- while slots 0..3 still
                     * held runnable processes and the resource queue could
                     * not be satisfied -- fell straight through to the
                     * bottom of the outer loop, changed nothing, and came
                     * back to the identical state. error_sistema() does not
                     * fire there (it requires the ready queue to be empty),
                     * so the program span forever on a frozen screen. It is
                     * reachable from ordinary seeds.
                     */
                    if (compact_ready(s)) return;
                } else {
                    i = retry_res_queue(s);
                    break;
                }

                if (s->outcome == SIM_ABORTED) return;
                if (spin > 4 * SIM_READY_SLOTS) { i = -1; break; }
            }

            while (i >= 0) {
                if (run_quantum(s, i)) return;
                dispatched = 1;

                if (!s->run_all && s->target <= 0) {
                    s->outcome = SIM_DONE_COUNT;
                    return;
                }
                i = retry_res_queue(s);
            }
        }

        if (!dispatched) {
            /*
             * Invariant repair. Nothing was dispatched, so if a process is
             * PS_READY but sitting in no queue the dispatcher can never see
             * it again. That is a bug if it happens -- it is counted, and
             * `make check` asserts the counter stays zero -- but repairing
             * it is strictly better than idling forever.
             */
            for (int k = 0; k < SIM_NPROC; k++) {
                if (s->pcb[k].state != PS_READY) continue;
                int queued = 0;
                for (int q = 0; q < SIM_READY_SLOTS; q++)
                    if (s->ready_q[q] == s->pcb[k].name) { queued = 1; break; }
                if (queued) continue;

                int slot = ready_free_slot_down(s, SIM_READY_SLOTS - 1);
                if (slot >= 0) {
                    s->ready_q[slot] = s->pcb[k].name;
                    s->st.repairs++;
                }
            }

            /*
             * Nothing was dispatched this round. Service I/O, then try to
             * pull a process in from the waiting queue.
             *
             * Reaching this from the `ready > 0` branch means the dispatcher
             * found nothing runnable despite the count -- the invariant is
             * that every outer iteration either runs a quantum or advances
             * the clock, so the loop can never spin without progressing.
             *
             * CHANGE: the original only ever admitted from inside
             * round_robin(). Once the ready queue drained while processes
             * were still waiting -- every resident process having finished
             * or gone to I/O -- nothing could refill it, so round_robin()
             * was never reached again and the admission step it owned was
             * never run. error_sistema() explicitly does not report that
             * state (it requires an empty waiting queue), so the simulation
             * idled forever with work left to do.
             */
            if (service_io(s)) return;
            try_admit_one(s, 1);
            s->st.tick++;
            gantt_push(s, '.');
            if (publish(s)) return;
        }

        alive = count_alive(s);
        ready = count_state(s, PS_READY);

        /* Only meaningful once a round has failed to dispatch anything;
         * mid-dispatch the queues are legitimately in flux. */
        if (!dispatched && no_progress_possible(s)) {
            ev_push(s, EV_DEADLOCK, s->res_q[0], 0);
            s->st.deadlock_tick = s->st.tick;
            s->outcome = SIM_DEADLOCK;
            return;
        }

        /*
         * Safety valve. It counts consecutive rounds in which no quantum
         * ran, not elapsed ticks: the idle path advances the clock on every
         * pass, so a tick-based deadline would be reset forever by a loop
         * that is doing nothing but ticking.
         *
         * A legitimate idle stretch only lasts while an I/O burst drains,
         * which is bounded by io_time per queued process, so this threshold
         * is orders of magnitude clear of any real run.
         */
        (void)last_progress_tick;
        if (dispatched || s->st.completions != last_completions) {
            last_completions   = s->st.completions;
            last_progress_tick = s->st.tick;
            idle_rounds = 0;
        } else if (++idle_rounds > 100000) {
            s->outcome = SIM_STALLED;
            return;
        }
    }

    s->outcome = SIM_DONE_ALL;
}

/* ------------------------------------------------------------------ */
/*  Names                                                             */
/* ------------------------------------------------------------------ */

const char *sim_outcome_name(sim_outcome o)
{
    switch (o) {
        case SIM_RUNNING:    return "running";
        case SIM_DONE_ALL:   return "all processes finished";
        case SIM_DONE_COUNT: return "process quota reached";
        case SIM_DEADLOCK:   return "deadlock: resources exhausted";
        case SIM_STALLED:    return "stalled: no progress";
        case SIM_BAD_PCB:    return "error: invalid PCB reference";
        case SIM_ABORTED:    return "aborted by user";
    }
    return "?";
}

const char *sim_state_name(int state)
{
    switch (state) {
        case PS_TERMINATED:  return "done";
        case PS_NEW:         return "new";
        case PS_READY:       return "ready";
        case PS_BLOCKED_IO:  return "io";
        case PS_BLOCKED_RES: return "res";
    }
    return "?";
}

const char *sim_event_verb(int kind)
{
    switch (kind) {
        case EV_ADMIT:     return "admitted";
        case EV_DISPATCH:  return "dispatched";
        case EV_PREEMPT:   return "preempted";
        case EV_IO_BLOCK:  return "blocked on I/O";
        case EV_IO_DONE:   return "I/O complete";
        case EV_RES_BLOCK: return "blocked on resources";
        case EV_RES_GRANT: return "resources granted";
        case EV_EXIT:      return "terminated";
        case EV_NOMEM:     return "no memory, deferred";
        case EV_DEADLOCK:  return "DEADLOCK";
        case EV_UNSAFE:    return "refused: unsafe state";
    }
    return "?";
}
