/*
 * alsa_output.h — direct hw: ALSA output. Deliberately never touches
 * plughw/dmix/default — see PROTOCOL.md and the design conversation this
 * came out of: any of those layers silently resamples/reformats and
 * breaks bit-perfect passthrough, which is the entire point of this
 * daemon existing.
 */
#ifndef HALO_ALSA_OUTPUT_H
#define HALO_ALSA_OUTPUT_H

#include <alsa/asoundlib.h>
#include "protocol.h"

typedef struct {
    snd_pcm_t *pcm;
    char device_name[64];
    struct halo_format fmt;
    int is_open;
    snd_pcm_uframes_t period_size;
    snd_pcm_uframes_t buffer_size_frames;
    /* What the device actually accepted, which is not always the natural
     * mapping of fmt.bits_per_sample: plenty of USB DACs advertise only
     * S32_LE and no 24-bit format at all. When that happens the samples are
     * widened on the way out (see widen_to_s32) rather than the track being
     * refused. */
    snd_pcm_format_t hw_format;
    int widen_to_s32;
    uint8_t *convert_buf;
    size_t convert_buf_bytes;
} halo_alsa_ctx_t;

/* Queries the device without fixing a format, to build the CAPS message.
 * best-effort: not every driver reports DSD support cleanly, so
 * supports_native_dsd/supported_dsd_rates_mask may need a config override
 * per-DAC (see README) rather than trusting autodetection blindly. */
int halo_alsa_query_caps(const char *device_name, struct halo_caps *caps_out);

/* Opens (or reopens) the device for the given format. Safe to call on an
 * already-open ctx — it will drain+close the old handle first if needed. */
int halo_alsa_open(halo_alsa_ctx_t *ctx, const char *device_name,
                    const struct halo_format *fmt);

void halo_alsa_close(halo_alsa_ctx_t *ctx);

/* Blocking write of nframes frames (frame = channels * bytes_per_sample,
 * or for native DSD, channels * dsd_bytes_per_channel_sample). Returns
 * frames actually written, or a negative value if a fatal (non-underrun)
 * error occurred. Underruns are handled internally via snd_pcm_prepare()
 * and reported back through *underrun_occurred so the caller can emit a
 * HALO_MSG_UNDERRUN. */
snd_pcm_sframes_t halo_alsa_write(halo_alsa_ctx_t *ctx, const void *frames,
                                   snd_pcm_uframes_t nframes,
                                   int *underrun_occurred);

/* Let currently-buffered audio finish playing out (used at end of a
 * format-changing gapless transition, NOT used for seek). */
int halo_alsa_drain(halo_alsa_ctx_t *ctx);

/* Immediately discard buffered audio and get ready for new data (used for
 * seek/FLUSH). */
int halo_alsa_drop_and_prepare(halo_alsa_ctx_t *ctx);

/* Bytes per *wire tick* — the unit the protocol counts in, which is what
 * sample_rate and POSITION.frames_written are both expressed in. For PCM a
 * tick is a sample frame; for native DSD it is one byte per channel,
 * independent of how the device packs DSD. */
size_t halo_format_frame_size(const struct halo_format *fmt);

/* How many wire ticks make up one ALSA frame. 1 for PCM; for native DSD it
 * is the device's packing width (4 under DSD_U32, 2 under DSD_U16, 1 under
 * DSD_U8), so callers sizing reads against period_size can scale correctly. */
size_t halo_alsa_wire_ticks_per_frame(const struct halo_format *fmt);

/* The DSD packing the attached device actually accepts, discovered by
 * halo_alsa_query_caps. Falls back to the compile-time default until then. */
snd_pcm_format_t halo_alsa_dsd_format(void);
void halo_alsa_set_detected_dsd_format(snd_pcm_format_t fmt);

/* ALSA frame rate currently configured (0 if closed). This is *not* the
 * wire `sample_rate` — for native DSD the wire value is a per-channel byte
 * rate and the ALSA rate depends on the DSD packing width. Exposed so the
 * status line can turn frames_written into a real elapsed time. */
unsigned int halo_alsa_current_rate(const halo_alsa_ctx_t *ctx);

/* Best-effort: pins the calling thread to SCHED_FIFO and mlocks its
 * memory. Logs a warning and continues (does not fail) if the process
 * lacks CAP_SYS_NICE / RLIMIT_MEMLOCK — real-time scheduling is a
 * nice-to-have for jitter, not a hard requirement for correctness. */
void halo_set_realtime_priority(void);

#endif /* HALO_ALSA_OUTPUT_H */
