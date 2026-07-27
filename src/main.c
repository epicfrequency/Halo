/*
 * halo-daemon — Pi5-side receiver for the HALO audio endpoint protocol.
 * See PROTOCOL.md for the wire format and README.md for build/run/tuning
 * notes. Single client at a time, single ALSA device, no discovery, no
 * multi-endpoint sync — see the design conversation this came out of for
 * why that scope is deliberate, not a shortcut.
 *
 * Gapless design: two ring buffers (ring[0], ring[1]). One is "active"
 * (being drained into ALSA), the other can be "pending" (receiving a
 * preannounced next track). When the active ring drains to empty and a
 * pending stream exists, we flip which ring is active — if the format is
 * unchanged this is a zero-gap handover; if it changed, ALSA has to be
 * drained/closed/reopened, which is the one gap that's physically
 * unavoidable.
 */
#define _DEFAULT_SOURCE /* usleep() under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "protocol.h"
#include "ring_buffer.h"
#include "alsa_output.h"
#include "net_io.h"

#define RING_CAPACITY (1u << 21) /* 2 MiB per ring; ~500ms+ of headroom even at DSD256 stereo */
#define DEFAULT_PORT 5555

/* Legacy-sender fallback only: how long the active stream's AUDIO_DATA may
 * go quiet (with its ring fully drained and a pending preannounce waiting)
 * before we infer a gapless switch without ever having received
 * SWITCH_TO_PENDING. Current senders always send the explicit message
 * (PROTOCOL.md's Gapless section), which makes the switch deterministic;
 * this exists so an old sender doesn't hang forever, and it is deliberately
 * huge because inferring *early* used to deadlock the sender's POSITION
 * accounting (it rejects reports for format_ids it doesn't consider active
 * yet). */
#define PENDING_SWITCH_FALLBACK_NS (20ull * 1000000000ull)

typedef struct {
    halo_ring_t ring[2];
    struct halo_format fmt[2];   /* fmt[i] describes ring[i]'s contents */
    uint32_t format_id[2];
    int active_idx;              /* which of ring[0]/ring[1] feeds ALSA right now */
    int pending_valid;           /* is the *other* ring holding a preannounced track? */
    int switch_requested;        /* sender sent SWITCH_TO_PENDING for the pending slot:
                                  * flip to it as soon as the active ring drains
                                  * (guarded by state_mtx, like pending_valid) */
    int stream_open;             /* has the first FORMAT (non-preannounce) arrived yet? */
    uint64_t flush_barrier_seq;  /* seq of the latest FLUSH; older AUDIO_DATA is pre-seek */

    halo_alsa_ctx_t alsa;
    char alsa_device[64];

    _Atomic int paused;
    _Atomic int running;
    _Atomic uint64_t frames_written; /* frames actually handed to snd_pcm_writei, current active stream */
    _Atomic uint64_t underrun_count; /* cumulative, for the status line */
    _Atomic uint64_t last_active_data_ns; /* mono_ns() of the last AUDIO_DATA routed to the
                                           * active ring — only consulted by the legacy
                                           * no-SWITCH_TO_PENDING fallback */

    int sockfd;
    pthread_mutex_t send_mtx;
    pthread_mutex_t state_mtx;
    _Atomic uint64_t seq_counter;
} halo_state_t;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int formats_equivalent(const struct halo_format *a, const struct halo_format *b) {
    return a->sample_rate == b->sample_rate &&
           a->bits_per_sample == b->bits_per_sample &&
           a->channels == b->channels &&
           a->is_dsd == b->is_dsd &&
           a->dsd_rate_mult == b->dsd_rate_mult;
}

/* Defined below with the other control-message senders; the writer thread
 * needs it for the gapless-reopen failure path. */
static void send_format_rejected(halo_state_t *st, uint32_t format_id, uint32_t reason);

/* ---------------- ALSA writer thread ---------------- */

static void *alsa_writer_thread(void *arg) {
    halo_state_t *st = (halo_state_t *)arg;
    halo_set_realtime_priority();

    size_t buf_bytes = 65536;
    uint8_t *scratch = malloc(buf_bytes);
    if (!scratch) { fprintf(stderr, "halo: OOM in writer thread\n"); return NULL; }

    while (atomic_load(&st->running)) {
        if (atomic_load(&st->paused)) {
            usleep(5000);
            continue;
        }

        pthread_mutex_lock(&st->state_mtx);
        int idx = st->active_idx;
        int stream_open = st->stream_open;
        struct halo_format fmt = st->fmt[idx];
        uint32_t fid = st->format_id[idx];
        pthread_mutex_unlock(&st->state_mtx);

        if (!stream_open) { usleep(5000); continue; }

        size_t frame_size = halo_format_frame_size(&fmt);
        size_t used = halo_ring_used(&st->ring[idx]);
        /* Only whole frames are ever eligible to be pulled out of the ring
         * — see the "frame-alignment truncation bug" note in the design
         * conversation this came out of. Any 1..frame_size-1 leftover
         * bytes stay put and wait for more data to complete the frame,
         * rather than being silently read-and-dropped, which used to
         * permanently misalign every sample after it. */
        size_t usable_bytes = used - (used % frame_size);

        if (usable_bytes == 0) {
            /* Nothing (whole-frame) buffered for the active stream right
             * now. Check whether a preannounced next track is ready to
             * take over. The switch is sender-driven (SWITCH_TO_PENDING
             * sets switch_requested; the active ring draining to empty is
             * exactly the "old tail finished playing" condition PROTOCOL.md
             * asks us to wait for) — an empty active ring *alone* is not a
             * switch signal, since it also happens transiently whenever the
             * sender's feed pacing or the network briefly stalls
             * mid-track. Legacy senders that never send the message get a
             * conservative idle-time fallback instead. */
            pthread_mutex_lock(&st->state_mtx);
            int do_switch = 0;
            if (st->pending_valid) {
                if (st->switch_requested) {
                    do_switch = 1;
                } else {
                    uint64_t last = atomic_load(&st->last_active_data_ns);
                    do_switch = (mono_ns() - last) > PENDING_SWITCH_FALLBACK_NS;
                    if (do_switch) {
                        fprintf(stderr, "halo: legacy fallback gapless switch "
                                        "(no SWITCH_TO_PENDING, active idle)\n");
                    }
                }
            }
            if (do_switch) {
                int other = 1 - st->active_idx;
                struct halo_format next_fmt = st->fmt[other];
                uint32_t next_fid = st->format_id[other];
                int same = formats_equivalent(&fmt, &next_fmt);

                if (!same) {
                    /* Unlock while doing the (slow, blocking) drain/reopen
                     * so we don't hold state_mtx across I/O. */
                    pthread_mutex_unlock(&st->state_mtx);
                    halo_alsa_drain(&st->alsa);
                    if (halo_alsa_open(&st->alsa, st->alsa_device, &next_fmt) != 0) {
                        /* The next track's format won't open. Say so rather
                         * than switching to a stream that can never play. */
                        send_format_rejected(st, next_fid, HALO_REJECT_DEVICE_BUSY);
                        pthread_mutex_lock(&st->state_mtx);
                        st->pending_valid = 0;
                        st->switch_requested = 0;
                        pthread_mutex_unlock(&st->state_mtx);
                        continue;
                    }
                    pthread_mutex_lock(&st->state_mtx);
                }
                /* Re-check under lock in case a FLUSH raced with us */
                if (st->pending_valid) {
                    st->active_idx = other;
                    st->pending_valid = 0;
                    st->switch_requested = 0;
                    /* New position epoch (PROTOCOL.md "Position epochs") —
                     * the sender resets its own enqueued-frames baseline at
                     * its commit, and both sides' counters must restart
                     * together for its pacing math to stay coherent. */
                    atomic_store(&st->frames_written, 0);
                    atomic_store(&st->last_active_data_ns, mono_ns());
                    fprintf(stderr, "halo: gapless switch -> format_id=%u (%s reopen)\n",
                            next_fid, same ? "no" : "with");
                }
                pthread_mutex_unlock(&st->state_mtx);
                continue; /* re-loop, will pick up new active idx */
            }
            pthread_mutex_unlock(&st->state_mtx);
            usleep(2000);
            continue;
        }

        size_t want = st->alsa.period_size > 0
                          ? (size_t)st->alsa.period_size * frame_size
                          : buf_bytes;
        if (want > buf_bytes) want = buf_bytes;
        if (want > usable_bytes) want = usable_bytes; /* never pull a partial trailing frame */
        size_t got = halo_ring_read(&st->ring[idx], scratch, want);
        if (got == 0) continue;

        snd_pcm_uframes_t nframes = got / frame_size; /* exact now — got is frame-aligned by construction */
        if (nframes == 0) continue;

        int underrun = 0;
        snd_pcm_sframes_t written = halo_alsa_write(&st->alsa, scratch, nframes, &underrun);

        if (underrun) {
            atomic_fetch_add(&st->underrun_count, 1);
            struct halo_underrun ev = { .format_id = fid, .mono_ns = mono_ns() };
            halo_send_message(st->sockfd, &st->send_mtx, HALO_MSG_UNDERRUN, &ev, sizeof(ev),
                               atomic_fetch_add(&st->seq_counter, 1));
        }
        if (written > 0) {
            atomic_fetch_add(&st->frames_written, (uint64_t)written);
        } else if (written < 0) {
            /* The device is gone (unplugged, or a driver error we can't
             * recover from). Tell the sender: it is still streaming audio at
             * us, and with the stream closed that audio is dropped and no
             * POSITION is ever emitted — so a sender pacing itself against
             * playback position would wait forever for a position that
             * cannot advance. Same silent-hang shape as a format we can't
             * open, so it reuses the same message. */
            fprintf(stderr, "halo: fatal ALSA error, closing stream\n");
            pthread_mutex_lock(&st->state_mtx);
            st->stream_open = 0;
            pthread_mutex_unlock(&st->state_mtx);
            send_format_rejected(st, fid, HALO_REJECT_DEVICE_BUSY);
        }
    }

    free(scratch);
    return NULL;
}

/* ---------------- Runtime state files (for a local display) ---------------- */

/* The daemon deliberately does not draw anything itself: it runs at
 * SCHED_FIFO with one job, and linking a graphics stack into the process
 * that feeds ALSA would put allocation and CPU contention on the audio path
 * and force the systemd sandbox wide open for GPU access. Instead it
 * publishes what it knows as plain files under RuntimeDirectory, so a
 * display can be any separate program in any language — and can crash,
 * restart, or be developed live without touching playback. */
/* Overridable at compile time so the offline self-test (make check) can
 * write somewhere that doesn't need root. */
#ifndef HALO_RUNTIME_DIR
#define HALO_RUNTIME_DIR "/run/halo-daemon"
#endif

/* Atomic so a reader never observes a half-written file: write to a temp
 * name, then rename (which is atomic within a filesystem). */
static int write_file_atomic(const char *name, const void *data, size_t len) {
    char final_path[256], tmp_path[256];
    snprintf(final_path, sizeof(final_path), "%s/%s", HALO_RUNTIME_DIR, name);
    snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.tmp", HALO_RUNTIME_DIR, name);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;
    int ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* ---------------- Status line ---------------- */

/* Describe a format the way a listener thinks about it, not the way the
 * wire encodes it. The wire `sample_rate` for DSD is a per-channel byte
 * rate (bit rate / 8 native, / 16 DoP) — printing that raw shows "352800"
 * for DSD64, which reads like 352.8 kHz PCM and is nobody's mental model. */
static void describe_format(const struct halo_format *fmt, char *out, size_t out_len) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) {
        double mhz = (double)fmt->sample_rate * 8.0 / 1e6;
        snprintf(out, out_len, "DSD%u native 1-bit %.3f MHz %uch",
                 64u * (fmt->dsd_rate_mult ? fmt->dsd_rate_mult : 1),
                 mhz, fmt->channels);
    } else if (fmt->is_dsd == HALO_FMT_DSD_DOP) {
        double mhz = (double)fmt->sample_rate * 16.0 / 1e6;
        snprintf(out, out_len, "DSD%u over DoP 1-bit %.3f MHz %uch",
                 64u * (fmt->dsd_rate_mult ? fmt->dsd_rate_mult : 1),
                 mhz, fmt->channels);
    } else {
        snprintf(out, out_len, "PCM %u-bit %.1f kHz %uch",
                 fmt->bits_per_sample, (double)fmt->sample_rate / 1000.0, fmt->channels);
    }
}

/* Printed on change, plus a heartbeat, rather than on a fixed tick: this
 * usually runs under systemd with journald capturing stderr, where a line
 * every second would be pure noise. Quiet when healthy, immediate when
 * something moves. */
static void report_status(halo_state_t *st, int force) {
    static char last[256];
    static uint64_t last_ns;

    pthread_mutex_lock(&st->state_mtx);
    int open_ = st->stream_open;
    int idx = st->active_idx;
    struct halo_format fmt = st->fmt[idx];
    uint32_t fid = st->format_id[idx];
    int pending = st->pending_valid;
    pthread_mutex_unlock(&st->state_mtx);

    char line[256];
    if (!open_) {
        snprintf(line, sizeof(line), "[halo] idle — no stream open");
    } else {
        char fmt_desc[96];
        describe_format(&fmt, fmt_desc, sizeof(fmt_desc));

        size_t used = halo_ring_used(&st->ring[idx]);
        unsigned ring_pct = (unsigned)((used * 100) / RING_CAPACITY);
        /* Bucketed: raw percentage jitters constantly and would defeat the
         * print-on-change filter below. */
        ring_pct = (ring_pct / 5) * 5;

        unsigned int rate = halo_alsa_current_rate(&st->alsa);
        uint64_t frames = atomic_load(&st->frames_written);
        unsigned secs = rate ? (unsigned)(frames / rate) : 0u;

        snprintf(line, sizeof(line),
                 "[halo] %s | fmt#%u%s | ring %u%% | %u:%02u:%02u | xrun %llu%s",
                 fmt_desc, fid, pending ? " (+pending)" : "",
                 ring_pct, secs / 3600, (secs / 60) % 60, secs % 60,
                 (unsigned long long)atomic_load(&st->underrun_count),
                 atomic_load(&st->paused) ? " | PAUSED" : "");
    }

    uint64_t now = mono_ns();
    int changed = strcmp(line, last) != 0;
    int heartbeat = (now - last_ns) > 10ull * 1000000000ull;
    if (!force && !changed && !heartbeat) return;

    snprintf(last, sizeof(last), "%s", line);
    last_ns = now;
    fprintf(stderr, "%s\n", line);

    /* Same information as the log line, in a form a display can parse.
     * Written on the same change-or-heartbeat cadence, so a display polling
     * this file (or watching it with inotify) sees every transition without
     * the daemon doing extra work when nothing is happening. */
    char json[512];
    int n;
    if (!open_) {
        n = snprintf(json, sizeof(json), "{\"state\":\"idle\"}\n");
    } else {
        char fmt_desc[96];
        describe_format(&fmt, fmt_desc, sizeof(fmt_desc));
        unsigned int rate = halo_alsa_current_rate(&st->alsa);
        uint64_t frames = atomic_load(&st->frames_written);
        size_t used = halo_ring_used(&st->ring[idx]);
        n = snprintf(json, sizeof(json),
                     "{\"state\":\"%s\",\"format\":\"%s\",\"format_id\":%u,"
                     "\"is_dsd\":%u,\"channels\":%u,\"wire_sample_rate\":%u,"
                     "\"alsa_rate\":%u,\"position_frames\":%llu,"
                     "\"position_seconds\":%.3f,\"ring_percent\":%u,"
                     "\"underruns\":%llu}\n",
                     atomic_load(&st->paused) ? "paused" : "playing",
                     fmt_desc, fid, fmt.is_dsd, fmt.channels, fmt.sample_rate,
                     rate, (unsigned long long)frames,
                     rate ? (double)frames / (double)rate : 0.0,
                     (unsigned)((used * 100) / RING_CAPACITY),
                     (unsigned long long)atomic_load(&st->underrun_count));
    }
    if (n > 0) write_file_atomic("status.json", json, (size_t)n);
}

/* ---------------- Position reporting thread ---------------- */

static void *position_thread(void *arg) {
    halo_state_t *st = (halo_state_t *)arg;
    while (atomic_load(&st->running)) {
        usleep(200000);
        report_status(st, 0);
        pthread_mutex_lock(&st->state_mtx);
        int stream_open = st->stream_open;
        uint32_t fid = st->format_id[st->active_idx];
        pthread_mutex_unlock(&st->state_mtx);
        if (!stream_open) continue;

        struct halo_position pos = {
            .format_id = fid,
            .frames_written = atomic_load(&st->frames_written),
            .mono_ns = mono_ns(),
        };
        halo_send_message(st->sockfd, &st->send_mtx, HALO_MSG_POSITION, &pos, sizeof(pos),
                           atomic_fetch_add(&st->seq_counter, 1));
    }
    return NULL;
}

/* ---------------- Control message handling (network thread) ---------------- */

/* Publish what the DAC can actually do, so a display can show "PCM up to
 * 768k / native DSD256" without asking the sender — the sender only knows
 * what it was told, whereas this comes straight from the device's own
 * hw_params. Written at startup (so it exists with no client connected) and
 * refreshed on every HELLO, which is when the device is re-queried. */
static void write_caps_json(const char *device, const struct halo_caps *c) {
    char rates[64] = "";
    size_t n = 0;
    static const struct { unsigned bit; const char *name; } dsd_rates[] = {
        { 0, "DSD64" }, { 1, "DSD128" }, { 2, "DSD256" }, { 3, "DSD512" },
    };
    for (size_t i = 0; i < sizeof(dsd_rates) / sizeof(dsd_rates[0]); i++) {
        if (c->supported_dsd_rates_mask & (1u << dsd_rates[i].bit)) {
            n += (size_t)snprintf(rates + n, sizeof(rates) - n, "%s\"%s\"",
                                  n ? "," : "", dsd_rates[i].name);
        }
    }

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"device\":\"%s\",\"max_sample_rate_pcm\":%u,"
        "\"max_bits_per_sample\":%u,\"max_channels\":%u,"
        "\"supports_native_dsd\":%s,\"supports_dop\":%s,"
        "\"dsd_rates\":[%s],\"feature_flags\":%u}\n",
        device, c->max_sample_rate_pcm, c->max_bits_per_sample, c->max_channels,
        c->supports_native_dsd ? "true" : "false",
        c->supports_dop ? "true" : "false",
        rates, c->feature_flags);
    if (len > 0) write_file_atomic("caps.json", json, (size_t)len);
}

/* Tell the sender we could not open this format. Without it the failure is
 * invisible: the audio for that format_id belongs to no open stream and is
 * dropped, no POSITION is emitted because nothing is playing, and a sender
 * pacing itself against playback position waits forever for a position that
 * will never move. The user sees a play button that does nothing. */
static void send_format_rejected(halo_state_t *st, uint32_t format_id, uint32_t reason) {
    struct halo_format_rejected msg = { .format_id = format_id, .reason = reason };
    halo_send_message(st->sockfd, &st->send_mtx, HALO_MSG_FORMAT_REJECTED,
                       &msg, sizeof(msg), atomic_fetch_add(&st->seq_counter, 1));
    fprintf(stderr, "halo: rejected format_id=%u (reason %u)\n", format_id, reason);
}

static void handle_hello(halo_state_t *st) {
    struct halo_caps caps;
    if (halo_alsa_query_caps(st->alsa_device, &caps) != 0) {
        /* Probing failed — most often because something else holds the
         * device (another player mid-playback), or it was unplugged.
         *
         * The old code ignored this and sent the all-zero struct, which
         * says "supports nothing": the sender then refused *every* track,
         * PCM included, with a message blaming its own DSD setting. That is
         * both wrong and unactionable. Advertise a permissive fallback
         * instead and let the real open decide — a format that genuinely
         * can't be played now answers FORMAT_REJECTED with a reason, which
         * is a truthful and specific failure rather than a blanket one. */
        fprintf(stderr, "halo: WARNING — could not probe %s (device busy or absent). "
                        "Advertising permissive capabilities; individual formats will be "
                        "rejected at open time if unsupported.\n", st->alsa_device);
        memset(&caps, 0, sizeof(caps));
        caps.max_sample_rate_pcm = 768000;
        caps.max_bits_per_sample = 32;
        caps.max_channels = 2;
        caps.supports_native_dsd = 0; /* unknown: don't claim what we couldn't verify */
        caps.supported_dsd_rates_mask = 0;
    }
    /* Advertise what this build actually implements, so a sender can tell
     * us apart from an original v1 daemon (which sends a 16-byte CAPS with
     * no flags word at all, and must keep working). */
    caps.feature_flags = HALO_FEATURE_FORMAT_REJECTED
                       | HALO_FEATURE_PING
                       | HALO_FEATURE_SKIPS_UNKNOWN;
    write_caps_json(st->alsa_device, &caps);
    halo_send_message(st->sockfd, &st->send_mtx, HALO_MSG_CAPS, &caps, sizeof(caps),
                       atomic_fetch_add(&st->seq_counter, 1));
}

static void handle_format(halo_state_t *st, const struct halo_format *fmt) {
    pthread_mutex_lock(&st->state_mtx);

    if (!st->stream_open) {
        /* First format of the connection: open for real, right away. */
        st->active_idx = 0;
        st->fmt[0] = *fmt;
        st->format_id[0] = fmt->format_id;
        halo_ring_clear(&st->ring[0]);
        pthread_mutex_unlock(&st->state_mtx);

        if (halo_alsa_open(&st->alsa, st->alsa_device, fmt) != 0) {
            fprintf(stderr, "halo: initial ALSA open failed for requested format\n");
            send_format_rejected(st, fmt->format_id, HALO_REJECT_DEVICE_BUSY);
            return;
        }
        pthread_mutex_lock(&st->state_mtx);
        st->stream_open = 1;
        atomic_store(&st->frames_written, 0);
        atomic_store(&st->paused, 0); /* fresh active stream implies intent to play */
        atomic_store(&st->last_active_data_ns, mono_ns());
        pthread_mutex_unlock(&st->state_mtx);
        fprintf(stderr, "halo: stream opened, format_id=%u\n", fmt->format_id);
        return;
    }

    if (fmt->is_preannounce) {
        if (st->pending_valid) {
            /* Sender preannounced a second track before the first
             * preannounce's handover actually happened. Only one pending
             * slot exists (see PROTOCOL.md), so this overwrites the
             * previous preannounce's format/ring — that track's buffered
             * lookahead data is silently lost from the daemon's point of
             * view. This is a sender-side pacing bug, not something the
             * daemon can recover gracefully from, so make it loud instead
             * of a quiet audio glitch downstream. */
            fprintf(stderr, "halo: WARNING — preannounce format_id=%u arrived while "
                            "format_id=%u was still pending (not yet switched to); "
                            "overwriting. Sender is preannouncing more than one track "
                            "ahead, which this protocol/daemon does not support.\n",
                    fmt->format_id, st->format_id[1 - st->active_idx]);
        }
        int other = 1 - st->active_idx;
        halo_ring_clear(&st->ring[other]);
        st->fmt[other] = *fmt;
        st->format_id[other] = fmt->format_id;
        st->pending_valid = 1;
        pthread_mutex_unlock(&st->state_mtx);
        fprintf(stderr, "halo: preannounce received, format_id=%u\n", fmt->format_id);
        return;
    }

    /* A non-preannounce FORMAT arriving mid-stream (not the very first one)
     * is a hard cut to a brand-new active stream (PROTOCOL.md,
     * "Non-preannounce FORMAT mid-stream"): the sender uses it for manual
     * skips and fresh-track starts, so whatever is still buffered for the
     * old stream — and any pending preannounce slot — belongs to an
     * abandoned playback plan and gets discarded, not drained. Staging it
     * in the other ring with switch_requested set reuses the writer
     * thread's switch machinery (the active ring was just cleared, so it
     * flips essentially immediately, reopening ALSA only if the format
     * actually differs). Note ALSA's own already-submitted buffer still
     * drains (~a period or two); dropping that too would need
     * snd_pcm_drop, which FLUSH does — acceptable for a track cut.
     * frames_written resets at the flip (new position epoch), and paused
     * state clears: announcing a fresh active stream is an unambiguous
     * statement of intent to play it. */
    int other = 1 - st->active_idx;
    halo_ring_clear(&st->ring[st->active_idx]);
    halo_ring_clear(&st->ring[other]);
    st->fmt[other] = *fmt;
    st->format_id[other] = fmt->format_id;
    st->pending_valid = 1;
    st->switch_requested = 1;
    atomic_store(&st->paused, 0);
    atomic_store(&st->last_active_data_ns, mono_ns());
    pthread_mutex_unlock(&st->state_mtx);
    fprintf(stderr, "halo: hard-cut FORMAT mid-stream, format_id=%u\n", fmt->format_id);
}

static void handle_switch_to_pending(halo_state_t *st, const struct halo_switch_to_pending *msg) {
    pthread_mutex_lock(&st->state_mtx);
    int other = 1 - st->active_idx;
    if (st->pending_valid && msg->format_id == st->format_id[other]) {
        /* The sender's gapless commit: no more AUDIO_DATA is coming for
         * the active stream. The writer thread performs the actual flip
         * once the active ring drains — that drain *is* the "old track's
         * tail finishes playing" semantics PROTOCOL.md requires. */
        st->switch_requested = 1;
        pthread_mutex_unlock(&st->state_mtx);
        fprintf(stderr, "halo: SWITCH_TO_PENDING format_id=%u (will flip when active ring drains)\n",
                msg->format_id);
        return;
    }
    pthread_mutex_unlock(&st->state_mtx);
    /* Not an error — benign race, same contract as CANCEL_PREANNOUNCE. */
    fprintf(stderr, "halo: SWITCH_TO_PENDING format_id=%u ignored (not the current pending slot)\n",
            msg->format_id);
}

static void handle_audio_data(halo_state_t *st, uint32_t payload_len, uint64_t seq, int fd) {
    struct halo_audio_data_hdr hdr;
    if (halo_read_full(fd, &hdr, sizeof(hdr)) < 0) {
        atomic_store(&st->running, 0);
        return;
    }
    uint32_t remaining = payload_len - (uint32_t)sizeof(hdr);

    pthread_mutex_lock(&st->state_mtx);
    int target_idx = -1;
    /* Pre-flush audio barrier: the sender writes FLUSH straight to the
     * socket while already-queued AUDIO_DATA is still draining, so bytes
     * written before the flush can arrive after it. They still carry the
     * pre-seek format_id, which is still active here until the new FORMAT
     * lands, so without this they would be played at the new position (a
     * brief fragment of the old one). seq is monotonic in send order, so
     * "sent before the FLUSH" is exactly seq < flush_barrier_seq. The
     * payload is still drained from the socket below, just not buffered. */
    if (seq < st->flush_barrier_seq) {
        target_idx = -1;
    } else if (st->stream_open && hdr.format_id == st->format_id[st->active_idx]) {
        target_idx = st->active_idx;
        atomic_store(&st->last_active_data_ns, mono_ns());
    } else if (st->pending_valid && hdr.format_id == st->format_id[1 - st->active_idx]) {
        target_idx = 1 - st->active_idx;
    }
    pthread_mutex_unlock(&st->state_mtx);

    while (remaining > 0) {
        if (target_idx < 0) {
            /* Unknown or superseded format_id (pre-flush audio, a cancelled
             * preannounce): the bytes still have to be taken off the socket
             * to stay framed, they just never reach a ring. */
            uint8_t discard[16384];
            uint32_t want = remaining < sizeof(discard) ? remaining : (uint32_t)sizeof(discard);
            if (halo_read_full(fd, discard, want) < 0) {
                atomic_store(&st->running, 0);
                return;
            }
            remaining -= want;
            continue;
        }

        /* recv() straight into the ring's writable span: one copy
         * (kernel -> ring) instead of two (kernel -> stack -> ring). Safe
         * without extra locking because this thread is the ring's only
         * producer — and it is also the thread that runs handle_flush(), so
         * a clear can never land in the middle of one of these writes. */
        uint8_t *dst = NULL;
        size_t space = halo_ring_writable(&st->ring[target_idx], &dst);
        if (space == 0) {
            usleep(1000); /* ring full: back off instead of dropping audio */
            continue;
        }
        uint32_t want = remaining < space ? remaining : (uint32_t)space;
        if (halo_read_full(fd, dst, want) < 0) {
            atomic_store(&st->running, 0);
            return;
        }
        halo_ring_commit_write(&st->ring[target_idx], want);
        remaining -= want;
    }
}

static void handle_cancel_preannounce(halo_state_t *st, const struct halo_cancel_preannounce *msg) {
    pthread_mutex_lock(&st->state_mtx);
    int other = 1 - st->active_idx;
    if (st->pending_valid && msg->format_id == st->format_id[other]) {
        halo_ring_clear(&st->ring[other]);
        st->pending_valid = 0;
        pthread_mutex_unlock(&st->state_mtx);
        fprintf(stderr, "halo: preannounce format_id=%u cancelled by sender\n", msg->format_id);
        return;
    }
    pthread_mutex_unlock(&st->state_mtx);
    /* Not an error — sender and daemon can race here (already promoted, or
     * a newer preannounce already overwrote the slot this cancel was for).
     * See PROTOCOL.md's "Cancelling a preannounce" note. */
    fprintf(stderr, "halo: cancel for format_id=%u ignored (not the current pending slot)\n",
            msg->format_id);
}

/* Album/track info the daemon maintains for a local display: one fixed,
 * preallocated block rather than per-message allocation. Cover art is up to
 * 4 MiB and senders may re-send it every track, so malloc/free churn of
 * multi-megabyte buffers in the message loop is worth designing out — this
 * is sized once from the protocol caps and then never grows.
 *
 * It lives in BSS (zero-filled, costs no resident memory until touched) and
 * is the daemon's authoritative copy: the files under /run mirror it for
 * easy consumption, but a display served directly by the daemon later would
 * read this instead. Guarded by its own mutex, deliberately not state_mtx —
 * nothing here is on the audio path and it must never contend with it. */
typedef struct {
    pthread_mutex_t mtx;
    uint32_t metadata_len;
    char     metadata[HALO_METADATA_MAX_BYTES];
    uint32_t coverart_len;
    uint8_t  coverart_sha256[32];
    int      has_coverart;
    uint8_t  coverart[HALO_COVERART_MAX_BYTES];
} halo_media_t;

static halo_media_t g_media = { .mtx = PTHREAD_MUTEX_INITIALIZER };

static void media_sha_hex(const uint8_t sha[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", sha[i]);
}

/* Always consume the whole payload even when discarding it — the stream is
 * a single framed channel, so leaving bytes unread desynchronises every
 * message after this one. */
static int drain_payload(halo_state_t *st, uint32_t remaining, int fd) {
    uint8_t discard[4096];
    while (remaining > 0) {
        uint32_t want = remaining < sizeof(discard) ? remaining : (uint32_t)sizeof(discard);
        if (halo_read_full(fd, discard, want) < 0) {
            atomic_store(&st->running, 0);
            return -1;
        }
        remaining -= want;
    }
    return 0;
}

static void handle_metadata(halo_state_t *st, uint32_t len, int fd) {
    if (len == 0 || len > HALO_METADATA_MAX_BYTES) {
        if (len > 0) {
            fprintf(stderr, "halo: METADATA %u bytes exceeds cap, discarding\n", len);
            drain_payload(st, len, fd);
        }
        return;
    }
    pthread_mutex_lock(&g_media.mtx);
    if (halo_read_full(fd, g_media.metadata, len) < 0) {
        g_media.metadata_len = 0;
        pthread_mutex_unlock(&g_media.mtx);
        atomic_store(&st->running, 0);
        return;
    }
    g_media.metadata_len = len;
    /* Stored verbatim — the daemon has no opinion on the contents beyond
     * PROTOCOL.md's schema being the sender's contract with the display. */
    write_file_atomic("metadata.json", g_media.metadata, len);
    pthread_mutex_unlock(&g_media.mtx);
}

static void handle_coverart(halo_state_t *st, uint32_t len, int fd) {
    struct halo_coverart_hdr chdr;
    if (len < sizeof(chdr)) { drain_payload(st, len, fd); return; }
    if (halo_read_full(fd, &chdr, sizeof(chdr)) < 0) {
        atomic_store(&st->running, 0);
        return;
    }
    uint32_t remaining = len - (uint32_t)sizeof(chdr);

    if (remaining > HALO_COVERART_MAX_BYTES) {
        fprintf(stderr, "halo: COVERART %u bytes exceeds cap, discarding\n", remaining);
        drain_payload(st, remaining, fd);
        return;
    }

    /* Unchanged art is dropped without re-reading it into the block or
     * rewriting the file: senders may resend the same album cover on every
     * track. The payload is still drained — framing depends on it. */
    pthread_mutex_lock(&g_media.mtx);
    int same = g_media.has_coverart &&
               g_media.coverart_len == remaining &&
               memcmp(g_media.coverart_sha256, chdr.sha256, 32) == 0;
    pthread_mutex_unlock(&g_media.mtx);
    if (same) { drain_payload(st, remaining, fd); return; }

    pthread_mutex_lock(&g_media.mtx);
    if (halo_read_full(fd, g_media.coverart, remaining) < 0) {
        g_media.has_coverart = 0;
        g_media.coverart_len = 0;
        pthread_mutex_unlock(&g_media.mtx);
        atomic_store(&st->running, 0);
        return;
    }
    memcpy(g_media.coverart_sha256, chdr.sha256, 32);
    g_media.coverart_len = remaining;
    g_media.has_coverart = 1;

    char hex[65];
    media_sha_hex(chdr.sha256, hex);
    if (write_file_atomic("coverart.bin", g_media.coverart, remaining) == 0) {
        char line[68];
        int n = snprintf(line, sizeof(line), "%s\n", hex);
        write_file_atomic("coverart.sha256", line, (size_t)n);
        fprintf(stderr, "halo: cover art updated (%u bytes, %.16s...)\n", remaining, hex);
    }
    pthread_mutex_unlock(&g_media.mtx);
}

static void handle_flush(halo_state_t *st) {
    pthread_mutex_lock(&st->state_mtx);
    halo_ring_clear(&st->ring[st->active_idx]);
    st->pending_valid = 0;
    st->switch_requested = 0;
    pthread_mutex_unlock(&st->state_mtx);

    halo_alsa_drop_and_prepare(&st->alsa);
    atomic_store(&st->frames_written, 0);
    /* FLUSH clears paused state per PROTOCOL.md's pause rules — the sender
     * re-pauses explicitly if it was seeking while paused. */
    atomic_store(&st->paused, 0);

    halo_send_message(st->sockfd, &st->send_mtx, HALO_MSG_FLUSH_ACK, NULL, 0,
                       atomic_fetch_add(&st->seq_counter, 1));
}

/* ---------------- Network reader loop (main thread per connection) ---------------- */

static void connection_loop(halo_state_t *st) {
    while (atomic_load(&st->running)) {
        struct halo_header hdr;
        if (halo_read_full(st->sockfd, &hdr, HALO_HEADER_SIZE) < 0) break;
        if (hdr.magic != HALO_MAGIC) {
            fprintf(stderr, "halo: bad magic, dropping connection\n");
            break;
        }

        switch (hdr.type) {
            case HALO_MSG_HELLO: {
                struct halo_hello h;
                if (halo_read_full(st->sockfd, &h, sizeof(h)) < 0) goto done;
                fprintf(stderr, "halo: HELLO proto_version=%u\n", h.proto_version);
                handle_hello(st);
                break;
            }
            case HALO_MSG_FORMAT: {
                struct halo_format f;
                if (halo_read_full(st->sockfd, &f, sizeof(f)) < 0) goto done;
                handle_format(st, &f);
                break;
            }
            case HALO_MSG_AUDIO_DATA:
                handle_audio_data(st, hdr.length, hdr.seq, st->sockfd);
                break;
            case HALO_MSG_FLUSH:
                pthread_mutex_lock(&st->state_mtx);
                st->flush_barrier_seq = hdr.seq;
                pthread_mutex_unlock(&st->state_mtx);
                handle_flush(st);
                break;
            case HALO_MSG_CANCEL_PREANNOUNCE: {
                struct halo_cancel_preannounce c;
                if (halo_read_full(st->sockfd, &c, sizeof(c)) < 0) goto done;
                handle_cancel_preannounce(st, &c);
                break;
            }
            case HALO_MSG_SWITCH_TO_PENDING: {
                struct halo_switch_to_pending s;
                if (halo_read_full(st->sockfd, &s, sizeof(s)) < 0) goto done;
                handle_switch_to_pending(st, &s);
                break;
            }
            case HALO_MSG_PAUSE:
                atomic_store(&st->paused, 1);
                break;
            case HALO_MSG_RESUME:
                atomic_store(&st->paused, 0);
                break;
            case HALO_MSG_METADATA:
                handle_metadata(st, hdr.length, st->sockfd);
                break;
            case HALO_MSG_COVERART:
                handle_coverart(st, hdr.length, st->sockfd);
                break;
            case HALO_MSG_BYE:
                goto done;
            case HALO_MSG_PING:
                /* Liveness probe — a paused session is otherwise completely
                 * silent, so neither end notices a dead link until TCP
                 * eventually gives up minutes later. */
                halo_send_message(st->sockfd, &st->send_mtx, HALO_MSG_PONG, NULL, 0,
                                   atomic_fetch_add(&st->seq_counter, 1));
                break;
            case HALO_MSG_PONG:
                break; /* we don't currently initiate PINGs; accept them anyway */
            default:
                /* Skip, don't disconnect. Framing is self-describing, so an
                 * unrecognised type is harmless to step over — whereas
                 * treating it as fatal makes every future message a breaking
                 * change (which is exactly what 0x0E/0x0F were to the
                 * original implementations). */
                fprintf(stderr, "halo: unknown message type 0x%x (%u bytes), skipping\n",
                        hdr.type, hdr.length);
                if (drain_payload(st, hdr.length, st->sockfd) < 0) goto done;
                break;
        }
    }
done:
    return;
}

/* ---------------- signal handling (clean systemd stop) ---------------- */

static volatile sig_atomic_t g_shutdown = 0;
static int g_listen_fd = -1;

static void on_shutdown_signal(int signo) {
    (void)signo;
    g_shutdown = 1;
    /* accept() is blocking with no SA_RESTART below, so it wakes up with
     * EINTR on its own; closing the listen fd here as well is a belt-and-
     * suspenders way to unstick it immediately even if some libc restarts
     * syscalls by default. */
    if (g_listen_fd >= 0) close(g_listen_fd);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : "hw:0,0";
    int port = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;

    halo_state_t st;
    memset(&st, 0, sizeof(st));
    strncpy(st.alsa_device, device, sizeof(st.alsa_device) - 1);
    pthread_mutex_init(&st.send_mtx, NULL);
    pthread_mutex_init(&st.state_mtx, NULL);

    if (halo_ring_init(&st.ring[0], RING_CAPACITY) != 0 ||
        halo_ring_init(&st.ring[1], RING_CAPACITY) != 0) {
        fprintf(stderr, "halo: failed to allocate ring buffers\n");
        return 1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 1) < 0) { perror("listen"); return 1; }

    g_listen_fd = listen_fd;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_shutdown_signal;
    sa.sa_flags = 0; /* deliberately no SA_RESTART: we want accept()/recv() to wake with EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* systemd creates this via RuntimeDirectory=, but the daemon is also
     * runnable by hand for testing; mkdir here so the state files work
     * either way. */
    if (mkdir(HALO_RUNTIME_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "halo: could not create %s (%s) — display state files disabled\n",
                HALO_RUNTIME_DIR, strerror(errno));
    }

    {   /* Publish capabilities before any client connects, so a display can
         * show the DAC's abilities on a freshly booted, idle endpoint. */
        struct halo_caps boot_caps;
        memset(&boot_caps, 0, sizeof(boot_caps));
        if (halo_alsa_query_caps(device, &boot_caps) == 0) {
            boot_caps.feature_flags = HALO_FEATURE_FORMAT_REJECTED
                                    | HALO_FEATURE_PING
                                    | HALO_FEATURE_SKIPS_UNKNOWN;
            write_caps_json(device, &boot_caps);
        }
    }

    fprintf(stderr, "halo: listening on port %d, ALSA device %s\n", port, device);

    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            if (g_shutdown) break;
            perror("accept");
            continue;
        }

        setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        /* Keep an idle session alive and detect a vanished sender. A paused
         * client sends nothing for as long as the user leaves it paused, and
         * consumer routers routinely reap idle NAT/conntrack entries after a
         * few minutes — without probes the connection is silently dead and
         * this daemon would hold it until the next write failed, refusing new
         * clients in the meantime. Values mirror the sender's side. */
        setsockopt(conn_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
        { int v = 15; setsockopt(conn_fd, IPPROTO_TCP, TCP_KEEPIDLE,  &v, sizeof(v)); }
        { int v = 5;  setsockopt(conn_fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v)); }
        { int v = 4;  setsockopt(conn_fd, IPPROTO_TCP, TCP_KEEPCNT,   &v, sizeof(v)); }
#endif
        fprintf(stderr, "halo: client connected from %s\n", inet_ntoa(client_addr.sin_addr));

        st.sockfd = conn_fd;
        st.active_idx = 0;
        st.pending_valid = 0;
        st.switch_requested = 0;
        st.stream_open = 0;
        /* seq restarts per connection, so a stale barrier from the
         * previous client would drop everything from the new one. */
        st.flush_barrier_seq = 0;
        atomic_store(&st.paused, 0);
        atomic_store(&st.running, 1);
        atomic_store(&st.frames_written, 0);
        atomic_store(&st.underrun_count, 0);
        atomic_store(&st.last_active_data_ns, mono_ns());
        halo_ring_clear(&st.ring[0]);
        halo_ring_clear(&st.ring[1]);

        pthread_t writer_tid, pos_tid;
        pthread_create(&writer_tid, NULL, alsa_writer_thread, &st);
        pthread_create(&pos_tid, NULL, position_thread, &st);

        connection_loop(&st);

        atomic_store(&st.running, 0);
        pthread_join(writer_tid, NULL);
        pthread_join(pos_tid, NULL);

        if (st.alsa.is_open) halo_alsa_close(&st.alsa);
        close(conn_fd);
        if (!g_shutdown) {
            fprintf(stderr, "halo: client disconnected, waiting for next connection\n");
        }
    }

    fprintf(stderr, "halo: shutdown signal received, exiting cleanly\n");
    return 0;
}
