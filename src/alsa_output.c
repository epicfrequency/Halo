#define _DEFAULT_SOURCE /* alloca() via alsa's snd_pcm_hw_params_alloca under -std=c11 */
#include "alsa_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

/* The DSD packing a device wants is discovered at runtime, never chosen at
 * build time. Different DACs disagree (the Gustard here takes DSD_U32_BE
 * only; plenty of XMOS/Amanero designs take DSD_U32_LE; some older ones only
 * DSD_U8), and a compile-time choice means one binary per DAC — which makes
 * a prebuilt package useless and silently mis-plays if the wrong one is
 * deployed. This is only the starting guess for the probe order. */
#define HALO_DSD_FIRST_GUESS SND_PCM_FORMAT_DSD_U32_BE

/* Every packing this daemon can drive, most capable first. DSD_U32 reaches
 * the highest bit rates for a given ALSA frame rate, so it is preferred when
 * the device accepts more than one. */
static const snd_pcm_format_t k_dsd_candidates[] = {
    SND_PCM_FORMAT_DSD_U32_BE, SND_PCM_FORMAT_DSD_U32_LE,
    SND_PCM_FORMAT_DSD_U16_BE, SND_PCM_FORMAT_DSD_U16_LE,
    SND_PCM_FORMAT_DSD_U8,
};
#define HALO_DSD_CANDIDATE_COUNT \
    (sizeof(k_dsd_candidates) / sizeof(k_dsd_candidates[0]))

static size_t dsd_alsa_bytes_per_channel_frame(void) {
    switch (halo_alsa_dsd_format()) {
        case SND_PCM_FORMAT_DSD_U8:     return 1;
        case SND_PCM_FORMAT_DSD_U16_LE:
        case SND_PCM_FORMAT_DSD_U16_BE: return 2;
        case SND_PCM_FORMAT_DSD_U32_LE:
        case SND_PCM_FORMAT_DSD_U32_BE: return 4;
        default:                        return 4;
    }
}

size_t halo_alsa_wire_ticks_per_frame(const struct halo_format *fmt) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) {
        return dsd_alsa_bytes_per_channel_frame();
    }
    return 1;
}

size_t halo_format_frame_size(const struct halo_format *fmt) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) {
        /* One *wire tick*: one DSD byte per channel. This deliberately does
         * not track the device's packing width. The wire's sample_rate for
         * native DSD is bit_rate/8 — a per-channel byte rate — so a tick has
         * to be one byte per channel for position and rate arithmetic to
         * agree with the sender. Returning the ALSA frame size here instead
         * (4 bytes/channel under DSD_U32) made frames_written report a
         * quarter of the real elapsed time. The packing conversion belongs
         * in halo_alsa_write, not in the unit the protocol counts in. */
        return (size_t)fmt->channels;
    }
    if (fmt->is_dsd == HALO_FMT_DSD_DOP) {
        /* DoP container: not fully wired up yet (marker byte interleaving
         * TODO — see README "DoP is a stub" note). Assumes 32-bit
         * container per channel for now. */
        return (size_t)fmt->channels * 4;
    }
    switch (fmt->bits_per_sample) {
        case 16: case 24: case 32:
            return (size_t)fmt->channels * (fmt->bits_per_sample / 8);
        default:
            return (size_t)fmt->channels * 4;
    }
}

static unsigned int alsa_rate_for(const struct halo_format *fmt);

unsigned int halo_alsa_current_rate(const halo_alsa_ctx_t *ctx) {
    return ctx->is_open ? alsa_rate_for(&ctx->fmt) : 0u;
}

/* Set by halo_alsa_query_caps from what the device actually accepted; the
 * compile-time HALO_DSD_ALSA_FORMAT is only the fallback for when probing
 * never ran or found nothing. */
static snd_pcm_format_t g_detected_dsd_format = HALO_DSD_FIRST_GUESS;
static int g_dsd_format_detected = 0;

void halo_alsa_set_detected_dsd_format(snd_pcm_format_t fmt) {
    g_detected_dsd_format = fmt;
    g_dsd_format_detected = 1;
}

snd_pcm_format_t halo_alsa_dsd_format(void) {
    return g_dsd_format_detected ? g_detected_dsd_format : (snd_pcm_format_t)HALO_DSD_FIRST_GUESS;
}

static snd_pcm_format_t pcm_format_for(const struct halo_format *fmt) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) return halo_alsa_dsd_format();
    if (fmt->is_dsd == HALO_FMT_DSD_DOP)    return SND_PCM_FORMAT_S32_LE;
    switch (fmt->bits_per_sample) {
        case 16: return SND_PCM_FORMAT_S16_LE;
        /* The sender packs 24-bit as three bytes per sample (see
         * halo_format_frame_size, which agrees), so the ALSA format must be
         * S24_3LE. S24_LE is a *four*-byte container holding 24 bits — a
         * different layout entirely, and picking it made the declared frame
         * size and the actual one disagree. Many USB DACs expose neither and
         * offer only S32_LE, which is why open() is retried across
         * alternatives below rather than trusting one mapping. */
        case 24: return SND_PCM_FORMAT_S24_3LE;
        case 32: return SND_PCM_FORMAT_S32_LE;
        default: return SND_PCM_FORMAT_S32_LE;
    }
}

/* Wire `sample_rate` for DSD is defined as (dsd_bit_rate / 8) — i.e. a
 * per-channel byte rate. Convert that to the ALSA "frame rate" for
 * whichever DSD packing width we're using. */
static unsigned int alsa_rate_for(const struct halo_format *fmt) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) {
        size_t bpf = dsd_alsa_bytes_per_channel_frame();
        return (unsigned int)(fmt->sample_rate / bpf);
    }
    if (fmt->is_dsd == HALO_FMT_DSD_DOP) {
        /* DoP packs 2 bytes of DSD payload per 24-bit word (see
         * PROTOCOL.md); unverified without real DoP hardware, treat as
         * approximate until tested. */
        return (unsigned int)(fmt->sample_rate / 2);
    }
    return fmt->sample_rate;
}

int halo_alsa_query_caps(const char *device_name, struct halo_caps *caps_out) {
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;
    int err;

    memset(caps_out, 0, sizeof(*caps_out));

    err = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "halo: snd_pcm_open(%s) failed: %s\n", device_name, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_alloca(&params);
    err = snd_pcm_hw_params_any(pcm, params);
    if (err < 0) {
        fprintf(stderr, "halo: hw_params_any failed: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return -1;
    }

    unsigned int rate_max = 0;
    snd_pcm_hw_params_get_rate_max(params, &rate_max, NULL);
    caps_out->max_sample_rate_pcm = rate_max;

    unsigned int ch_max = 0;
    snd_pcm_hw_params_get_channels_max(params, &ch_max);
    caps_out->max_channels = (uint8_t)(ch_max > 255 ? 255 : ch_max);

    /* bits: probe common PCM formats, report the widest supported */
    if (snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_S32_LE) == 0) {
        caps_out->max_bits_per_sample = 32;
    } else if (snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_S24_LE) == 0) {
        caps_out->max_bits_per_sample = 24;
    } else {
        caps_out->max_bits_per_sample = 16;
    }

    /* native DSD probe — best effort, not all drivers report this
     * reliably; override in config if autodetection disagrees with
     * `aplay --dump-hw-params` reality on your actual hardware. */
    uint32_t dsd_mask = 0;
    int any_dsd = 0;
    /* Probe every DSD packing ALSA defines, not just the little-endian ones.
     * XMOS-based DACs (Gustard, Topping, many others) commonly expose only
     * DSD_U32_BE, and testing just the LE variants made those report "no
     * native DSD at all" — the sender then refused every DSD track with a
     * message blaming its own settings. Whichever the device accepts is
     * remembered and used for playback, so the format follows the hardware
     * instead of a compile-time guess. */
    for (size_t i = 0; i < HALO_DSD_CANDIDATE_COUNT; i++) {
        if (snd_pcm_hw_params_test_format(pcm, params, k_dsd_candidates[i]) == 0) {
            any_dsd = 1;
            halo_alsa_set_detected_dsd_format(k_dsd_candidates[i]);
            fprintf(stderr, "halo: device accepts native DSD as %s\n",
                    snd_pcm_format_name(k_dsd_candidates[i]));
            break;
        }
    }
    if (any_dsd) {
        /* Which DSD multiples fit under the device's max rate.
         *
         * ALSA states a DSD stream's rate in *frames*, and one frame carries
         * 8 DSD bits per byte of the packing width: DSD_U32 therefore runs at
         * bit_rate/32, DSD_U16 at bit_rate/16, DSD_U8 at bit_rate/8. The
         * previous fixed "/4" matched none of those and understated every
         * device — a DAC reporting rate_max 768000 (good for DSD512 in U32
         * packing) came back advertising DSD64 only. Derive the divisor from
         * the packing actually detected above. */
        size_t bytes_per_frame = dsd_alsa_bytes_per_channel_frame();
        unsigned int bits_per_frame = (unsigned int)(bytes_per_frame * 8);
        static const struct { unsigned long bit_rate; unsigned bit; } dsd_rates[] = {
            { 2822400u,  0 }, /* DSD64  */
            { 5644800u,  1 }, /* DSD128 */
            { 11289600u, 2 }, /* DSD256 */
            { 22579200u, 3 }, /* DSD512 */
        };
        for (size_t i = 0; i < sizeof(dsd_rates) / sizeof(dsd_rates[0]); i++) {
            if (bits_per_frame > 0 &&
                rate_max >= (unsigned int)(dsd_rates[i].bit_rate / bits_per_frame)) {
                dsd_mask |= (1u << dsd_rates[i].bit);
            }
        }
    }
    caps_out->supports_native_dsd = (uint8_t)any_dsd;
    caps_out->supports_dop = 0; /* not implemented yet, see README */
    caps_out->supported_dsd_rates_mask = dsd_mask;

    snd_pcm_close(pcm);
    return 0;
}

/* One attempt with one specific ALSA format. Split out from open_locked so
 * native DSD can walk its candidate packings here rather than trusting a
 * single guess: the rate depends on the packing width, so a candidate cannot
 * be evaluated without going through the whole sequence. */
static int open_with_format(halo_alsa_ctx_t *ctx, const char *device_name,
                             const struct halo_format *fmt,
                             snd_pcm_format_t forced_dsd_format, int quiet) {
    int err;
    snd_pcm_hw_params_t *params = NULL;

    err = snd_pcm_open(&ctx->pcm, device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "halo: snd_pcm_open failed: %s\n", snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(ctx->pcm, params);

    /* RW_INTERLEAVED, not MMAP. The write path is snd_pcm_writei(), which is
     * only valid for RW access — asking for MMAP first (as this did) and then
     * calling writei() makes every write fail with EINVAL on any device that
     * happens to support MMAP. The stream opens fine and the rates look
     * right, so it presents as "device accepted the format then refused the
     * audio", which is a confusing place to land. MMAP would need
     * snd_pcm_mmap_begin/commit instead, and buys nothing here: the ring
     * buffer already decouples the network from ALSA. */
    if (snd_pcm_hw_params_set_access(ctx->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        fprintf(stderr, "halo: set_access(RW_INTERLEAVED) failed\n");
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
        return -1;
    }

    /* Try the exact format the wire asked for, then widen. A 24-bit track on
     * a device that only lists S32_LE used to be rejected outright, which is
     * a bad trade: left-justifying 24 bits into a 32-bit container loses
     * nothing, and the DAC ignores the low byte. Order matters — the natural
     * format is always tried first so the common case still writes the bytes
     * through untouched. */
    snd_pcm_format_t alsa_fmt = fmt->is_dsd == HALO_FMT_DSD_NATIVE
                                    ? forced_dsd_format
                                    : pcm_format_for(fmt);
    /* The rate below is derived from the packing width, so the candidate has
     * to be the active one before alsa_rate_for() runs. */
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) {
        halo_alsa_set_detected_dsd_format(forced_dsd_format);
    }
    ctx->widen_to_s32 = 0;
    err = snd_pcm_hw_params_set_format(ctx->pcm, params, alsa_fmt);
    if (err < 0 && fmt->is_dsd == HALO_FMT_PCM &&
        (fmt->bits_per_sample == 16 || fmt->bits_per_sample == 24)) {
        /* Widening is only ever upward, so nothing is lost: the sample keeps
         * every bit it had and gains zeros below it. Narrowing (a 32-bit
         * track on a 24-bit-only device) would be a truncation, so it is not
         * attempted — that stays a rejection the sender can act on. */
        int widened = snd_pcm_hw_params_set_format(ctx->pcm, params,
                                                   SND_PCM_FORMAT_S32_LE);
        if (widened == 0) {
            fprintf(stderr, "halo: device has no %s, using S32_LE "
                            "(%u-bit samples left-justified, still bit-exact)\n",
                    snd_pcm_format_name(alsa_fmt), fmt->bits_per_sample);
            alsa_fmt = SND_PCM_FORMAT_S32_LE;
            ctx->widen_to_s32 = 1;
            err = 0;
        }
    }
    if (err < 0) {
        if (!quiet) fprintf(stderr, "halo: set_format(%s) failed: %s\n",
                snd_pcm_format_name(alsa_fmt), snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
        return -1;
    }
    ctx->hw_format = alsa_fmt;

    err = snd_pcm_hw_params_set_channels(ctx->pcm, params, fmt->channels);
    if (err < 0) {
        if (!quiet) fprintf(stderr, "halo: set_channels(%u) failed: %s\n", fmt->channels, snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        return -1;
    }

    unsigned int rate = alsa_rate_for(fmt);
    unsigned int actual_rate = rate;
    err = snd_pcm_hw_params_set_rate_near(ctx->pcm, params, &actual_rate, NULL);
    if (err < 0) {
        if (!quiet) fprintf(stderr, "halo: set_rate_near(%u) failed: %s\n", rate, snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        return -1;
    }
    if (actual_rate != rate) {
        fprintf(stderr, "halo: warning — asked for %u Hz, device gave %u Hz "
                        "(driver may be lying about exact-rate support)\n",
                rate, actual_rate);
    }

    /* Generous buffer (~500ms) to absorb network jitter; moderate period
     * count so wakeups aren't too frequent nor too coarse. */
    unsigned int buffer_time_us = 500000;
    snd_pcm_hw_params_set_buffer_time_near(ctx->pcm, params, &buffer_time_us, NULL);
    unsigned int periods = 8;
    snd_pcm_hw_params_set_periods_near(ctx->pcm, params, &periods, NULL);

    err = snd_pcm_hw_params(ctx->pcm, params);
    if (err < 0) {
        if (!quiet) fprintf(stderr, "halo: hw_params commit failed: %s\n", snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        return -1;
    }

    snd_pcm_hw_params_get_period_size(params, &ctx->period_size, NULL);
    snd_pcm_hw_params_get_buffer_size(params, &ctx->buffer_size_frames);

    err = snd_pcm_prepare(ctx->pcm);
    if (err < 0) {
        fprintf(stderr, "halo: prepare failed: %s\n", snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        return -1;
    }

    ctx->fmt = *fmt;
    ctx->is_open = 1;
    strncpy(ctx->device_name, device_name, sizeof(ctx->device_name) - 1);

    fprintf(stderr, "halo: opened %s: %s, %u ch, %u Hz (requested), period=%lu buffer=%lu frames\n",
            device_name, snd_pcm_format_name(alsa_fmt), fmt->channels, rate,
            (unsigned long)ctx->period_size, (unsigned long)ctx->buffer_size_frames);

    return 0;
}

static int open_locked(halo_alsa_ctx_t *ctx, const char *device_name,
                        const struct halo_format *fmt) {
    if (fmt->is_dsd != HALO_FMT_DSD_NATIVE) {
        return open_with_format(ctx, device_name, fmt, SND_PCM_FORMAT_UNKNOWN, 0);
    }

    /* Try the packing the probe found first, then the rest. Probing at HELLO
     * can be wrong or can not have happened at all (device busy at the time),
     * and a DSD packing that the device refuses is not a track the daemon
     * should reject — it is a packing it should stop using. Doing the search
     * here is what lets one binary drive any DAC. */
    snd_pcm_format_t first = halo_alsa_dsd_format();
    if (open_with_format(ctx, device_name, fmt, first, 1) == 0) return 0;

    for (size_t i = 0; i < HALO_DSD_CANDIDATE_COUNT; i++) {
        if (k_dsd_candidates[i] == first) continue;
        if (open_with_format(ctx, device_name, fmt, k_dsd_candidates[i], 1) == 0) {
            fprintf(stderr, "halo: %s was not accepted, using %s instead\n",
                    snd_pcm_format_name(first),
                    snd_pcm_format_name(k_dsd_candidates[i]));
            return 0;
        }
    }

    /* Nothing worked — repeat the preferred attempt loudly so the log says
     * why, instead of only that "the format was rejected". */
    fprintf(stderr, "halo: no DSD packing this device accepts; last attempt:\n");
    halo_alsa_set_detected_dsd_format(first);
    return open_with_format(ctx, device_name, fmt, first, 0);
}

int halo_alsa_open(halo_alsa_ctx_t *ctx, const char *device_name,
                    const struct halo_format *fmt) {
    if (ctx->is_open) {
        halo_alsa_drain(ctx);
        halo_alsa_close(ctx);
    }
    return open_locked(ctx, device_name, fmt);
}

void halo_alsa_close(halo_alsa_ctx_t *ctx) {
    if (ctx->pcm) {
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
    }
    ctx->is_open = 0;
    ctx->widen_to_s32 = 0;
    free(ctx->convert_buf);
    ctx->convert_buf = NULL;
    ctx->convert_buf_bytes = 0;
}

/* Ensure ctx->convert_buf holds at least `bytes`. Returns 0 on success. */
static int ensure_convert_buf(halo_alsa_ctx_t *ctx, size_t bytes) {
    if (bytes <= ctx->convert_buf_bytes) return 0;
    uint8_t *grown = realloc(ctx->convert_buf, bytes);
    if (!grown) {
        fprintf(stderr, "halo: out of memory sizing conversion buffer to %zu bytes\n", bytes);
        return -1;
    }
    ctx->convert_buf = grown;
    ctx->convert_buf_bytes = bytes;
    return 0;
}

/* Left-justify narrower little-endian samples into 4-byte containers, for
 * DACs that offer S32_LE and nothing else — which is common enough that
 * refusing those tracks would rule out most of a CD-rip library on some
 * hardware. `src_bytes` is 2 (16-bit) or 3 (24-bit).
 *
 * The sample must sit in the *top* bytes. Right-justifying instead is the
 * tempting mistake: it still plays, at 1/256 or 1/65536 amplitude, which
 * reads as a volume or gain-staging problem rather than a packing one. */
static void widen_pcm_to_s32(const uint8_t *in, uint8_t *out, size_t samples,
                             size_t src_bytes) {
    size_t pad = 4 - src_bytes;
    for (size_t i = 0; i < samples; i++) {
        for (size_t b = 0; b < pad; b++) out[b] = 0;
        for (size_t b = 0; b < src_bytes; b++) out[pad + b] = in[b];
        in += src_bytes;
        out += 4;
    }
}

/* Repack native DSD from the wire layout into the device's packing.
 *
 * The wire layout is fixed by PROTOCOL.md and does not vary with the DAC:
 * chronological DSD bytes, interleaved two bytes per channel
 *   [c0.t0 c0.t1][c1.t0 c1.t1][c0.t2 c0.t3][c1.t2 c1.t3] ...
 * which is byte-for-byte DSD_U16_BE. ALSA's other DSD formats regroup those
 * same bytes: DSD_U32_BE takes four chronological bytes per channel per
 * frame, and the _LE variants store each group's bytes in reverse memory
 * order (the earliest sample sits in the most significant byte either way).
 *
 * Without this step the bytes were handed to a DSD_U32 device unchanged, so
 * each frame swallowed the next channel's pair as its own second half — the
 * channels smear into each other and it comes out as noise, not as music at
 * the wrong speed, which makes it easy to misread as a bit-order problem.
 *
 * `ticks` is wire ticks (one byte per channel); the output holds
 * ticks/width ALSA frames. */
static void repack_dsd(const uint8_t *in, uint8_t *out, size_t ticks,
                       unsigned channels, snd_pcm_format_t target) {
    size_t width = dsd_alsa_bytes_per_channel_frame();

    if (target == SND_PCM_FORMAT_DSD_U16_BE) {
        memcpy(out, in, ticks * channels); /* already this layout */
        return;
    }
    if (target == SND_PCM_FORMAT_DSD_U8) {
        /* One wire group of 2 bytes/channel becomes two 1-byte frames. */
        size_t groups = ticks / 2;
        for (size_t g = 0; g < groups; g++) {
            const uint8_t *src = in + g * channels * 2;
            for (unsigned c = 0; c < channels; c++) {
                out[(g * 2) * channels + c]       = src[c * 2];
                out[(g * 2 + 1) * channels + c]   = src[c * 2 + 1];
            }
        }
        return;
    }

    /* Remaining targets are U16_LE, U32_BE and U32_LE: gather `width`
     * chronological bytes per channel, then emit them in the byte order the
     * format's endianness calls for. */
    int reverse = (target == SND_PCM_FORMAT_DSD_U16_LE ||
                   target == SND_PCM_FORMAT_DSD_U32_LE);
    size_t frames = ticks / width;
    size_t groups_per_frame = width / 2; /* wire groups consumed per frame */

    for (size_t f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            uint8_t chrono[4];
            for (size_t g = 0; g < groups_per_frame; g++) {
                const uint8_t *src = in + ((f * groups_per_frame + g) * channels + c) * 2;
                chrono[g * 2]     = src[0];
                chrono[g * 2 + 1] = src[1];
            }
            uint8_t *dst = out + (f * channels + c) * width;
            for (size_t b = 0; b < width; b++) {
                dst[b] = reverse ? chrono[width - 1 - b] : chrono[b];
            }
        }
    }
}

snd_pcm_sframes_t halo_alsa_write(halo_alsa_ctx_t *ctx, const void *frames,
                                   snd_pcm_uframes_t nframes,
                                   int *underrun_occurred) {
    if (underrun_occurred) *underrun_occurred = 0;
    if (!ctx->is_open) return -1;

    /* Loop until every frame is accepted. snd_pcm_writei may legitimately
     * return a short count (a signal can interrupt it, and the recovery
     * retries below can come back short too). The caller advances its ring
     * by the value returned here, so returning a short count silently
     * dropped the remainder — an audible dropout that only showed up under
     * load or right after an underrun. Looping here keeps that failure mode
     * impossible without pushing partial-write bookkeeping onto the caller. */
    /* `nframes` counts wire ticks. For PCM a tick is an ALSA frame, but for
     * native DSD several ticks make up one frame, and the layout differs
     * too — so both the count and the bytes may need translating before
     * snd_pcm_writei sees them. Everything returned to the caller is
     * converted back to ticks so the ring and POSITION stay in wire units. */
    const uint8_t *cursor = (const uint8_t *)frames;
    size_t ticks_per_frame = halo_alsa_wire_ticks_per_frame(&ctx->fmt);
    size_t tick_bytes = halo_format_frame_size(&ctx->fmt);
    snd_pcm_uframes_t remaining = nframes / ticks_per_frame;
    if (remaining == 0) return 0;
    size_t frame_bytes = tick_bytes * ticks_per_frame;

    if (ctx->fmt.is_dsd == HALO_FMT_DSD_NATIVE) {
        size_t needed = frame_bytes * (size_t)remaining;
        if (ensure_convert_buf(ctx, needed) < 0) return -1;
        repack_dsd((const uint8_t *)frames, ctx->convert_buf,
                   (size_t)remaining * ticks_per_frame, ctx->fmt.channels,
                   ctx->hw_format);
        cursor = ctx->convert_buf;
    } else if (ctx->widen_to_s32) {
        /* Widen 3-byte samples into 4-byte containers when the device gave
         * us S32_LE instead of a real 24-bit format. Frame *counts* are
         * unchanged; only the stride and the source pointer differ. */
        size_t out_frame_bytes = (size_t)ctx->fmt.channels * 4;
        size_t needed = out_frame_bytes * (size_t)remaining;
        if (ensure_convert_buf(ctx, needed) < 0) return -1;
        widen_pcm_to_s32((const uint8_t *)frames, ctx->convert_buf,
                         (size_t)remaining * ctx->fmt.channels,
                         (size_t)ctx->fmt.bits_per_sample / 8);
        cursor = ctx->convert_buf;
        frame_bytes = out_frame_bytes;
    }

    const snd_pcm_uframes_t total_frames = remaining;
    int bad_state_retried = 0;

    while (remaining > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(ctx->pcm, cursor, remaining);

        if (written == -EPIPE) {
            if (underrun_occurred) *underrun_occurred = 1;
            fprintf(stderr, "halo: ALSA underrun (-EPIPE), recovering\n");
            snd_pcm_prepare(ctx->pcm);
            continue;
        }
        if (written == -ESTRPIPE) {
            /* Bounded, and it sleeps. The original spun on -EAGAIN with no
             * delay and no limit — on a SCHED_FIFO thread holding the device
             * lock, so a suspend that never resumes cleanly would peg a core
             * at real-time priority and wedge the socket reader behind the
             * lock at the same time. snd_pcm_prepare() is the correct
             * fallback: resuming is an optimisation that preserves the
             * buffered tail, and starting fresh is always available. */
            fprintf(stderr, "halo: ALSA suspended, attempting resume\n");
            int resume_waited_ms = 0;
            int resume_err;
            while ((resume_err = snd_pcm_resume(ctx->pcm)) == -EAGAIN) {
                if (resume_waited_ms >= 2000) {
                    fprintf(stderr, "halo: resume did not complete, restarting the stream\n");
                    break;
                }
                usleep(10000);
                resume_waited_ms += 10;
            }
            (void)resume_err;
            snd_pcm_prepare(ctx->pcm);
            continue;
        }
        if (written == -EINTR || written == -EAGAIN) {
            continue;
        }
        if (written == -EBADFD) {
            /* The stream is not in a writable state — most often SETUP,
             * left there by an snd_pcm_drop() that ran between this write
             * and the last. The daemon serialises those now (alsa_mtx), so
             * reaching here means something got past that; recover once
             * rather than treating it as fatal, since tearing the stream
             * down costs the listener the rest of the track while a
             * prepare() usually costs nothing. Only one attempt: if the
             * state is genuinely broken, retrying forever would spin. */
            if (!bad_state_retried) {
                bad_state_retried = 1;
                fprintf(stderr, "halo: ALSA in bad state (-EBADFD), re-preparing\n");
                if (snd_pcm_prepare(ctx->pcm) == 0) continue;
            }
        }
        if (written < 0) {
            fprintf(stderr, "halo: unrecoverable ALSA write error: %s\n",
                    snd_strerror((int)written));
            /* Report frames already accepted so the caller's ring and
             * position accounting stay truthful about what was consumed. */
            /* Back to wire ticks — the caller's ring and POSITION are
             * counted in those, not in ALSA frames. */
            snd_pcm_uframes_t done = total_frames - remaining;
            return done > 0 ? (snd_pcm_sframes_t)(done * ticks_per_frame) : written;
        }

        remaining -= (snd_pcm_uframes_t)written;
        cursor += (size_t)written * frame_bytes;
    }

    return (snd_pcm_sframes_t)(total_frames * ticks_per_frame);
}

int halo_alsa_drain(halo_alsa_ctx_t *ctx) {
    if (!ctx->is_open) return 0;

    /* Only a stream that is actually running has a tail to play out. After an
     * underrun it sits in XRUN, and before the first write in PREPARED —
     * draining either waits for a playback position that will never advance,
     * which on real hardware means the full timeout below, every time.
     *
     * This is not hypothetical: a hard-cut FORMAT clears the ring, so the
     * device has usually underrun by the time the switch reopens it, and the
     * log filled with "drain did not finish in 3000ms" — three seconds of
     * dead air at each format change, spent waiting for nothing. */
    snd_pcm_state_t state = snd_pcm_state(ctx->pcm);
    if (state != SND_PCM_STATE_RUNNING) {
        return 0;
    }

    /* Bounded, because a blocking snd_pcm_drain() can wait forever and the
     * caller holds the device lock across it. A stream that is not actually
     * running — stopped after an underrun, or prepared but never started —
     * has nothing draining it, so the wait never ends; the FLUSH that would
     * clear the situation is stuck behind the same lock, on the very thread
     * that reads the socket, so nothing can arrive to break it either. Same
     * permanent wedge as blocking the reader directly, reached by a
     * different route.
     *
     * Non-blocking mode turns the wait into a poll this function controls.
     * Draining is a courtesy at a format-changing handover (let the old
     * tail finish), never a correctness requirement, so giving up and
     * dropping is a sound worst case: at most the last fraction of a second
     * of the outgoing track is cut. */
    const int deadline_ms = 3000;
    int waited_ms = 0;
    snd_pcm_nonblock(ctx->pcm, 1);
    int err;
    while ((err = snd_pcm_drain(ctx->pcm)) == -EAGAIN) {
        if (waited_ms >= deadline_ms) {
            fprintf(stderr, "halo: drain did not finish in %dms, dropping the tail\n",
                    deadline_ms);
            snd_pcm_nonblock(ctx->pcm, 0);
            snd_pcm_drop(ctx->pcm);
            return snd_pcm_prepare(ctx->pcm);
        }
        usleep(5000);
        waited_ms += 5;
    }
    snd_pcm_nonblock(ctx->pcm, 0);
    return err;
}

int halo_alsa_drop_and_prepare(halo_alsa_ctx_t *ctx) {
    if (!ctx->is_open) return 0;
    snd_pcm_drop(ctx->pcm);
    return snd_pcm_prepare(ctx->pcm);
}

void halo_set_realtime_priority(void) {
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = 80; /* out of 1-99; leaves headroom above default kernel threads */

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        fprintf(stderr, "halo: warning — could not set SCHED_FIFO (need CAP_SYS_NICE / run as root "
                        "or via a systemd unit with appropriate limits); continuing at normal priority\n");
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "halo: warning — mlockall failed (RLIMIT_MEMLOCK too low?); continuing without it\n");
    }
}
