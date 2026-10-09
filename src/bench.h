#ifndef SIMSO_BENCH_H
#define SIMSO_BENCH_H

#include <stdint.h>

typedef struct {
    int      runs;
    int      jobs;
    int      quantum;
    int      io_time;
    int      target;
    int      run_all;
    int      use_banker;
    uint64_t seed0;
    int      quiet;
} bench_cfg;

int bench_run(const bench_cfg *cfg);

#endif /* SIMSO_BENCH_H */
