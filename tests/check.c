/*
 * check.c - invariant sweep over the engine.
 *
 * Runs the scheduler across a wide seed and parameter space and asserts the
 * properties that the animated single run can never establish on its own.
 * Every bug found while refactoring (lost processes, non-publishing spins,
 * queues that could not refill) showed up here as a stall or a repair, so
 * these are the regression tests for exactly those.
 */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void fail(const char *what, uint64_t seed, int q, int io, int all,
                 const char *detail)
{
    printf("  FAIL %-22s seed=%llu quantum=%d io=%d mode=%s  %s\n",
           what, (unsigned long long)seed, q, io, all ? "all" : "count", detail);
    failures++;
}

/* every process is in exactly the queue its state says it is in */
static int check_consistency(const sim_t *s, char *msg, size_t n)
{
    for (int i = 0; i < SIM_NPROC; i++) {
        const pcb_t *p = &s->pcb[i];
        int in_ready = 0, in_io = 0, in_res = 0, in_wait = 0;

        for (int k = 0; k < SIM_READY_SLOTS; k++) if (s->ready_q[k] == p->name) in_ready++;
        for (int k = 0; k < SIM_IO_SLOTS;    k++) if (s->io_q[k]    == p->name) in_io++;
        for (int k = 0; k < SIM_RES_SLOTS;   k++) if (s->res_q[k]   == p->name) in_res++;
        for (int k = 0; k < SIM_NPROC;       k++) if (s->wait_q[k]  == p->name) in_wait++;

        if (in_ready > 1 || in_io > 1 || in_res > 1 || in_wait > 1) {
            snprintf(msg, n, "process %c duplicated in a queue", p->name);
            return 0;
        }
        if (p->state == PS_TERMINATED && (in_ready || in_io || in_res || in_wait)) {
            snprintf(msg, n, "terminated process %c still queued", p->name);
            return 0;
        }
        if (p->state == PS_TERMINATED) {
            for (int k = 0; k < SIM_MEM_CELLS; k++)
                if (s->mem[k] == p->name) {
                    snprintf(msg, n, "terminated process %c still holds memory", p->name);
                    return 0;
                }
            for (int j = 0; j < SIM_NRES; j++)
                if (p->alloc[j]) {
                    snprintf(msg, n, "terminated process %c still holds resources", p->name);
                    return 0;
                }
        }
    }

    for (int j = 0; j < SIM_NRES; j++) {
        int held = 0;
        for (int i = 0; i < SIM_NPROC; i++) held += s->pcb[i].alloc[j];
        if (s->avail[j] + held != s->total[j]) {
            snprintf(msg, n, "resource %c leaked: %d avail + %d held != %d",
                     "ABC"[j], s->avail[j], held, s->total[j]);
            return 0;
        }
        if (s->avail[j] < 0) {
            snprintf(msg, n, "resource %c went negative", "ABC"[j]);
            return 0;
        }
    }

    int used = sim_mem_used(s);
    if (used < 0 || used > SIM_MEM_CELLS) {
        snprintf(msg, n, "memory occupancy out of range: %d", used);
        return 0;
    }
    return 1;
}

static char live_msg[160];
static int  live_bad;

static int observer(sim_t *s, void *user)
{
    (void)user;
    if (!live_bad && !check_consistency(s, live_msg, sizeof live_msg))
        live_bad = 1;
    return live_bad;         /* stop the run at the first violation */
}

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 3000;
    const int quanta[] = { 1, 3, 10, 25, 50 };
    const int ios[]    = { 1, 5, 20, 50 };

    printf("engine invariant sweep: %d seeds x %zu quanta x %zu io times "
           "x 2 modes\n", runs,
           sizeof quanta / sizeof *quanta, sizeof ios / sizeof *ios);

    long total = 0, stalls = 0, deadlocks = 0, repairs = 0;
    uint64_t max_ticks = 0;

    for (size_t qi = 0; qi < sizeof quanta / sizeof *quanta; qi++)
    for (size_t ii = 0; ii < sizeof ios / sizeof *ios; ii++)
    for (int all = 0; all < 2; all++)
    for (int r = 0; r < runs; r++) {
        uint64_t seed = 0xC0FFEEULL + (uint64_t)r * 0x9E3779B97F4A7C15ULL
                      + (uint64_t)qi * 1009 + (uint64_t)ii * 10007 + (uint64_t)all * 65537;

        sim_t s;
        sim_init(&s, quanta[qi], ios[ii], all ? 0 : 5, all, 0, seed);

        /* full invariant checking on a sample; the rest run unobserved so the
         * sweep stays fast enough to cover real volume */
        int observed = (r % 50 == 0);
        if (observed) { live_bad = 0; s.observe = observer; }

        sim_run(&s);
        total++;

        if (observed && live_bad)
            fail("invariant", seed, quanta[qi], ios[ii], all, live_msg);

        char msg[160];
        if (s.outcome != SIM_ABORTED && !check_consistency(&s, msg, sizeof msg))
            fail("final state", seed, quanta[qi], ios[ii], all, msg);

        if (s.st.repairs) {
            repairs += s.st.repairs;
            fail("lost process", seed, quanta[qi], ios[ii], all,
                 "engine had to re-queue a PS_READY process");
        }

        switch (s.outcome) {
            case SIM_STALLED:
                stalls++;
                fail("stalled", seed, quanta[qi], ios[ii], all, "no progress");
                break;
            case SIM_DEADLOCK:  deadlocks++; break;
            case SIM_BAD_PCB:
                fail("bad pcb", seed, quanta[qi], ios[ii], all, "invalid PCB reference");
                break;
            case SIM_DONE_ALL:
                for (int i = 0; i < SIM_NPROC; i++)
                    if (s.pcb[i].state != PS_TERMINATED) {
                        fail("incomplete", seed, quanta[qi], ios[ii], all,
                             "reported all-done with a live process");
                        break;
                    }
                break;
            case SIM_DONE_COUNT:
                if (s.st.completions < 5)
                    fail("short count", seed, quanta[qi], ios[ii], all,
                         "quota reached with too few completions");
                break;
            default: break;
        }

        if (s.st.tick > max_ticks) max_ticks = s.st.tick;
        if (failures > 20) { printf("  (too many failures, stopping)\n"); goto done; }
    }

done:
    printf("\n  runs            %ld\n", total);
    printf("  deadlocks       %ld  (%.2f%% - a legitimate outcome)\n",
           deadlocks, 100.0 * deadlocks / (total ? total : 1));
    printf("  stalls          %ld\n", stalls);
    printf("  lost processes  %ld\n", repairs);
    printf("  longest run     %llu ticks\n", (unsigned long long)max_ticks);
    printf("\n%s  (%d failures)\n", failures ? "FAILED" : "all invariants hold",
           failures);
    return failures ? 1 : 0;
}
