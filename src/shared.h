/*
 * shared.h - the hand-off between the simulation thread and the UI thread.
 *
 * In the original, the simulation *was* the render loop: round_robin() called
 * imprimir_colas(), which drew a frame, slept 110ms and polled for 'q'. The
 * sim could not run faster or slower than the animation, and input was only
 * sampled at the points the scheduler happened to draw.
 *
 * Here they are separate threads:
 *
 *   sim thread  -- runs sim_run(). At every former imprimir_colas() point it
 *                  copies its state into shared.snap, bumps seq, and then
 *                  parks itself for `speed_ms` (or indefinitely while
 *                  paused) on cv_sim.
 *   UI thread   -- the main thread. Wakes on its own ~60Hz timer, takes the
 *                  latest snapshot under the lock, releases the lock, and
 *                  draws. Input is read every frame, so pause/step/speed/
 *                  quit respond immediately even when the sim is parked.
 *
 * The lock is held only for two struct copies; all drawing and all sleeping
 * happen outside it, so the two threads never meaningfully contend.
 */
#ifndef SIMSO_SHARED_H
#define SIMSO_SHARED_H

#include <pthread.h>
#include <stdint.h>
#include "sim.h"

#define SPEED_MIN_MS    0      /* uncapped */
#define SPEED_MAX_MS 1000

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv_sim;    /* UI -> sim: resume / step / settings changed */

    sim_t    snap;             /* latest published engine state */
    uint64_t seq;              /* publish counter; UI uses it to spot staleness */

    /* UI-owned controls, read by the sim thread at each publish point */
    int paused;
    int step_req;              /* single-step credits while paused */
    int speed_ms;              /* delay between publishes */
    int quit;

    /* sim-owned status, read by the UI */
    int         finished;
    sim_outcome outcome;
} shared_t;

void shared_init(shared_t *sh, int speed_ms, int start_paused);
void shared_destroy(shared_t *sh);

/* sim side */
int  shared_publish(sim_t *s, void *user);   /* the sim_observer */
void shared_mark_finished(shared_t *sh, sim_outcome o);

/* UI side */
void shared_take(shared_t *sh, sim_t *dst, uint64_t *seq);
void shared_toggle_pause(shared_t *sh);
void shared_step(shared_t *sh);
void shared_nudge_speed(shared_t *sh, int delta_ms);
void shared_set_speed(shared_t *sh, int ms);
void shared_quit(shared_t *sh);

#endif /* SIMSO_SHARED_H */
