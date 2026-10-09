/*
 * main.c - argument handling and thread orchestration.
 *
 * Interactive mode wires three pieces together:
 *   sim.c    engine, on its own thread, I/O-free
 *   shared.c the publish/pace handshake between the two threads
 *   ui.c     renderer + input, on the main thread
 *
 * Batch mode (--bench) skips the UI entirely and fans the same engine out
 * across cores. Same sim.c either way.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench.h"
#include "shared.h"
#include "sim.h"
#include "ui.h"

typedef struct {
    sim_t    *sim;
    shared_t *sh;
} sim_thread_args;

static void *sim_thread_main(void *arg)
{
    sim_thread_args *a = (sim_thread_args *)arg;
    sim_run(a->sim);
    shared_mark_finished(a->sh, a->sim->outcome);
    return NULL;
}

static void usage(const char *argv0)
{
    printf(
"simso - operating system scheduler simulator\n"
"\n"
"usage: %s [options]\n"
"\n"
"simulation\n"
"  -q, --quantum N     CPU time slice (1-50)\n"
"  -i, --io N          I/O service time (1-50)\n"
"  -f, --finish N      stop after N processes complete (1-12)\n"
"      --all           run until every process has finished\n"
"      --banker        use a real Banker's safety check instead of the\n"
"                      original's plain availability test\n"
"      --seed N        PRNG seed; a given seed replays exactly\n"
"\n"
"interactive\n"
"      --speed MS      delay between steps (0 = uncapped, default 110)\n"
"      --paused        start paused\n"
"      --ascii         ASCII glyphs instead of Unicode blocks\n"
"\n"
"batch\n"
"      --bench N       run N simulations headless and report statistics\n"
"  -j, --jobs N        worker threads for --bench (default: online CPUs)\n"
"\n"
"  -h, --help          this text\n"
"\n"
"keys while running\n"
"  space  pause/resume      n  single step\n"
"  +/-    faster/slower     0  uncapped     1  slow\n"
"  q      quit\n"
"\n"
"With no simulation options, simso prompts for them as the original did.\n",
        argv0);
}

static int parse_int(const char *s, int lo, int hi, const char *what, int *ok)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "simso: %s must be an integer in [%d, %d], got '%s'\n",
                what, lo, hi, s);
        *ok = 0;
        return lo;
    }
    return (int)v;
}

int main(int argc, char **argv)
{
    int quantum = -1, io_time = -1, target = -1, run_all = -1;
    int use_banker = 0, speed_ms = 110, paused = 0, ascii_mode = 0;
    int bench_runs = 0, jobs = 0;
    uint64_t seed = 0;
    int have_seed = 0, ok = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEED_ARG(name) \
            if (i + 1 >= argc) { fprintf(stderr, "simso: %s needs a value\n", name); return 2; }

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "-q") || !strcmp(a, "--quantum")) {
            NEED_ARG(a); quantum = parse_int(argv[++i], 1, 50, "quantum", &ok);
        } else if (!strcmp(a, "-i") || !strcmp(a, "--io")) {
            NEED_ARG(a); io_time = parse_int(argv[++i], 1, 50, "io time", &ok);
        } else if (!strcmp(a, "-f") || !strcmp(a, "--finish")) {
            NEED_ARG(a); target = parse_int(argv[++i], 1, SIM_NPROC, "finish count", &ok);
            run_all = 0;
        } else if (!strcmp(a, "--all")) {
            run_all = 1; target = 0;
        } else if (!strcmp(a, "--banker")) {
            use_banker = 1;
        } else if (!strcmp(a, "--seed")) {
            NEED_ARG(a); seed = (uint64_t)strtoull(argv[++i], NULL, 10); have_seed = 1;
        } else if (!strcmp(a, "--speed")) {
            NEED_ARG(a); speed_ms = parse_int(argv[++i], 0, 1000, "speed", &ok);
        } else if (!strcmp(a, "--paused")) {
            paused = 1;
        } else if (!strcmp(a, "--ascii")) {
            ascii_mode = 1;
        } else if (!strcmp(a, "--bench")) {
            NEED_ARG(a); bench_runs = parse_int(argv[++i], 1, 1000000, "bench runs", &ok);
        } else if (!strcmp(a, "-j") || !strcmp(a, "--jobs")) {
            NEED_ARG(a); jobs = parse_int(argv[++i], 1, 1024, "jobs", &ok);
        } else {
            fprintf(stderr, "simso: unknown option '%s' (try --help)\n", a);
            return 2;
        }
        #undef NEED_ARG
    }
    if (!ok) return 2;

    if (!have_seed) seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);

    /* ---- batch mode: no terminal involved ---- */
    if (bench_runs > 0) {
        if (jobs <= 0) {
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            jobs = n > 0 ? (int)n : 4;
        }
        bench_cfg cfg = {
            .runs = bench_runs, .jobs = jobs,
            .quantum = quantum > 0 ? quantum : 10,
            .io_time = io_time > 0 ? io_time : 5,
            .target  = target  >= 0 ? target : 5,
            .run_all = run_all == 1,
            .use_banker = use_banker,
            .seed0 = seed, .quiet = 0,
        };
        return bench_run(&cfg);
    }

    /* ---- interactive mode ---- */
    if (ascii_mode) ui_set_ascii(1);
    if (ui_start() != 0) return 1;

    if (quantum < 0)
        quantum = ui_prompt_int("CPU quantum",
                                "how many ticks a process runs before preemption",
                                1, 50);
    if (io_time < 0)
        io_time = ui_prompt_int("I/O service time",
                                "how long an I/O burst keeps a process blocked",
                                1, 50);
    if (run_all < 0) {
        int c = ui_prompt_choice("Termination mode",
                                 "stop after N processes complete",
                                 "run until every process has finished");
        if (c == 1) {
            target  = ui_prompt_int("Processes to finish", "1-12", 1, SIM_NPROC);
            run_all = 0;
        } else {
            target  = 0;
            run_all = 1;
        }
    }

    static sim_t   sim;         /* static: ~9KB, keeps it off the stack */
    static shared_t sh;

    shared_init(&sh, speed_ms, paused);
    sim_init(&sim, quantum, io_time, target, run_all, use_banker, seed);
    sim.observe       = shared_publish;
    sim.observer_user = &sh;

    /* seed the first frame so the UI has something to draw immediately */
    sh.snap = sim;

    sim_thread_args args = { .sim = &sim, .sh = &sh };
    pthread_t sim_tid;
    if (pthread_create(&sim_tid, NULL, sim_thread_main, &args) != 0) {
        ui_stop();
        fprintf(stderr, "simso: could not start the simulation thread\n");
        return 1;
    }

    ui_loop(&sh);

    /* ui_loop has either seen `finished` or already called shared_quit();
     * either way the sim thread is on its way out. */
    shared_quit(&sh);
    pthread_join(sim_tid, NULL);

    ui_stop();
    shared_destroy(&sh);

    printf("simso: %s  (seed %llu, %llu ticks, %u completed)\n",
           sim_outcome_name(sim.outcome), (unsigned long long)seed,
           (unsigned long long)sim.st.tick, sim.st.completions);
    return 0;
}
