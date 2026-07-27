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
    switch (HALO_DSD_ALSA_FORMAT) {
        case SND_PCM_FORMAT_DSD_U8:     return 1;
        case SND_PCM_FORMAT_DSD_U16_LE:
        case SND_PCM_FORMAT_DSD_U16_BE: return 2;
        case SND_PCM_FORMAT_DSD_U32_LE:
        case SND_PCM_FORMAT_DSD_U32_BE: return 4;
        default:                        return 4;
    }
}

size_t halo_format_frame_size(const struct halo_format *fmt) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) {
        return (size_t)fmt->channels * dsd_alsa_bytes_per_channel_frame();
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

static snd_pcm_format_t pcm_format_for(const struct halo_format *fmt) {
    if (fmt->is_dsd == HALO_FMT_DSD_NATIVE) return HALO_DSD_ALSA_FORMAT;
    if (fmt->is_dsd == HALO_FMT_DSD_DOP)    return SND_PCM_FORMAT_S32_LE;
    switch (fmt->bits_per_sample) {
        case 16: return SND_PCM_FORMAT_S16_LE;
        case 24: return SND_PCM_FORMAT_S24_LE; /* 24 data bits in 32-bit container */
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
    struct { snd_pcm_format_t fmt; int bit; } dsd_probe[] = {
        { SND_PCM_FORMAT_DSD_U32_LE, -1 }, /* rate-dependent, checked below */
    };
    (void)dsd_probe;
    if (snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_DSD_U32_LE) == 0 ||
        snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_DSD_U16_LE) == 0 ||
        snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_DSD_U8) == 0) {
        any_dsd = 1;
        /* Rough rate-based mask: DSD64=2.8224MHz, 128=5.6448, 256=11.2896 */
        unsigned int rmin = 0;
        snd_pcm_hw_params_get_rate_min(params, &rmin, NULL);
        if (rate_max >= 11289600 / 4) dsd_mask |= (1u << 2); /* DSD256 (rate expressed post /4 for U32) */
        if (rate_max >= 5644800 / 4)  dsd_mask |= (1u << 1); /* DSD128 */
        if (rate_max >= 2822400 / 4)  dsd_mask |= (1u << 0); /* DSD64 */
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

    /* Prefer mmap access (fewer copies); fall back transparently — either
     * way we still call snd_pcm_writei() at the call site. */
    if (snd_pcm_hw_params_set_access(ctx->pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0) {
        if (snd_pcm_hw_params_set_access(ctx->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
            fprintf(stderr, "halo: no usable access mode\n");
            snd_pcm_close(ctx->pcm);
            return -1;
        }
    }

    snd_pcm_format_t alsa_fmt = pcm_format_for(fmt);
    err = snd_pcm_hw_params_set_format(ctx->pcm, params, alsa_fmt);
    if (err < 0) {
        fprintf(stderr, "halo: set_format(%s) failed: %s\n",
                snd_pcm_format_name(alsa_fmt), snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        return -1;
    }

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
    const uint8_t *cursor = (const uint8_t *)frames;
    snd_pcm_uframes_t remaining = nframes;
    size_t frame_bytes = halo_format_frame_size(&ctx->fmt);

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
            return (nframes - remaining) > 0
                       ? (snd_pcm_sframes_t)(nframes - remaining)
                       : written;
        }

        remaining -= (snd_pcm_uframes_t)written;
        cursor += (size_t)written * frame_bytes;
    }

    return (snd_pcm_sframes_t)nframes;
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
