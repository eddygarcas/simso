/*
 * bench.c - run many independent simulations across many cores.
 *
 * This is the part that is genuinely *parallel* rather than merely
 * concurrent: each run is a closed system with its own sim_t and its own
 * rng_t, so N workers scale essentially linearly and nothing needs a lock
 * except the few microseconds of handing out the next run index.
 *
 * It only became possible once the engine stopped using globals -- the
 * original could not have run two simulations in one process at all, let
 * alone two at once.
 *
 * What it is for: the scheduler's behaviour is stochastic, so a single
 * animated run tells you very little. Sweeping a few hundred seeds tells you
 * the deadlock rate, the spread of completion times, and how a parameter
 * change (quantum, I/O time, --banker) actually moves them.
 */
#include "bench.h"
#include "sim.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint64_t ticks;
    uint32_t completions;
    uint32_t switches;
    uint32_t io;
    uint32_t res_blocks;
    uint32_t unsafe;
    double   cpu_util;
    double   mem_mean;
    int      outcome;
} run_result;

typedef struct {
    const bench_cfg *cfg;
    run_result      *out;
    pthread_mutex_t  mu;
    int              next;
} job_pool;

static void *worker(void *arg)
{
    job_pool *jp = (job_pool *)arg;

    for (;;) {
        pthread_mutex_lock(&jp->mu);
        int i = jp->next < jp->cfg->runs ? jp->next++ : -1;
        pthread_mutex_unlock(&jp->mu);
        if (i < 0) break;

        /* One engine per run, on the worker's own stack: ~8KB, no sharing,
         * no allocation, no observer. */
        sim_t s;
        sim_init(&s, jp->cfg->quantum, jp->cfg->io_time, jp->cfg->target,
                 jp->cfg->run_all, jp->cfg->use_banker,
                 jp->cfg->seed0 + (uint64_t)i * 0x9E3779B97F4A7C15ULL);
        sim_run(&s);

        run_result *r = &jp->out[i];
        r->ticks       = s.st.tick;
        r->completions = s.st.completions;
        r->switches    = s.st.switches;
        r->io          = s.st.io_completions;
        r->res_blocks  = s.st.res_blocks;
        r->unsafe      = s.st.unsafe_rejects;
        r->cpu_util    = s.st.tick ? (double)s.st.cpu_busy / s.st.tick : 0.0;
        r->mem_mean    = s.st.mem_samples
                       ? (double)s.st.mem_used_sum / s.st.mem_samples : 0.0;
        r->outcome     = s.outcome;
    }
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void histogram(const uint64_t *sorted, int n, int width)
{
    if (n < 2) return;
    uint64_t lo = sorted[0], hi = sorted[n - 1];
    if (hi == lo) return;

    enum { BINS = 12 };
    int count[BINS] = {0};
    for (int i = 0; i < n; i++) {
        int b = (int)((sorted[i] - lo) * BINS / (hi - lo + 1));
        if (b >= BINS) b = BINS - 1;
        count[b]++;
    }
    int peak = 0;
    for (int b = 0; b < BINS; b++) if (count[b] > peak) peak = count[b];
    if (!peak) return;

    printf("\n  ticks to completion\n");
    for (int b = 0; b < BINS; b++) {
        uint64_t edge = lo + (hi - lo) * b / BINS;
        int bars = count[b] * width / peak;
        printf("  %8llu |", (unsigned long long)edge);
        for (int i = 0; i < bars; i++) putchar('#');
        printf("  %d\n", count[b]);
    }
}

int bench_run(const bench_cfg *cfg)
{
    if (cfg->runs <= 0 || cfg->jobs <= 0) return 2;

    run_result *out = calloc((size_t)cfg->runs, sizeof *out);
    pthread_t  *th  = calloc((size_t)cfg->jobs, sizeof *th);
    if (!out || !th) { free(out); free(th); return 2; }

    job_pool jp = { .cfg = cfg, .out = out, .next = 0 };
    pthread_mutex_init(&jp.mu, NULL);

    if (!cfg->quiet)
        printf("simso benchmark: %d runs on %d threads "
               "(quantum=%d io=%d mode=%s avoidance=%s)\n",
               cfg->runs, cfg->jobs, cfg->quantum, cfg->io_time,
               cfg->run_all ? "all" : "count",
               cfg->use_banker ? "banker" : "simple");

    double t0 = now_sec();

    int spawned = 0;
    for (int i = 0; i < cfg->jobs; i++)
        if (pthread_create(&th[i], NULL, worker, &jp) == 0) spawned++;

    if (spawned == 0) {            /* no threads: run inline rather than fail */
        worker(&jp);
    } else {
        for (int i = 0; i < spawned; i++) pthread_join(th[i], NULL);
    }

    double elapsed = now_sec() - t0;
    pthread_mutex_destroy(&jp.mu);

    /* ---- aggregate ---- */
    uint64_t *ticks = calloc((size_t)cfg->runs, sizeof *ticks);
    if (!ticks) { free(out); free(th); return 2; }

    double sum_util = 0, sum_mem = 0;
    double sum_sw = 0, sum_io = 0, sum_rb = 0, sum_done = 0, sum_unsafe = 0;
    int by_outcome[8] = {0};

    for (int i = 0; i < cfg->runs; i++) {
        ticks[i]    = out[i].ticks;
        sum_util   += out[i].cpu_util;
        sum_mem    += out[i].mem_mean;
        sum_sw     += out[i].switches;
        sum_io     += out[i].io;
        sum_rb     += out[i].res_blocks;
        sum_done   += out[i].completions;
        sum_unsafe += out[i].unsafe;
        if (out[i].outcome >= 0 && out[i].outcome < 8) by_outcome[out[i].outcome]++;
    }
    qsort(ticks, (size_t)cfg->runs, sizeof *ticks, cmp_u64);

    int n = cfg->runs;
    uint64_t total_ticks = 0;
    for (int i = 0; i < n; i++) total_ticks += ticks[i];

    printf("\n  wall time        %.3fs   (%.0f runs/s, %.2f Mtick/s)\n",
           elapsed, n / (elapsed > 0 ? elapsed : 1e-9),
           total_ticks / (elapsed > 0 ? elapsed : 1e-9) / 1e6);
    printf("  ticks            min %llu  p50 %llu  p95 %llu  max %llu\n",
           (unsigned long long)ticks[0],
           (unsigned long long)ticks[n / 2],
           (unsigned long long)ticks[(int)(n * 0.95) < n ? (int)(n * 0.95) : n - 1],
           (unsigned long long)ticks[n - 1]);
    printf("  completions      mean %.2f\n", sum_done / n);
    printf("  cpu utilisation  mean %.1f%%\n", sum_util / n * 100.0);
    printf("  memory in use    mean %.1f / %d cells (%.0f%%)\n",
           sum_mem / n, SIM_MEM_CELLS, sum_mem / n / SIM_MEM_CELLS * 100.0);
    printf("  context switches mean %.1f\n", sum_sw / n);
    printf("  i/o completions  mean %.1f\n", sum_io / n);
    printf("  resource blocks  mean %.1f\n", sum_rb / n);
    if (cfg->use_banker)
        printf("  unsafe refusals  mean %.1f\n", sum_unsafe / n);

    printf("\n  outcomes\n");
    for (int o = 0; o < 8; o++)
        if (by_outcome[o])
            printf("  %-32s %5d  (%4.1f%%)\n",
                   sim_outcome_name((sim_outcome)o), by_outcome[o],
                   100.0 * by_outcome[o] / n);

    histogram(ticks, n, 40);

    free(ticks);
    free(out);
    free(th);
    return 0;
}
