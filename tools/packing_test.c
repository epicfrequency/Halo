/*
 * packing_test.c — byte-layout tests for the sample repacking in
 * alsa_output.c.
 *
 * These paths are worth pinning down precisely because getting them wrong
 * does not fail loudly. A DSD repack that groups bytes incorrectly still
 * opens the device, still keeps up with the clock, and still reports a
 * healthy position — it just plays noise, which is easy to misdiagnose as a
 * bit-order or DAC problem. The only cheap way to tell right from wrong is
 * to assert the exact output bytes, so that is what this does.
 *
 * alsa_output.c is #included rather than linked so the static repack
 * helpers are reachable without widening their visibility purely for tests.
 * It has to come first: its leading _DEFAULT_SOURCE only takes effect if no
 * system header has been processed yet, and glibc quietly withholds
 * alloca() (which ALSA's snd_pcm_hw_params_alloca needs) without it.
 */
#include "../src/alsa_output.c"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_bytes(const char *what, const uint8_t *got,
                         const uint8_t *want, size_t n) {
    if (memcmp(got, want, n) == 0) {
        printf("  PASS  %s\n", what);
        return;
    }
    failures++;
    printf("  FAIL  %s\n        got: ", what);
    for (size_t i = 0; i < n; i++) printf("%02x ", got[i]);
    printf("\n        want:");
    for (size_t i = 0; i < n; i++) printf(" %02x", want[i]);
    printf("\n");
}

/* Only the stub build has a start threshold to inspect; check-linux compiles
 * this same file against real ALSA, where an unused helper is an error. */
#ifdef HALO_ALSA_STUB
static void expect_true(const char *what, int ok, unsigned long got) {
    if (ok) {
        printf("  PASS  %s\n", what);
        return;
    }
    failures++;
    printf("  FAIL  %s (got %lu)\n", what, got);
}
#endif

static void expect_size(const char *what, size_t got, size_t want) {
    if (got == want) {
        printf("  PASS  %s\n", what);
        return;
    }
    failures++;
    printf("  FAIL  %s (got %zu, want %zu)\n", what, got, want);
}

/* Two channels, eight chronological bytes each, in the wire layout:
 * two bytes per channel, interleaved. L bytes are 0xL0..0xL7 as 0x10.., R
 * as 0x20.., with the low nibble carrying the time index so a misgrouped
 * byte is obvious by eye in a failure dump. */
static const uint8_t wire[] = {
    0x10, 0x11,  0x20, 0x21,   /* t0,t1 */
    0x12, 0x13,  0x22, 0x23,   /* t2,t3 */
    0x14, 0x15,  0x24, 0x25,   /* t4,t5 */
    0x16, 0x17,  0x26, 0x27,   /* t6,t7 */
};
#define WIRE_TICKS 8u  /* 8 bytes per channel */

int main(void) {
    uint8_t out[64];

    /* --- DSD_U16_BE: the wire layout is already this, byte for byte. --- */
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U16_BE);
    memset(out, 0xEE, sizeof(out));
    repack_dsd(wire, out, WIRE_TICKS, 2, SND_PCM_FORMAT_DSD_U16_BE);
    expect_bytes("DSD_U16_BE passes wire bytes through unchanged",
                 out, wire, sizeof(wire));

    /* --- DSD_U16_LE: same grouping, bytes reversed inside each group. --- */
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U16_LE);
    memset(out, 0xEE, sizeof(out));
    repack_dsd(wire, out, WIRE_TICKS, 2, SND_PCM_FORMAT_DSD_U16_LE);
    static const uint8_t want_u16le[] = {
        0x11, 0x10,  0x21, 0x20,
        0x13, 0x12,  0x23, 0x22,
        0x15, 0x14,  0x25, 0x24,
        0x17, 0x16,  0x27, 0x26,
    };
    expect_bytes("DSD_U16_LE reverses each 2-byte group",
                 out, want_u16le, sizeof(want_u16le));

    /* --- DSD_U32_BE: four chronological bytes per channel per frame.
     * This is the case the Gustard uses, and the one that was broken:
     * feeding the wire bytes straight through made frame 0 read
     * [10 11 20 21] as the left channel — the right channel's first pair
     * swallowed as left's second half. --- */
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U32_BE);
    memset(out, 0xEE, sizeof(out));
    repack_dsd(wire, out, WIRE_TICKS, 2, SND_PCM_FORMAT_DSD_U32_BE);
    static const uint8_t want_u32be[] = {
        0x10, 0x11, 0x12, 0x13,   0x20, 0x21, 0x22, 0x23,  /* frame 0 */
        0x14, 0x15, 0x16, 0x17,   0x24, 0x25, 0x26, 0x27,  /* frame 1 */
    };
    expect_bytes("DSD_U32_BE gathers 4 chronological bytes per channel",
                 out, want_u32be, sizeof(want_u32be));

    /* --- DSD_U32_LE: same gathering, whole 4-byte group reversed. --- */
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U32_LE);
    memset(out, 0xEE, sizeof(out));
    repack_dsd(wire, out, WIRE_TICKS, 2, SND_PCM_FORMAT_DSD_U32_LE);
    static const uint8_t want_u32le[] = {
        0x13, 0x12, 0x11, 0x10,   0x23, 0x22, 0x21, 0x20,
        0x17, 0x16, 0x15, 0x14,   0x27, 0x26, 0x25, 0x24,
    };
    expect_bytes("DSD_U32_LE reverses each gathered 4-byte group",
                 out, want_u32le, sizeof(want_u32le));

    /* --- DSD_U8: one byte per channel per frame, so each wire group of two
     * becomes two frames. --- */
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U8);
    memset(out, 0xEE, sizeof(out));
    repack_dsd(wire, out, WIRE_TICKS, 2, SND_PCM_FORMAT_DSD_U8);
    static const uint8_t want_u8[] = {
        0x10, 0x20,  0x11, 0x21,
        0x12, 0x22,  0x13, 0x23,
        0x14, 0x24,  0x15, 0x25,
        0x16, 0x26,  0x17, 0x27,
    };
    expect_bytes("DSD_U8 splits each wire group into two 1-byte frames",
                 out, want_u8, sizeof(want_u8));

    /* --- Unit accounting. A wire tick is one byte per channel regardless of
     * the device's packing; only ticks-per-frame tracks the packing. Getting
     * this backwards made POSITION report a quarter of the elapsed time. --- */
    struct halo_format dsd = { .channels = 2, .is_dsd = HALO_FMT_DSD_NATIVE,
                               .sample_rate = 352800 };
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U32_BE);
    expect_size("DSD wire tick stays 1 byte/channel under U32",
                halo_format_frame_size(&dsd), 2);
    expect_size("DSD_U32 needs 4 wire ticks per ALSA frame",
                halo_alsa_wire_ticks_per_frame(&dsd), 4);
    halo_alsa_set_detected_dsd_format(SND_PCM_FORMAT_DSD_U16_BE);
    expect_size("DSD_U16 needs 2 wire ticks per ALSA frame",
                halo_alsa_wire_ticks_per_frame(&dsd), 2);

    struct halo_format pcm24 = { .channels = 2, .is_dsd = HALO_FMT_PCM,
                                 .bits_per_sample = 24, .sample_rate = 88200 };
    expect_size("24-bit PCM frame is 3 bytes/channel on the wire",
                halo_format_frame_size(&pcm24), 6);
    expect_size("PCM is always 1 tick per frame",
                halo_alsa_wire_ticks_per_frame(&pcm24), 1);

    /* --- 24-bit widening into a 32-bit container, for DACs that offer only
     * S32_LE. The sample must land left-justified so the DAC's top bits are
     * the real ones; a right-justified copy is 256x too quiet. --- */
    static const uint8_t pcm_in[] = {
        0x11, 0x22, 0x33,   0x44, 0x55, 0x66,   /* frame 0: L, R */
        0x77, 0x88, 0x99,   0xAA, 0xBB, 0xCC,   /* frame 1: L, R */
    };
    uint8_t widened[16];
    widen_pcm_to_s32(pcm_in, widened, 4, 3);
    static const uint8_t want_widened[] = {
        0x00, 0x11, 0x22, 0x33,   0x00, 0x44, 0x55, 0x66,
        0x00, 0x77, 0x88, 0x99,   0x00, 0xAA, 0xBB, 0xCC,
    };
    expect_bytes("24-bit widens left-justified into S32_LE",
                 widened, want_widened, sizeof(want_widened));

    /* 16-bit needs the same treatment: DACs offering only S32_LE are common,
     * and refusing them would rule out ordinary CD-rate material. */
    static const uint8_t pcm16_in[] = { 0x11, 0x22,  0x33, 0x44 };
    uint8_t widened16[8];
    widen_pcm_to_s32(pcm16_in, widened16, 2, 2);
    static const uint8_t want_widened16[] = {
        0x00, 0x00, 0x11, 0x22,   0x00, 0x00, 0x33, 0x44,
    };
    expect_bytes("16-bit widens left-justified into S32_LE",
                 widened16, want_widened16, sizeof(want_widened16));

    /* --- The stream must not start before its buffer is full. ---
     *
     * ALSA defaults start_threshold to one frame, which starts playback on
     * an empty buffer: after every seek and every track cut the device ran
     * dry before the next few hundred milliseconds arrived over the network,
     * producing an -EPIPE and a dropout reported to the sender each time.
     *
     * The stub consumes instantly and can never underrun, so this cannot
     * reproduce the failure — it checks the daemon still asks for the
     * threshold that prevents it, which is the part that regressed silently
     * because playback sounded fine either way. */
#ifdef HALO_ALSA_STUB
    halo_alsa_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    struct halo_format pcm_fmt = {
        .format_id = 1, .sample_rate = 44100, .bits_per_sample = 32,
        .channels = 2, .is_dsd = 0, .dsd_rate_mult = 0, .channel_mask = 0x3,
    };
    if (halo_alsa_open(&ctx, "stub:0,0", &pcm_fmt) != 0) {
        failures++;
        printf("  FAIL  could not open the stub device\n");
    } else {
        /* Tens of milliseconds at 44.1k — 441 frames is 10ms, 4410 is 100ms.
         * Checked as a range because the exact frame count follows whatever
         * period size the device grants, and pinning it would make the test
         * about the stub rather than about the daemon refusing to start on
         * an empty buffer. */
        snd_pcm_uframes_t got = snd_stub_start_threshold();
        expect_true("start threshold is tens of ms, not one frame",
                    got >= 441 && got <= 4410, (unsigned long)got);
        /* The other half of the fix: it must stay well under the buffer, or
         * a seek waits for the whole 500ms before a sample is heard. The
         * stub's buffer is smaller than the real one, so this checks the
         * relationship rather than the ratio the hardware actually gets. */
        expect_true("start threshold is well below the buffer",
                    got * 2 <= ctx.buffer_size_frames, (unsigned long)got);
        halo_alsa_close(&ctx);
    }
#endif

    if (failures) {
        printf("\n%d packing check(s) failed\n", failures);
        return 1;
    }
    printf("\nall packing checks passed\n");
    return 0;
}
