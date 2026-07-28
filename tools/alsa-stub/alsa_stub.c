#define _DEFAULT_SOURCE /* usleep() under -std=c11 on glibc; must precede every header */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
/* The stub models one thing beyond "accept everything": the PCM state
 * machine's rule that a write is only legal from PREPARED or RUNNING.
 *
 * Without it the stub could not express the bug that broke every seek and
 * stop on real hardware — snd_pcm_drop() on the network thread leaving the
 * stream in SETUP while the writer thread was inside snd_pcm_writei(),
 * which real ALSA answers with -EBADFD and the daemon treated as fatal. A
 * stub whose drop() and writei() are both no-ops has no conflicting state
 * for a test (or ThreadSanitizer) to catch, so the regression test passed
 * just as happily with the locking removed. Modelling the state is what
 * makes tools/race_stress.py meaningful. */
enum { STUB_OPEN, STUB_SETUP, STUB_PREPARED, STUB_RUNNING };
struct _snd_pcm {
    int state;
    /* Bumped by every state-resetting call. A write that spans one of these
     * is doomed even though the state looks fine again afterwards, which is
     * the detail that makes the real bug so slippery: drop() and prepare()
     * run back to back, so by the time the blocked writer wakes up the
     * stream reads as PREPARED and only the in-flight write was lost. */
    unsigned epoch;
};
static struct _snd_pcm g_pcm;
static snd_pcm_uframes_t g_written;

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t s, int m) {
    (void)name; (void)s; (void)m; g_pcm.state = STUB_OPEN; *pcm = &g_pcm; return 0;
}
int snd_pcm_close(snd_pcm_t *p) { p->epoch++; p->state = STUB_SETUP; return 0; }
int snd_pcm_prepare(snd_pcm_t *p) { p->epoch++; p->state = STUB_PREPARED; return 0; }
int snd_pcm_nonblock(snd_pcm_t *p, int nonblock) { (void)p; (void)nonblock; return 0; }
/* RUNNING once anything has been written, matching the state machine above. */
/* Reports the real state name rather than collapsing everything that is not
 * RUNNING to OPEN: the drain path now distinguishes PREPARED (a tail that was
 * written but never reached the start threshold) from the rest, and it cannot
 * be exercised against a stub that never says PREPARED. */
snd_pcm_state_t snd_pcm_state(snd_pcm_t *p) {
    switch (p->state) {
    case STUB_PREPARED: return SND_PCM_STATE_PREPARED;
    case STUB_RUNNING:  return SND_PCM_STATE_RUNNING;
    default:            return SND_PCM_STATE_SETUP;
    }
}

int snd_pcm_start(snd_pcm_t *p) {
    if (p->state != STUB_PREPARED) return -EBADFD;
    p->state = STUB_RUNNING;
    return 0;
}
int snd_pcm_drain(snd_pcm_t *p) { p->epoch++; p->state = STUB_SETUP; return 0; }
int snd_pcm_drop(snd_pcm_t *p) { p->epoch++; p->state = STUB_SETUP; return 0; }
int snd_pcm_resume(snd_pcm_t *p) { p->epoch++; p->state = STUB_PREPARED; return 0; }
/* Consumes everything, but only from a writable state.
 *
 * HALO_STUB_WRITE_DELAY_US makes the write *block*, the way a real one does
 * while it waits for the ring buffer to drain. That delay is not decoration:
 * it is the precondition of the drop()-during-writei() race, and with an
 * instant write the two threads essentially never overlap, so a stress test
 * passes whether or not the daemon locks correctly. Left at 0 for `make
 * check` so the normal suite stays fast. */
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *p, const void *b, snd_pcm_uframes_t n) {
    (void)b;
    if (p->state != STUB_PREPARED && p->state != STUB_RUNNING) return -EBADFD;
    p->state = STUB_RUNNING;
    const char *delay = getenv("HALO_STUB_WRITE_DELAY_US");
    if (delay) {
        long us = atol(delay);
        /* Re-check the state after blocking, exactly as the kernel would:
         * a drop() that lands mid-write invalidates the stream. */
        if (us > 0) {
            unsigned epoch_at_entry = p->epoch;
            usleep((unsigned)us);
            if (p->epoch != epoch_at_entry) return -EBADFD;
        }
    }
    g_written += n;
    return (snd_pcm_sframes_t)n;
}
/* Real ALSA leaves the stream PREPARED after a successful hw_params. */
int snd_pcm_hw_params(snd_pcm_t *p, snd_pcm_hw_params_t *h) { (void)h; p->state = STUB_PREPARED; return 0; }
int snd_pcm_hw_params_any(snd_pcm_t *p, snd_pcm_hw_params_t *h) { (void)p; (void)h; return 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_access_t a) { (void)p;(void)h;(void)a; return 0; }
int snd_pcm_hw_params_set_format(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_format_t f) { (void)p;(void)h;(void)f; return 0; }
int snd_pcm_hw_params_test_format(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_format_t f) { (void)p;(void)h;(void)f; return 0; }
int snd_pcm_hw_params_test_rate(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int rate, int dir) { (void)p;(void)h;(void)rate;(void)dir; return 0; }
int snd_pcm_hw_params_set_channels(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int v) { (void)p;(void)h;(void)v; return 0; }
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)p;(void)h;(void)d; (void)v; return 0; }
int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)p;(void)h;(void)v;(void)d; return 0; }
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)p;(void)h;(void)v;(void)d; return 0; }

int snd_pcm_hw_params_set_period_time_near(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                           unsigned int *val, int *dir) {
    (void)p; (void)h; (void)val; (void)dir;
    return 0;
}
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *d) { (void)h;(void)d; *v = 1024; return 0; }
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v) { (void)h; *v = 8192; return 0; }
int snd_pcm_hw_params_get_rate_min(const snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)h;(void)d; *v = 44100; return 0; }
int snd_pcm_hw_params_get_rate_max(const snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)h;(void)d; *v = 768000; return 0; }
int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *h, unsigned int *v) { (void)h; *v = 2; return 0; }
int snd_pcm_set_chmap(snd_pcm_t *p, const snd_pcm_chmap_t *m) { (void)p; (void)m; return 0; }
const char *snd_pcm_format_name(snd_pcm_format_t f) { (void)f; return "STUB_FORMAT"; }
const char *snd_strerror(int e) { (void)e; return "stub error"; }

const char *snd_pcm_state_name(snd_pcm_state_t state) {
    switch (state) {
    case SND_PCM_STATE_OPEN:     return "OPEN";
    case SND_PCM_STATE_SETUP:    return "SETUP";
    case SND_PCM_STATE_PREPARED: return "PREPARED";
    case SND_PCM_STATE_RUNNING:  return "RUNNING";
    case SND_PCM_STATE_XRUN:     return "XRUN";
    case SND_PCM_STATE_DRAINING: return "DRAINING";
    case SND_PCM_STATE_PAUSED:   return "PAUSED";
    default:                     return "UNKNOWN";
    }
}

int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp) {
    (void)pcm;
    if (delayp) *delayp = 0;
    return 0;
}

/* ---- sw_params ----
 *
 * Only start_threshold carries behaviour here: the daemon sets it so a
 * freshly prepared stream waits for a full buffer instead of starting on the
 * first frame and starving. The stub consumes instantly and so can never
 * underrun, which means it cannot reproduce that failure — it can only check
 * that the daemon still asks for the threshold that prevents it. */
static snd_pcm_uframes_t g_start_threshold;

snd_pcm_uframes_t snd_stub_start_threshold(void) { return g_start_threshold; }

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params) {
    (void)pcm; (void)params;
    return 0;
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                          snd_pcm_uframes_t val) {
    (void)pcm; (void)params;
    g_start_threshold = val;
    return 0;
}

int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                    snd_pcm_uframes_t val) {
    (void)pcm; (void)params; (void)val;
    return 0;
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params) {
    (void)pcm; (void)params;
    return 0;
}
