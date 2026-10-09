/*
 * rng.h - per-instance deterministic PRNG.
 *
 * The original used rand()/srand(), which is process-global: two engines
 * running on two threads would fight over one hidden state and neither run
 * would be reproducible. Every sim_t carries its own rng_t instead, so a
 * (seed, parameters) pair always replays the same run, and N engines on N
 * threads never touch each other's state.
 *
 * xorshift64* - fast, no locks, no allocation, good enough for a scheduler
 * toy. Not for cryptography.
 */
#ifndef SIMSO_RNG_H
#define SIMSO_RNG_H

#include <stdint.h>

typedef struct { uint64_t s; } rng_t;

static inline void rng_seed(rng_t *r, uint64_t seed)
{
    /* state 0 is a fixed point of xorshift, so fold it away */
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

static inline uint64_t rng_next(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* uniform in [0, n); returns 0 for n <= 0 (the old safe_rr() guard) */
static inline int rng_int(rng_t *r, int n)
{
    return n > 0 ? (int)(rng_next(r) % (uint64_t)n) : 0;
}

/* uniform in [lo, hi] inclusive */
static inline int rng_range(rng_t *r, int lo, int hi)
{
    return hi > lo ? lo + rng_int(r, hi - lo + 1) : lo;
}

#endif /* SIMSO_RNG_H */
