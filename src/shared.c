#include "shared.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static void deadline_in(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

void shared_init(shared_t *sh, int speed_ms, int start_paused)
{
    memset(sh, 0, sizeof(*sh));
    pthread_mutex_init(&sh->mu, NULL);
    pthread_cond_init(&sh->cv_sim, NULL);
    sh->speed_ms = speed_ms;
    sh->paused   = start_paused;
    sh->outcome  = SIM_RUNNING;
}

void shared_destroy(shared_t *sh)
{
    pthread_cond_destroy(&sh->cv_sim);
    pthread_mutex_destroy(&sh->mu);
}

/*
 * The sim_observer. Publishes the engine's state, then blocks until the UI
 * says it may continue. Returns 1 to ask sim_run() to unwind, which is how
 * 'q' propagates out of the scheduler without a longjmp.
 */
int shared_publish(sim_t *s, void *user)
{
    shared_t *sh = (shared_t *)user;

    pthread_mutex_lock(&sh->mu);

    sh->snap = *s;        /* ~8KB struct copy; the only work under the lock */
    sh->seq++;

    while (!sh->quit) {
        if (sh->paused) {
            if (sh->step_req > 0) { sh->step_req--; break; }
            pthread_cond_wait(&sh->cv_sim, &sh->mu);
        } else if (sh->speed_ms <= 0) {
            break;        /* full speed: never park */
        } else {
            struct timespec ts;
            deadline_in(&ts, sh->speed_ms);
            int r = pthread_cond_timedwait(&sh->cv_sim, &sh->mu, &ts);
            if (r == ETIMEDOUT) break;
            /* woken early => a control changed; re-evaluate from the top */
        }
    }

    int q = sh->quit;
    pthread_mutex_unlock(&sh->mu);
    return q;
}

void shared_mark_finished(shared_t *sh, sim_outcome o)
{
    pthread_mutex_lock(&sh->mu);
    sh->finished = 1;
    sh->outcome  = o;
    pthread_mutex_unlock(&sh->mu);
}

void shared_take(shared_t *sh, sim_t *dst, uint64_t *seq)
{
    pthread_mutex_lock(&sh->mu);
    *dst = sh->snap;
    if (seq) *seq = sh->seq;
    pthread_mutex_unlock(&sh->mu);
}

void shared_toggle_pause(shared_t *sh)
{
    pthread_mutex_lock(&sh->mu);
    sh->paused = !sh->paused;
    pthread_cond_signal(&sh->cv_sim);
    pthread_mutex_unlock(&sh->mu);
}

void shared_step(shared_t *sh)
{
    pthread_mutex_lock(&sh->mu);
    sh->paused = 1;
    sh->step_req++;
    pthread_cond_signal(&sh->cv_sim);
    pthread_mutex_unlock(&sh->mu);
}

void shared_nudge_speed(shared_t *sh, int delta_ms)
{
    pthread_mutex_lock(&sh->mu);
    sh->speed_ms += delta_ms;
    if (sh->speed_ms < SPEED_MIN_MS) sh->speed_ms = SPEED_MIN_MS;
    if (sh->speed_ms > SPEED_MAX_MS) sh->speed_ms = SPEED_MAX_MS;
    pthread_cond_signal(&sh->cv_sim);
    pthread_mutex_unlock(&sh->mu);
}

void shared_set_speed(shared_t *sh, int ms)
{
    pthread_mutex_lock(&sh->mu);
    sh->speed_ms = ms < SPEED_MIN_MS ? SPEED_MIN_MS
                 : ms > SPEED_MAX_MS ? SPEED_MAX_MS : ms;
    pthread_cond_signal(&sh->cv_sim);
    pthread_mutex_unlock(&sh->mu);
}

void shared_quit(shared_t *sh)
{
    pthread_mutex_lock(&sh->mu);
    sh->quit = 1;
    pthread_cond_broadcast(&sh->cv_sim);
    pthread_mutex_unlock(&sh->mu);
}
