#define _DEFAULT_SOURCE /* alloca() via alsa's snd_pcm_hw_params_alloca under -std=c11 */
#include "alsa_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

/* Which physical ALSA DSD packing to use for native DSD frames on the
 * wire. U32_LE is what most modern async USB DAC drivers (XMOS/Amanero
 * based) expose. If `aplay --dump-hw-params -D hw:X,Y /dev/null` on your
 * actual Pi5 + DAC shows only DSD_U8 or DSD_U16_LE, change this and
 * recompile — see README.md. */
#ifndef HALO_DSD_ALSA_FORMAT
#define HALO_DSD_ALSA_FORMAT SND_PCM_FORMAT_DSD_U32_LE
#endif

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
static snd_pcm_format_t g_detected_dsd_format = HALO_DSD_ALSA_FORMAT;
static int g_dsd_format_detected = 0;

void halo_alsa_set_detected_dsd_format(snd_pcm_format_t fmt) {
    g_detected_dsd_format = fmt;
    g_dsd_format_detected = 1;
}

snd_pcm_format_t halo_alsa_dsd_format(void) {
    return g_dsd_format_detected ? g_detected_dsd_format : (snd_pcm_format_t)HALO_DSD_ALSA_FORMAT;
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
    static const snd_pcm_format_t dsd_candidates[] = {
        SND_PCM_FORMAT_DSD_U32_BE, SND_PCM_FORMAT_DSD_U32_LE,
        SND_PCM_FORMAT_DSD_U16_BE, SND_PCM_FORMAT_DSD_U16_LE,
        SND_PCM_FORMAT_DSD_U8,
    };
    for (size_t i = 0; i < sizeof(dsd_candidates) / sizeof(dsd_candidates[0]); i++) {
        if (snd_pcm_hw_params_test_format(pcm, params, dsd_candidates[i]) == 0) {
            any_dsd = 1;
            halo_alsa_set_detected_dsd_format(dsd_candidates[i]);
            fprintf(stderr, "halo: device accepts native DSD as %s\n",
                    snd_pcm_format_name(dsd_candidates[i]));
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

static int open_locked(halo_alsa_ctx_t *ctx, const char *device_name,
                        const struct halo_format *fmt) {
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
    snd_pcm_format_t alsa_fmt = pcm_format_for(fmt);
    ctx->widen_to_s32 = 0;
    err = snd_pcm_hw_params_set_format(ctx->pcm, params, alsa_fmt);
    if (err < 0 && fmt->is_dsd == HALO_FMT_PCM && fmt->bits_per_sample == 24) {
        int widened = snd_pcm_hw_params_set_format(ctx->pcm, params,
                                                   SND_PCM_FORMAT_S32_LE);
        if (widened == 0) {
            fprintf(stderr, "halo: device has no %s, using S32_LE "
                            "(24-bit samples left-justified, still bit-exact)\n",
                    snd_pcm_format_name(alsa_fmt));
            alsa_fmt = SND_PCM_FORMAT_S32_LE;
            ctx->widen_to_s32 = 1;
            err = 0;
        }
    }
    if (err < 0) {
        fprintf(stderr, "halo: set_format(%s) failed: %s\n",
                snd_pcm_format_name(alsa_fmt), snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
        return -1;
    }
    ctx->hw_format = alsa_fmt;

    err = snd_pcm_hw_params_set_channels(ctx->pcm, params, fmt->channels);
    if (err < 0) {
        fprintf(stderr, "halo: set_channels(%u) failed: %s\n", fmt->channels, snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        return -1;
    }

    unsigned int rate = alsa_rate_for(fmt);
    unsigned int actual_rate = rate;
    err = snd_pcm_hw_params_set_rate_near(ctx->pcm, params, &actual_rate, NULL);
    if (err < 0) {
        fprintf(stderr, "halo: set_rate_near(%u) failed: %s\n", rate, snd_strerror(err));
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
        fprintf(stderr, "halo: hw_params commit failed: %s\n", snd_strerror(err));
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

/* Left-justify 3-byte samples into 4-byte containers, for DACs that offer
 * S32_LE but no real 24-bit format. The sample must sit in the *top* three
 * bytes: right-justifying it instead still plays, just 256x too quiet. */
static void widen_s24_to_s32(const uint8_t *in, uint8_t *out, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        out[0] = 0;           /* low byte the DAC will ignore */
        out[1] = in[0];
        out[2] = in[1];
        out[3] = in[2];       /* sign/MSB stays in the top byte */
        in += 3;
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
        widen_s24_to_s32((const uint8_t *)frames, ctx->convert_buf,
                         (size_t)remaining * ctx->fmt.channels);
        cursor = ctx->convert_buf;
        frame_bytes = out_frame_bytes;
    }

    const snd_pcm_uframes_t total_frames = remaining;

    while (remaining > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(ctx->pcm, cursor, remaining);

        if (written == -EPIPE) {
            if (underrun_occurred) *underrun_occurred = 1;
            fprintf(stderr, "halo: ALSA underrun (-EPIPE), recovering\n");
            snd_pcm_prepare(ctx->pcm);
            continue;
        }
        if (written == -ESTRPIPE) {
            fprintf(stderr, "halo: ALSA suspended, attempting resume\n");
            while (snd_pcm_resume(ctx->pcm) == -EAGAIN) {
                /* busy-wait briefly; real deployments may want a sleep here */
            }
            snd_pcm_prepare(ctx->pcm);
            continue;
        }
        if (written == -EINTR || written == -EAGAIN) {
            continue;
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
    return snd_pcm_drain(ctx->pcm);
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
