/*
 * protocol.h — wire format for the HALO audio endpoint protocol (v1).
 * See PROTOCOL.md for the full spec. This header is the single source of
 * truth for struct layouts; keep it in sync with any macOS-side client.
 *
 * Assumption: both ends of this protocol are little-endian (x86_64/arm64
 * macOS, arm64 Pi5). No byte-swapping is done. If you ever run this on a
 * big-endian host, add htole32/le32toh calls at the (de)serialize points
 * marked below instead of assuming this holds.
 */
#ifndef HALO_PROTOCOL_H
#define HALO_PROTOCOL_H

#include <stdint.h>

#define HALO_MAGIC 0x484C4F31u /* "HLO1" */
#define HALO_PROTO_VERSION 1u

#pragma pack(push, 1)

typedef enum {
    HALO_MSG_HELLO      = 0x01,
    HALO_MSG_CAPS       = 0x02,
    HALO_MSG_FORMAT     = 0x03,
    HALO_MSG_AUDIO_DATA = 0x04,
    HALO_MSG_FLUSH      = 0x05,
    HALO_MSG_FLUSH_ACK  = 0x06,
    HALO_MSG_PAUSE      = 0x07,
    HALO_MSG_RESUME     = 0x08,
    HALO_MSG_POSITION   = 0x09,
    HALO_MSG_UNDERRUN   = 0x0A,
    HALO_MSG_METADATA   = 0x0B,
    HALO_MSG_COVERART   = 0x0C,
    HALO_MSG_BYE        = 0x0D,
    HALO_MSG_CANCEL_PREANNOUNCE = 0x0E,
    HALO_MSG_SWITCH_TO_PENDING  = 0x0F,
    HALO_MSG_FORMAT_REJECTED    = 0x10,
    HALO_MSG_PING               = 0x11,
    HALO_MSG_PONG               = 0x12,
} halo_msg_type;

/* Receivers MUST skip unknown message types using the header's `length`
 * rather than dropping the connection. Without this rule every new message
 * is a breaking change: 0x0E/0x0F were fatal to the original implementations
 * on both sides, which treated an unrecognised type as a framing error.
 * Skipping keeps framing intact and lets a peer ignore what it can't use. */

struct halo_header {
    uint32_t magic;
    uint16_t type;
    uint16_t flags;
    uint32_t length;    /* payload bytes following this header */
    uint32_t _reserved; /* must be 0 in v1; exists purely so `seq` below
                         * lands on an 8-byte boundary — see PROTOCOL.md
                         * "why the header is 24 bytes, not 20" note.
                         * Free for a future protocol version to use. */
    uint64_t seq;
};
#define HALO_HEADER_SIZE 24

struct halo_hello {
    uint32_t proto_version;
};

/* Feature bits for halo_caps.feature_flags. A peer advertises only what it
 * actually implements; absence must be treated as "not supported" so that
 * an older peer, which sends a 16-byte CAPS with no flags word at all,
 * degrades correctly. */
#define HALO_FEATURE_FORMAT_REJECTED (1u << 0) /* sends/understands 0x10 */
#define HALO_FEATURE_PING            (1u << 1) /* answers PING with PONG */
#define HALO_FEATURE_SKIPS_UNKNOWN   (1u << 2) /* skips unknown types by length */
#define HALO_FEATURE_UNSOLICITED_CAPS (1u << 3) /* may re-send CAPS mid-session */

/* CAPS is length-extensible: readers MUST accept a payload at least this
 * struct's original 16 bytes and ignore anything beyond what they know,
 * and MUST treat missing trailing fields as zero. That is what makes
 * adding a field here a non-breaking change. */
struct halo_caps {
    uint32_t max_sample_rate_pcm;
    uint32_t max_bits_per_sample;
    uint8_t  max_channels;
    uint8_t  supports_native_dsd;
    uint8_t  supports_dop;
    uint8_t  _pad;
    uint32_t supported_dsd_rates_mask;
    uint32_t feature_flags;   /* added after v1; absent == 0 */
};
#define HALO_CAPS_MIN_SIZE 16

/* is_dsd values */
#define HALO_FMT_PCM        0
#define HALO_FMT_DSD_NATIVE 1
#define HALO_FMT_DSD_DOP    2

struct halo_format {
    uint32_t format_id;
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  channels;
    uint8_t  is_dsd;
    uint8_t  dsd_rate_mult;
    /* Advisory in v1: the reference daemon opens the device by channel
     * *count* and does not remap. Senders must therefore emit channels in
     * the standard WAVE_EXTENSIBLE order for the mask they declare — a
     * daemon that applies snd_pcm_set_chmap can be added later without a
     * wire change, but do not assume one does. */
    uint32_t channel_mask;
    uint8_t  is_preannounce;
    uint8_t  _pad[3];
};

/* AUDIO_DATA payload is: struct halo_audio_data_hdr followed by raw frame bytes */
struct halo_audio_data_hdr {
    uint32_t format_id;
};

struct halo_position {
    uint32_t format_id;
    uint64_t frames_written;
    uint64_t mono_ns;
};

struct halo_underrun {
    uint32_t format_id;
    uint64_t mono_ns;
};

/* Hard cap on a single COVERART payload. Control and audio share one TCP
 * stream and one serial reader, so an oversized image stalls audio delivery
 * for exactly as long as it takes to read — the "never blocks the audio
 * path" claim only holds once the size is bounded. A receiver MUST drop
 * (drain and discard) anything larger rather than buffer it.
 *
 * The real constraint is that the stall stay well under the daemon's ring
 * depth (~5 s of audio), not the image size as such: 4 MiB is ~1.7 s even
 * on a weak 20 Mbps link, comfortably inside it. Sized for a local display
 * on the Pi, where art is shown at panel resolution rather than thumbnail
 * size. Sending it once per album, before the first FORMAT, avoids the
 * stall entirely — note that per-track art necessarily arrives *during*
 * playback of the previous track, since gapless preannounces the next one
 * while the current is still playing. */
#define HALO_COVERART_MAX_BYTES (4u << 20)
/* Metadata was text-only when this was 64 KiB, and text-only is not what it
 * carries any more: a sender may include a rendering of the cover for
 * endpoints whose display is a terminal, which is tens of kilobytes. The cap
 * exists to bound how long one message can occupy the serial reader, and by
 * that measure 256 KiB is nothing — it is a sixteenth of the cover-art cap
 * already accepted above, and about two milliseconds on a local network.
 * Sizing it to the old text-only assumption instead forced the rendering down
 * to a resolution where the art stopped being recognisable. */
#define HALO_METADATA_MAX_BYTES (256u << 10)

struct halo_coverart_hdr {
    uint8_t  sha256[32];
    uint32_t image_len;
};

/* Sender -> daemon only. Discards whatever was buffered under a previously
 * preannounced (is_preannounce=1) format_id that the sender has since
 * abandoned — e.g. a seek invalidated the gapless transition that format
 * was staged for, before the daemon ever promoted it to active. See
 * PROTOCOL.md's "Cancelling a preannounce" note under Gapless: without
 * this, the daemon has no way to know a preannounce was abandoned, since
 * v1 originally had no signal for that at all. Daemon behavior:
 *   - format_id matches the current pending slot -> discard its buffered
 *     bytes, clear the pending slot, no reply.
 *   - format_id doesn't match (already promoted, or a stale/duplicate
 *     cancel) -> ignore. Not an error; sender and daemon can race benignly
 *     here and this keeps that race harmless.
 */
struct halo_cancel_preannounce {
    uint32_t format_id;
};

/* Daemon -> sender. The daemon could not open the device for this format;
 * it will not consume AUDIO_DATA tagged with it. Without this the sender
 * has no way to learn about the failure: the daemon just silently drops
 * the audio, sends no POSITION (nothing is playing), and the sender stalls
 * forever waiting for a playback position that will never advance. */
#define HALO_REJECT_UNKNOWN        0
#define HALO_REJECT_RATE           1 /* sample rate not supported */
#define HALO_REJECT_CHANNELS       2 /* channel count not supported */
#define HALO_REJECT_FORMAT         3 /* bit depth / DSD packing not supported */
#define HALO_REJECT_DEVICE_BUSY    4 /* device open failed (in use, unplugged) */

struct halo_format_rejected {
    uint32_t format_id;
    uint32_t reason;
};

/* Sender -> daemon only. The sender's explicit gapless commit: "the stream
 * for the currently active format_id is complete — once whatever you still
 * have buffered for it finishes playing, promote the preannounced
 * format_id in this payload to active." The daemon:
 *   - format_id matches the current pending slot -> treat the active
 *     stream as ended (no more AUDIO_DATA is coming for it), drain its
 *     remaining buffered audio, then promote the pending stream to active
 *     and reset frames_written to 0 (a new position epoch — see
 *     PROTOCOL.md "Position epochs"). No reply.
 *   - format_id doesn't match (already promoted, stale, or no pending
 *     slot) -> ignore silently; sender and daemon can race benignly here.
 * This replaces v1's original "daemon infers the switch from the old
 * format's data going quiet" heuristic, which was both slow (idle timers)
 * and racy (the sender rejects POSITION for format_ids it no longer
 * considers active, so a mistimed inference deadlocked the transition —
 * see PROTOCOL.md's Gapless section for the full story). Daemons keep an
 * inference fallback only for senders that never send this message.
 */
struct halo_switch_to_pending {
    uint32_t format_id;
};

#pragma pack(pop)

#endif /* HALO_PROTOCOL_H */
