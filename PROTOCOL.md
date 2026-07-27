# HALO Audio Transport Protocol v1

("HALO" = Hi-res Audio Loop Output — placeholder name, rename freely.)

Single point-to-point protocol between a macOS sender (Audio Lounge) and one
Pi5 receiver daemon. No multi-device sync, no discovery, no auth — the
receiver listens on a fixed TCP port, the sender connects to a known IP.
Design goals in priority order: bit-perfect PCM/DSD passthrough, low and
predictable jitter tolerance via a decoupled ring buffer, gapless track
transitions, simple enough to debug by eye.

## Transport

One persistent TCP connection. `TCP_NODELAY` set on both ends (no Nagle
batching — every message should hit the wire immediately). All messages,
control and audio alike, share one stream and one framing format so there is
only one parser to get right.

## Framing

Every message is a fixed 24-byte header followed by `length` bytes of
payload:

```
struct halo_header {
    uint32_t magic;     // 0x484C4F31 ("HLO1"), sanity check / resync anchor
    uint16_t type;      // halo_msg_type
    uint16_t flags;     // reserved, must be 0 for v1
    uint32_t length;    // payload length in bytes, not including this header
    uint32_t _reserved; // must be 0 in v1 — see alignment note below
    uint64_t seq;       // monotonically increasing per-connection counter,
                        // set by whichever side sends the message. Mostly
                        // for logging, but it IS load-bearing in one place:
                        // the pre-flush audio barrier (see Seek / flush
                        // sequencing). Senders must keep it strictly
                        // increasing in the order messages are written.
} __attribute__((packed));  // 24 bytes, all fields little-endian
```

All integers are little-endian. The 4-byte magic lets a corrupted stream be
detected immediately (if the daemon ever fails to find `HLO1` at an expected
header boundary, it drops the connection rather than trying to resync
mid-stream — resyncing on live PCM/DSD is not worth the complexity given
this is a single trusted client).

**Why 24 bytes and not the more obvious 20**: without the `_reserved`
field, `magic+type+flags+length` sums to 12 bytes, putting the 8-byte `seq`
at offset 12 — not a multiple of 8. That's not a correctness bug (both
target platforms handle unaligned access fine, and the compiler generates
correct code for reads through a packed-struct type regardless), but it's
a free fix with no real cost — the header is only on the control-plane
path, not the hot audio-data path — so `_reserved` pads `seq` to start at
offset 16 instead. Struct size (24) and `seq`'s offset (16) both end up
multiples of 8, which is the property actually worth having, not "20
looked like a clean number."

## What travels on this protocol

Exactly three payload shapes, and never anything else:

| `is_dsd` | payload |
|---|---|
| 0 | linear PCM, 16/24/32-bit, channel-interleaved |
| 1 | native DSD, 1-bit, channel-interleaved raw bytes |
| 2 | DSD packed into a DoP container |

PCM on the wire does not imply a PCM source: converting DSD to PCM before
sending is a normal mode (the sender's "HALO DSD Mode: PCM"), and it is what
lets a DSD file play to a DAC with no DSD support at all. The wire carries
the conversion's *output*, so `FORMAT` correctly says PCM — the fact that it
began as DSD is metadata, not format, and travels in `METADATA.source` so a
display can show "DSD64 → PCM 352.8k" rather than just "PCM 352.8k".

### Byte layout on the wire

The layout below is fixed and does **not** vary with the receiving DAC. A
receiver whose hardware wants a different arrangement is responsible for
repacking; a sender must never adapt its layout to the device.

**PCM** — little-endian signed samples, channel-interleaved, one frame per
`sample_rate` tick. 24-bit is *three* bytes per sample, packed with no
padding — not a 24-in-32 container. A receiver whose device only offers a
32-bit format widens the samples itself, left-justified.

**Native DSD** — chronological DSD bytes, MSB-first within each byte,
interleaved **two bytes per channel**:

```
[c0.t0 c0.t1] [c1.t0 c1.t1] [c0.t2 c0.t3] [c1.t2 c1.t3] ...
```

which is byte-for-byte ALSA's `DSD_U16_BE`. It was chosen because DSD
sources are naturally 16-bit-grouped and because DoP needs pairs anyway, so
no sender has to regroup.

The receiver regroups from here. ALSA's other DSD formats hold the same
bytes differently: `DSD_U32_*` gathers four chronological bytes per channel
into one frame, and the `_LE` variants store each group in reverse memory
order (the earliest sample sits in the most significant byte either way).
Handing wire bytes to a `DSD_U32` device unrepacked is a silent failure
worth naming, because it does not sound like a layout bug: the device opens,
the clock keeps up, and the position advances normally, but each frame
swallows the next channel's pair as its own second half and the channels
smear into noise — easily misread as a bit-order or DAC-compatibility
problem.

### The unit `sample_rate` and `frames_written` count

Both are in **wire ticks**, and a tick is not always an ALSA frame:

| `is_dsd` | one tick | `sample_rate` |
|---|---|---|
| 0 (PCM) | one sample frame | sample rate in Hz |
| 1 (native DSD) | **one byte per channel** | DSD bit rate ÷ 8 |
| 2 (DoP) | one 32-bit container per channel | DSD bit rate ÷ 16 |

For native DSD a tick is deliberately one byte per channel regardless of how
the device packs DSD, so that `sample_rate = bit_rate/8` and the position
arithmetic agree on both sides. A receiver that reports ALSA frames instead
under-reports elapsed time by its packing width — a factor of four under
`DSD_U32`, which reads as a stalled or half-speed position rather than as a
unit mismatch.

**No compressed audio ever reaches this protocol.** Not FLAC, ALAC, MP3,
DST or anything else — the sender decodes first, always, and HALO carries
only the finished samples. The daemon owns no decoder and cannot acquire
one: it is a pipe to `snd_pcm_writei`, so the bytes it receives are the
bytes the DAC gets.

That is what makes bit-perfect verifiable rather than aspirational, and why
`FORMAT` describes a memory layout rather than naming a codec. DST-compressed
SACD content is the case worth stating explicitly: DST is a lossless
compression *of a DSD bitstream*, so the sender must fully decompress it —
by the time it goes on the wire it is indistinguishable from DSD read
straight out of a `.dsf`.

## Message types

| value | name           | direction       | payload |
|-------|----------------|-----------------|---------|
| 0x01  | HELLO          | sender→daemon   | `{uint32 proto_version}` |
| 0x02  | CAPS           | daemon→sender   | capability struct (below) |
| 0x03  | FORMAT         | sender→daemon   | format struct (below) |
| 0x04  | AUDIO_DATA     | sender→daemon   | `{uint32 format_id}` + raw frame bytes |
| 0x05  | FLUSH          | sender→daemon   | empty |
| 0x06  | FLUSH_ACK      | daemon→sender   | empty |
| 0x07  | PAUSE          | sender→daemon   | empty |
| 0x08  | RESUME         | sender→daemon   | empty |
| 0x09  | POSITION       | daemon→sender   | `{uint32 format_id, uint64 frames_written, uint64 mono_ns}` |
| 0x0A  | UNDERRUN       | daemon→sender   | `{uint32 format_id, uint64 mono_ns}` |
| 0x0B  | METADATA       | sender→daemon   | UTF-8 JSON, schema below |
| 0x0C  | COVERART       | sender→daemon   | `{uint8[32] sha256, uint32 image_len}` + image bytes |
| 0x0D  | BYE            | either          | empty |
| 0x0E  | CANCEL_PREANNOUNCE | sender→daemon | `{uint32 format_id}` |
| 0x0F  | SWITCH_TO_PENDING | sender→daemon | `{uint32 format_id}` |
| 0x10  | FORMAT_REJECTED | daemon→sender | `{uint32 format_id, uint32 reason}` |
| 0x11  | PING           | either          | empty |
| 0x12  | PONG           | either          | empty |

**Unknown message types must be skipped, not fatal.** A receiver that meets
a type it doesn't recognise reads and discards `length` bytes and carries
on. This rule is retroactive and load-bearing: without it every added
message is a breaking change, which is exactly what happened when 0x0E/0x0F
were introduced — both original implementations treated an unknown type as
a framing error and dropped the connection. Framing is self-describing, so
skipping is always safe.

The daemon does nothing with METADATA/COVERART except stash the latest copy
(for a local display, if the Pi ever gets one) — it never blocks the audio
path on these.

### CAPS payload

```
struct halo_caps {
    uint32_t max_sample_rate_pcm;   // e.g. 768000
    uint32_t max_bits_per_sample;   // e.g. 32
    uint8_t  max_channels;
    uint8_t  supports_native_dsd;   // 1 if ALSA hw exposes DSD_U* formats
    uint8_t  supports_dop;          // 1 if a DoP fallback path is wired up
    uint8_t  _pad;
    uint32_t supported_dsd_rates_mask; // bit 0 = DSD64, bit 1 = DSD128, ...
};
```

`feature_flags` was added after the original v1 and tells the sender which
of the later messages this daemon actually implements:

| bit | meaning |
|-----|---------|
| 0 | sends `FORMAT_REJECTED` when it can't open a format |
| 1 | answers `PING` with `PONG` |
| 2 | skips unknown message types instead of dropping the connection |
| 3 | may re-send `CAPS` mid-session (e.g. the USB DAC changed) |

**CAPS is length-extensible.** A reader must accept any payload of at least
16 bytes (the original struct), ignore trailing bytes it doesn't understand,
and treat absent trailing fields as zero. That is what makes adding a field
here non-breaking; a v1 daemon simply reports no features, and the sender
degrades to v1 behaviour.

Sent once, immediately after HELLO is received and the daemon has queried
the configured ALSA device with `snd_pcm_hw_params_any`. A daemon that sets
feature bit 3 may also send it again later, unsolicited — the sender must
accept that and replace its cached copy rather than treating it as a
protocol error. The sender is
expected to reject/refuse to send any FORMAT the daemon didn't advertise
rather than let a `snd_pcm_hw_params` call fail downstream.

### FORMAT payload

```
struct halo_format {
    uint32_t format_id;       // sender-assigned, increments per format change
    uint32_t sample_rate;     // PCM sample rate, or DSD bit rate / 8 if is_dsd
    uint8_t  bits_per_sample; // 16/24/32 for PCM, ignored for native DSD
    uint8_t  channels;
    uint8_t  is_dsd;          // 0 = PCM, 1 = native DSD, 2 = DSD-over-PCM
    uint8_t  dsd_rate_mult;   // 1=DSD64, 2=DSD128, 4=DSD256, 0 if not DSD
    uint32_t channel_mask;    // bit per output position, standard WAVE_EXT layout
    uint8_t  is_preannounce;  // 1 = "next track", 0 = "current track, apply now"
    uint8_t  _pad[3];
};
```

`channel_mask` uses the standard Microsoft `SPEAKER_*` bit positions (same
convention WAVEFORMATEXTENSIBLE/multi-channel FLAC/DoP-capable DACs already
use, not something HALO-specific):

| bit | position           | bit | position            |
|-----|---------------------|-----|----------------------|
| 0   | front left          | 5   | back right           |
| 1   | front right         | 6   | front left-of-center |
| 2   | front center        | 7   | front right-of-center |
| 3   | LFE / subwoofer     | 9   | side left            |
| 4   | back left           | 10  | side right           |

Common cases spelled out so there's no per-implementation guessing: stereo
= `0x3` (bits 0,1). 5.1 (the layout basically all multi-channel SACD/DVD-A
content uses) = `0x3F` (bits 0–5). 7.1 = `0x63F` (5.1 plus bits 9,10 for
the side speakers).

Every AUDIO_DATA message carries the `format_id` it belongs to, so the two
formats that exist briefly during a gapless handover (current tail +
next-track head) never get mixed up even if they arrive close together.

**`format_id` must be unique per FORMAT message the sender ever emits,
including consecutive tracks whose PCM/DSD parameters are byte-for-byte
identical.** It is a routing key, not a "did the format change" flag — the
daemon only has two ring buffers (active + one pending slot) and decides
which one incoming AUDIO_DATA belongs to purely by comparing `format_id`
against `format_id[active]` and `format_id[pending]`. If the sender reuses
the same `format_id` for two same-format tracks in a row, the pending
track's audio will be misrouted into the still-playing active ring instead
of the pending one. A simple monotonic counter, incremented on every
FORMAT message regardless of whether the parameters changed, is sufficient
and is what the reference sender should do.

**Only one preannounced track can be pending at a time.** The daemon has
exactly one "other" ring buffer slot; if a second preannounce arrives
before the daemon has actually switched to the first one, the second
overwrites the first (with a loud warning logged — see main.c) and the
first preannounced track's buffered lookahead is lost. In practice this
means: don't preannounce track N+2 until you've confirmed (e.g. via
POSITION reports, or just by tracking your own send-side state) that the
switch to N+1 has happened. This protocol does not support arbitrary
look-ahead depth — one track ahead, gaplessly, is the whole feature.

## Source format responsibilities — where each boundary problem is handled

HALO only ever sees the *output* of decoding: fully-decoded, per-sample
channel-interleaved, correct-bit-order raw PCM or DSD bytes plus a format
descriptor. It deliberately knows nothing about DSF, DFF/DSDIFF, SACD ISO,
DST, FLAC, ALAC, or any other container/codec — that separation is
intentional, not an oversight, and it's worth being explicit about which
layer owns which class of boundary bug, because getting this wrong
produces symptoms that don't look like an obvious crash (garbled/harsh
noise, silent channel swaps) rather than a clean error:

- **SACD ISO extraction + DST decompression**: entirely the sender's
  decoder's job. DST is a lossless *compression* of a DSD bitstream, not a
  different final format — by the time audio reaches HALO it must already
  be plain decompressed DSD, indistinguishable on the wire from a DSD
  extracted straight from a `.dsf` file. If your decoder ever hands HALO
  still-DST-compressed bytes, that's a decoder bug, not something this
  protocol/daemon can detect or correct — there's no framing here that
  would tell it "this is still compressed."

- **DSF's block structure**: DSF stores each channel's DSD data in its own
  block (commonly 4096 bytes) rather than sample-interleaved like PCM —
  the decoder must de-interleave these into the same
  channel-interleaved-per-frame layout PCM already uses before handing
  bytes to HALO. `halo_format_frame_size()` / the ALSA writer assume
  standard interleaving (frame N = one sample per channel, back to back);
  if the decoder still hands over DSF's native per-channel-block layout,
  every frame downstream is reading the wrong bytes as the wrong channel —
  this would sound like noise or heavily distorted audio, not a crash, so
  it's exactly the kind of bug the "play a known reference track, verify
  bit-perfect" step (see README) is there to catch.

- **DSF's bit order**: this is the one most worth flagging explicitly
  because it's a well-known real-world gotcha in DIY DSD pipelines and
  fails silently as "harsh/wrong sound," not an error. DSF stores DSD bits
  LSB-first within each byte; DFF/DSDIFF stores MSB-first; and which order
  a given DAC's ALSA driver expects for its native `DSD_U8`/`U16`/`U32`
  format is **not guaranteed to be the same for every DAC** — it's
  effectively per-driver. The sender's decoder needs to normalize to
  whichever bit order your specific target DAC actually wants, and that's
  something you determine empirically per-DAC (a byte-reversal lookup
  table is a one-line fix if it turns out to be backwards — trivial to
  add, easy to forget to check for). This is squarely a sender-side
  concern; HALO's wire format carries opaque bytes and has no opinion on
  bit order within them.

- **Multi-channel PCM (5.1/7.1) that doesn't match the DAC's actual
  channel count**: HALO does not downmix. If `FORMAT` announces 6 channels
  and the DAC's ALSA device only supports 2, `snd_pcm_hw_params_set_channels`
  fails, the daemon logs it and refuses to open — it does not silently
  fold 5.1 down to stereo. Any downmix decision has to be made by the
  sender before the `FORMAT` message is sent, using whatever channel count
  the sender already confirmed (via `CAPS`) the target DAC supports.

- **Frame alignment / incomplete frames on the wire**: this one *is*
  handled at the daemon level, generically, regardless of which container
  format the audio originally came from — see the ring buffer's
  whole-frames-only read logic in `main.c` (`usable_bytes` clamping). It
  doesn't matter whether the misalignment risk originated from a DSF
  block boundary, a PCM resampler producing an odd tail, or just an
  unlucky TCP chunk boundary — the daemon never pulls a partial frame out
  of the ring, full stop.

## Handshake

1. Sender connects, sends `HELLO`.
2. Daemon opens the configured ALSA device just far enough to query
   capabilities (`snd_pcm_hw_params_any`, no format fixed yet), replies with
   `CAPS`.
3. Sender picks a format from what CAPS advertised, sends `FORMAT`
   (`is_preannounce = 0`), daemon opens the device for real with those
   `hw_params` and starts accepting `AUDIO_DATA`.

### Non-preannounce FORMAT mid-stream

A `FORMAT` with `is_preannounce = 0` arriving while a stream is already
active is a **hard cut** to a brand-new active stream — the sender uses it
for "start this fresh track now" (manual skip, auto-advance where no
gapless handover was staged). The daemon:

- discards any audio still buffered for the old active stream (this is a
  cut, not a handover — use preannounce + `SWITCH_TO_PENDING` for
  gapless),
- discards any pending preannounce slot (whatever was staged belongs to a
  playback plan the sender has abandoned; no `CANCEL_PREANNOUNCE` needed
  first),
- reopens/keeps the output device as the format requires and makes the new
  `format_id` active,
- starts a new position epoch (`frames_written` resets to 0 — see
  "Position epochs" below), and
- clears any paused state (see "Pause semantics" below): announcing a
  fresh active stream is an unambiguous statement of intent to play it.

## Gapless

While the current track is playing, once the sender knows the next track's
format and has decoded enough of it to have data ready, it sends a `FORMAT`
message with `is_preannounce = 1` and a new `format_id`, immediately
followed by `AUDIO_DATA` tagged with that new `format_id`. The daemon
buffers those bytes in a side buffer keyed by `format_id` and does **not**
write them to ALSA yet.

When the sender's own playback accounting says the current track has
finished (it, not the daemon, is the side that actually knows — it decoded
the last byte and has been tracking POSITION against the track's total
length), it sends `SWITCH_TO_PENDING` with the pending `format_id`,
**after** the old stream's final `AUDIO_DATA` on the same TCP stream. That
message means: "no more data is coming for the old format; the pending
format is the new active stream." The daemon then:

- lets whatever it still has buffered for the old format finish playing
  (drain, not drop — network buffering means the old track's tail may
  still be in flight/queued daemon-side, and gapless correctness requires
  it to be audible to the last sample), then
- compares old vs new format:
  - identical `sample_rate`/`bits_per_sample`/`channels`/`is_dsd` → just
    keep writing to the already-open ALSA handle, no gap.
  - different → `snd_pcm_drain()` on the old handle (let the tail finish
    playing out), close, reopen with the new `hw_params`, then start
    writing the buffered next-track bytes immediately. This is the one case
    where a small gap is physically unavoidable (the hardware literally
    cannot represent both sample rates at once); the goal is to make that
    gap exactly the ALSA reopen cost and nothing more.
- promotes the pending stream to active and resets `frames_written` to 0
  (new position epoch). All subsequent POSITION messages carry the new
  `format_id`.

`SWITCH_TO_PENDING` with a `format_id` that doesn't match the current
pending slot (already promoted, stale, nothing pending) is silently
ignored — the sender and daemon can race here benignly, same contract as
`CANCEL_PREANNOUNCE`.

**Why an explicit message.** v1 originally had the daemon *infer* the
switch from the old format's AUDIO_DATA going quiet plus its buffers
draining. That inference is fundamentally racy against the sender's own
commit: the sender only trusts POSITION tagged with the `format_id` *it*
currently considers active, so any timing mismatch between the two sides'
notion of "switched" makes one side silently reject the other's position
reports — in practice this deadlocked every gapless transition (sender
commits, daemon keeps reporting the old id, sender's played-position
freezes, its send pacing gate never reopens, so the daemon never receives
the data whose arrival it was waiting for) until some daemon-side idle
timer guessed its way out, tens of seconds later. The sender knows the
exact commit moment; now it just says so. Daemons may keep a conservative
idle/drain inference **only as a fallback** for senders that never send
`SWITCH_TO_PENDING`.

#### Why the receiver must not promote on its own

It is tempting to let the receiver decide. TCP ordering appears to hand it a
sound test: the sender emits all of the old stream's audio, then the
preannounce, then the new stream's audio, on one connection — so the presence
of any pending-stream byte proves every old-stream byte already arrived, and
an empty active buffer therefore means the old track is genuinely finished.

That reasoning is correct, and promoting on it is still wrong. The sender
trusts POSITION only for the `format_id` **it** considers active. A receiver
that switches first has every position report it sends discarded by the
sender, whose played-position then freezes, whose send-pacing gate never
reopens, and whose newly-promoted stream starves a few seconds later. It
presents as a track that starts and then stalls, which points nowhere near
the actual cause.

Seeking close to the end of a track triggers it almost every time: the old
tail is already consumed before the preannounce even lands, so the "active
empty, pending non-empty" condition is true the instant the next track is
announced.

The commit is the sender's to make. A receiver may keep a conservative
idle/drain inference **only as a fallback** for senders that never send
`SWITCH_TO_PENDING` at all.

#### The deadlock that makes waiting look impossible

There is a real trap on the other side, and it is worth naming because it is
what tempts implementers into receiver-side promotion:

1. the receiver's pending buffer fills with the next track's audio;
2. it stops reading the socket to apply backpressure — but the reading side
   is also what delivers `SWITCH_TO_PENDING`;
3. the pending buffer is only drained *after* a promotion, which is the thing
   waiting on the undelivered message.

Nothing breaks that cycle except a timeout, so every boundary stalls for its
full length. It is strongly rate-dependent: DSD fills a buffer fast enough to
hit it on essentially every track, while PCM at ordinary rates usually slips
through, which makes it read like a DSD-specific bug rather than a structural
one.

The fix belongs in the receiver's I/O, not in its promotion policy: **the
socket reader must never block**. Overflow that does not fit the buffer goes
somewhere else — the reference daemon spills it to a growable area the
playback thread drains back — so control messages are always delivered and
the sender's commit always arrives. Solving it by promoting early instead
trades a stall that recovers for a starvation that does not.

This is one of the concrete costs of sharing a single stream between control
and bulk data, and part of the motivation for the channel split in the v2
proposal below.

### Senders MUST bound lookahead in bytes, not only in time

The same head-of-line hazard has a second, worse form, and closing it is a
requirement on the *sender*.

A receiver applies backpressure by not reading its socket once its buffer is
full. That also stops control messages, because they share the socket. While
**paused** this never recovers on its own: the receiver's writer is stopped,
so its buffer never drains, so it never resumes reading, so the `RESUME` that
would restart the writer can never arrive. There is no timeout that can
rescue it — unlike the gapless case, waiting does not help.

Seconds are the wrong unit to bound this with, because the byte cost of a
second spans two orders of magnitude across the formats this protocol
carries:

| format | 5 seconds of lookahead |
|---|---|
| PCM 16/44.1 | ~0.9 MB |
| PCM 24/96 | ~2.9 MB |
| DSD64 | ~3.5 MB |
| DSD512 | ~28 MB |

A sender tuned to a comfortable 5s at PCM rates silently demands 28 MB of
receiver buffer at DSD512. Senders therefore **must** apply a byte ceiling
alongside whatever time-based pacing they use, and receivers **should** size
each per-stream buffer above that ceiling with headroom for what is already
in the socket. The reference implementations use 8 MiB and 16 MiB
respectively.

### Cancelling a preannounce

A preannounce isn't a commitment — the sender can legitimately abandon it
before the daemon ever promotes it, most commonly because the *current*
track got seeked (which invalidates whatever "next track" lookahead was in
flight) before the switch happened. v1 originally had no way for the daemon
to learn this, which left it holding a pending format_id the sender would
never send more data for. If the sender later preannounces a *real* next
track while that orphan is still sitting in the pending slot, the daemon
has no way to tell old-abandoned from new-legitimate — they collide.

`CANCEL_PREANNOUNCE` (`0x0E`, sender → daemon only, payload is a single
`format_id`) fixes this: sent whenever the sender abandons a `format_id` it
previously preannounced, before that format was ever promoted to active.

- `format_id` matches the daemon's current pending slot → discard the
  buffered bytes for it, clear the pending slot. No reply (fire-and-forget,
  same as `PAUSE`/`RESUME`).
- `format_id` doesn't match (already promoted, or the cancel arrived after
  a newer preannounce already overwrote the slot) → ignore silently. The
  sender and daemon can race harmlessly here; this keeps that race a no-op
  instead of a bug.

This message carries no obligation for the daemon to act *instead of*
inferring abandonment some other way (a daemon is still free to notice via
its own idle/drain heuristics, as v1's original inference did) — it's a
faster, unambiguous signal the sender can provide whenever it already knows
a preannounce is dead, which is precisely the seek case above.

## When the daemon can't open a format

`CAPS` is coarse — a maximum rate, a maximum channel count, DSD yes/no. It
cannot express which *combinations* a device will actually accept, so a
`FORMAT` that passes the sender's pre-check can still fail at
`snd_pcm_hw_params`.

The daemon replies `FORMAT_REJECTED{format_id, reason}` and does not consume
audio for that `format_id`. The sender must stop and surface the failure
rather than continue sending.

Without this message the failure is invisible and looks like a hang: the
daemon drops the audio (it belongs to no open stream), sends no `POSITION`
(nothing is playing), and a sender that paces itself against playback
position waits forever for a position that will never advance. The user sees
a play button that does nothing, with no error anywhere. A daemon that
predates this message never sends it — check feature bit 0, and keep a
timeout for daemons that don't have it.

## Liveness

Either side may send `PING`; the peer answers `PONG`. Both are empty. Any
other message from the peer counts as proof of life too, so a busy session
never needs to probe.

Keeping an idle session up needs **two** things, and they solve different
problems:

- **TCP keepalive** (`SO_KEEPALIVE`, ~15 s idle then probes) stops the
  connection being *dropped*. Consumer routers and access points reap idle
  NAT/conntrack entries after a few minutes, and a paused session is
  completely silent — without probes the flow is quietly discarded by the
  network and neither end learns until the next write fails.
- **`PING`** makes a genuinely dead peer *detectable quickly*. Bare TCP can
  take many minutes to give up, which would leave a sender's UI claiming it
  is connected to an endpoint that has been unplugged.

The reference implementations enable keepalive on both sockets, and the
sender probes after 10 s of having sent nothing, treating 15 s without any
reply as a dead link.

This exists because a paused session is completely silent on the wire: the
sender stops sending audio and the daemon only emits `POSITION` while
something is actually playing. A connection that dies while paused — an
access point reboot, a cable pulled — is then invisible to both ends until
TCP eventually times out, which can take minutes. A sender that has sent
nothing for a few seconds should ping; no `PONG` within a few more means the
link is gone, regardless of what TCP still believes.

## METADATA schema

v1 left this as "some JSON, daemon doesn't care", which is fine while the
daemon only stores it — but a display on the endpoint has to be able to rely
on field names, so the shape is pinned here.

```json
{
  "track": {
    "title": "So What",
    "artist": "Miles Davis",
    "album": "Kind of Blue",
    "album_artist": "Miles Davis",
    "track_number": 1,
    "disc_number": 1,
    "duration_seconds": 545.2
  },
  "album": {
    "title": "Kind of Blue",
    "artist": "Miles Davis",
    "year": 1959,
    "track_count": 5,
    "tracks": [
      { "track_number": 1, "title": "So What",            "duration_seconds": 545.2 },
      { "track_number": 2, "title": "Freddie Freeloader", "duration_seconds": 574.1 }
    ]
  },
  "source": {
    "kind": "dsd",
    "container": "dsf",
    "dsd_bit_rate_hz": 2822400,
    "dsd_rate_label": "DSD64",
    "converted_to_pcm": true
  },
  "coverart_sha256": "9f86d081884c7d65..."
}
```

`source` describes the *file*, which is not always what goes on the wire.
`status.json` reports what the DAC is actually receiving; this reports where
it came from. They differ exactly when the sender converts — DSD played to a
PCM-only DAC shows `kind: "dsd"` here and PCM in `status.json`, which is the
pair a display needs to say "DSD64 → PCM 352.8k" honestly. For a native DSD
or plain PCM stream the two simply agree. `kind` is `"pcm"` or `"dsd"`; the
`dsd_*` fields are present only for `"dsd"`.

Every field is optional — a sender that knows only a title sends only that,
and a display must tolerate anything missing. `album.tracks` lets the
endpoint show the whole record with the current one highlighted; send it
once when the album starts rather than per track, since it doesn't change.
`coverart_sha256` ties this to the image most recently sent by `COVERART`,
so a display can tell whether the art on disk belongs to what's playing.

**Deliberately not in here: the audio format.** That lives in
`status.json`, written by the daemon from the stream it actually opened —
what the DAC is really receiving, not what the sender believes it sent. A
display should read format from there and everything else from here.

## Runtime files (display integration)

The daemon publishes what it knows under `/run/halo-daemon/`:

| file | written by | contents |
|---|---|---|
| `status.json` | daemon | playback state, real format, position, ring level, underruns |
| `metadata.json` | sender, via METADATA | the schema above, verbatim |
| `coverart.bin` | sender, via COVERART | image bytes as sent (JPEG/PNG — sniff or just hand to an image loader) |
| `coverart.sha256` | daemon | hex digest of `coverart.bin`, for change detection |

All writes are atomic (temp file + rename), so a reader never sees a
half-written file, and a display can simply poll or watch with inotify.
`COVERART` whose digest matches what is already stored is skipped without
rewriting, so re-sending the same art per track costs nothing on disk.

The split is deliberate: the daemon draws nothing itself. It runs at
`SCHED_FIFO` with one job — keep ALSA fed — and linking a graphics stack
into that process would put allocation and CPU contention on the audio path
and force its systemd sandbox open for GPU access. A display is a separate
program, in any language, that can crash and restart without interrupting
playback.

## Metadata size limits

`METADATA` is capped at 64 KiB and `COVERART` at 4 MiB. Anything larger must
be drained and discarded rather than buffered.

The cap exists because control and audio share one TCP stream and one serial
reader: a large image stalls audio delivery for exactly as long as it takes
to read off the socket. What actually matters is that the stall stays well
inside the daemon's ring depth (~5 s of audio) — 4 MiB is about 1.7 s even
on a weak 20 Mbps link, so it is safe with plenty of margin, and generous
enough for art displayed at panel resolution on the endpoint itself.

Timing matters more than size. Sent once per album, before the first
`FORMAT`, the read costs nothing — no audio is flowing yet. Per-track art
is the case to watch: gapless preannounces the next track while the current
one is still playing, so that art necessarily arrives mid-stream.

The daemon writes the latest of each to its runtime directory
(`/run/halo-daemon/metadata.json`, `/run/halo-daemon/coverart.bin`, plus
`coverart.sha256`) so a local display can just read files rather than speak
this protocol. Writes are atomic (temp file + rename), and an image whose
SHA-256 matches the one already stored is skipped without rewriting.
(The v2 proposal's channel split removes the stall constraint properly.)

## Seek / flush sequencing

1. Sender wants to jump to a new position: it stops sending `AUDIO_DATA`,
   sends `FLUSH`.
2. Daemon calls `snd_pcm_drop()` (discard whatever's buffered, don't wait for
   it to play out), then `snd_pcm_prepare()`, then replies `FLUSH_ACK`.
3. Sender waits for `FLUSH_ACK` before sending any more `AUDIO_DATA` for the
   new position. It must also ignore any `POSITION` whose header `seq` is
   below the `FLUSH_ACK`'s: the daemon resets `frames_written` at the flush,
   but a `POSITION` sent just before it carries the old, much larger value
   and arrives after. Applied blindly it makes the sender believe playback
   has jumped far ahead, so its "how far ahead have I sent" figure goes
   negative and it briefly floods audio. This is the same stale-message
   hazard as the pre-flush audio barrier below, in the opposite direction. Sending data before the ack arrives is undefined —
   whichever bytes the daemon hasn't dropped yet vs. the new bytes can
   interleave.

## Pause semantics

### The pre-flush audio barrier

Step 1 above hides a race that bit the reference implementation. A sender
that keeps a deep decode pipeline has audio queued internally when the user
seeks, and it deliberately does **not** send `FLUSH` behind that queue —
control has to jump ahead of it or seeks would take as long as the buffer
is deep. So `FLUSH` reaches the socket first, but audio written *before* it
is still in flight and arrives *after* it.

That late audio still carries the pre-seek `format_id`, which is still the
daemon's active format until the new `FORMAT` arrives — so a daemon that
only checks `format_id` accepts it and plays it at the new position. The
audible result is a short fragment of the old position after every seek.

**Daemons must therefore drop `AUDIO_DATA` whose header `seq` is lower than
the `seq` of the most recent `FLUSH`.** Because `seq` increases in write
order on a connection, "sent before the flush" is exactly `seq < flush_seq`,
and this needs no new message or state beyond remembering that one number.
Reset it per connection (`seq` restarts with each connection).

Senders should also discard their own internally-queued audio at seek
rather than sending bytes destined to be dropped — but the barrier is what
makes correctness independent of how well any sender does that.

`PAUSE` stops the daemon from calling `snd_pcm_writei` (it keeps reading off
the socket into the ring buffer so the TCP window doesn't stall, it just
stops draining into ALSA) without dropping the ALSA buffer, so `RESUME`
picks back up with zero data loss and no re-handshake needed.

The paused state is **cleared** by exactly three messages:

- `RESUME` (the obvious one),
- `FLUSH` (a seek is about to start a new stretch of playback; the sender
  re-pauses explicitly afterwards if it was seeking while paused), and
- a non-preannounce `FORMAT` (a fresh active stream is an unambiguous
  statement of intent to play — without this rule, a daemon paused during
  track N would silently swallow all of track N+1's audio while every
  other part of the pipeline looked healthy, which is exactly the failure
  mode this rule exists to kill).

`SWITCH_TO_PENDING` **preserves** the paused state: a gapless handover is a
continuation of the same listening session, so pausing right at a track
boundary stays paused across it.

## Position / underrun reporting

Daemon sends `POSITION` roughly every 200ms from the ALSA writer thread
(frames actually handed to `snd_pcm_writei`, not frames received over the
network — this is what makes it a trustworthy "what's actually audible now"
signal for the sender's UI). While paused or starved, `frames_written` does
not advance — POSITION must never be a wall-clock estimate, or the two
sides' pacing accounting drifts apart across every stall. On `-EPIPE` from
`snd_pcm_writei`, daemon calls `snd_pcm_prepare()` to recover and sends
`UNDERRUN` immediately, out of band from the regular POSITION cadence.

### Position epochs

`frames_written` counts frames **of the current active stream only**,
starting from 0. The counter resets to 0 whenever the active stream
changes or restarts:

- the first `FORMAT` of a connection,
- every subsequent non-preannounce `FORMAT` (hard cut to a new stream),
- a gapless promotion (`SWITCH_TO_PENDING`, or a fallback inference), and
- `FLUSH` (seek — playback restarts at a new position within the same
  stream).

The sender mirrors these resets in its own enqueued-frames accounting, so
"how far ahead of audible playback have I sent?" is always a same-epoch,
same-unit subtraction on both sides. The sender must ignore POSITION whose
`format_id` differs from what it currently considers active (stale reports
from the previous epoch can legitimately still be in flight right after a
switch).

## What v1 deliberately does not do

No compression, no encryption, no multi-endpoint fan-out, no service
discovery (mDNS can be layered on top later without touching this protocol —
it would just be how the sender learns the IP before opening the TCP
connection), no volume control in-protocol (do it in the decode chain before
data ever reaches this protocol, keeps the daemon a dumb pipe).

---

# v2 proposal — control/data channel split

**Status: design only, not implemented.** v1.x (everything above) is what
ships today. This chapter records the design agreed for v2 so it can be
implemented against real measurements rather than re-derived later.

## Why

Control latency (how long after the user presses pause/seek/next before the
speaker responds) has three independent sources. Only the third is a
protocol problem, and v1's single-stream design is what makes it one:

1. **Sender application queue** — control messages queued behind audio
   inside the sender. Fixed in v1.x: control sends bypass the audio FIFO
   entirely.
2. **Sender-side decode occupancy** — the sender's playback engine blocked
   on synchronous decode, so the control call couldn't even be issued.
   Fixed in v1.x by moving decode off the engine's actor.
3. **Wire backlog** — control shares one in-order TCP stream with
   AUDIO_DATA, so a control message cannot overtake audio bytes already
   written to the socket. v1.x bounds this (in-flight AUDIO_DATA is capped
   at a *duration* budget, ~200 ms, rather than a fixed byte count that
   meant ~6 s for CD PCM and ~0.4 s for DSD128), but bounding is not
   eliminating, and the bound is in direct tension with wanting a deep
   daemon ring for jitter tolerance.

On one stream those two goals — deep endpoint buffer, instant control —
trade against each other. On two they are independent. That is the whole
motivation; everything below follows from it.

## Prior art, and what it does not cover

The split itself is well-established: RTSP carries control on TCP while RTP
carries media separately; SlimProto puts control on TCP 3483 with audio on
a wholly separate HTTP connection; RAAT treats "quick response to play,
pause, and volume" as an explicit design goal. QUIC generalises it —
independent streams within one connection, so loss on one does not block
another — and is what a protocol designed today would reach for.

But those protocols were designed between 1998 and 2015 for lossy networks
and CBR media, and none of them address what actually cost the most
debugging time here: **both endpoints hold state, and v1 had no mechanism
to reconcile them when they diverged.** Every serious bug in this protocol's
first implementation was a state desync — sender believing format N+1 was
active while the daemon still played N; the daemon's pause flag stuck on
while the sender believed it was playing; the sender's pacing counter
frozen because it was rejecting position reports from an epoch it had
already left. Splitting the transport does not fix any of those. So v2 is
four changes, not one.

## Pillar 1 — logical channels, transport-agnostic

v2 defines two *logical* channels and does not mandate how they are carried:

- **control**: HELLO, CAPS, FLUSH/FLUSH_ACK, PAUSE, RESUME,
  SWITCH_TO_PENDING, CANCEL_PREANNOUNCE, POSITION, UNDERRUN, STATE, BYE.
  Low rate, latency-critical.
- **data**: FORMAT, AUDIO_DATA, METADATA, COVERART. High rate,
  throughput-critical, latency-tolerant.

Initial mapping is two TCP connections, correlated at setup by a session id
the daemon issues in CAPS on the control connection and the sender echoes
in the data connection's HELLO. A QUIC mapping (two streams on one
connection) can be added later without touching a single message
definition. Hardcoding the transport is precisely why the older protocols
aged badly; the message layer should outlive the transport choice.

## Pillar 2 — credit-based flow control

v1 paces by inference: the sender estimates how far ahead it is from
POSITION reports and its own sent-frame count. That estimate is where the
pacing bugs lived — it depends on both sides agreeing about epochs, units,
and resets, and it silently wedges when they do not.

v2 replaces it with explicit credit, the mechanism HTTP/2 and QUIC already
use for exactly this problem. The daemon advertises how much room its ring
has (`CREDIT{format_id, frames}`), the sender may send at most that much,
and the daemon issues more as the ring drains. No estimation, no epoch
arithmetic on the sender side, and buffer depth becomes a property the
daemon declares rather than something the sender guesses. The sender's
"how many seconds ahead am I" heuristic disappears entirely.

## Pillar 3 — scheduled control, not event control

v1 commands mean "do this when you receive it", which makes their effect a
function of network timing. v1.x already moved the worst case off inference
(SWITCH_TO_PENDING replaced "daemon guesses the old stream ended"), but the
general form is better: control messages carry an execution point.

`{action, at_frame}` where `at_frame` may be `IMMEDIATE`. Gapless becomes
`SWITCH_AT(format_id, frame)` — sample-accurate by construction, with no
drain heuristic on either side. Pause/seek normally stay IMMEDIATE. The
payoff is determinism: the same command sequence produces the same audible
result regardless of when it lands, and the whole class of "who decides the
boundary" races stops existing.

## Pillar 4 — state reconciliation

The largest lever. Not novel — but the prior art for it lives one layer up
from where this protocol has been looking.

RTSP, RAAT and SlimProto are *transport* protocols, and none of them carry
authoritative endpoint state; that is why the earlier survey found nothing.
The device-control protocols layered above transports solved it long ago
and converged on the same shape. Google Cast has the receiver broadcast
`MEDIA_STATUS` (playerState, currentTime, mediaSessionId) to every sender,
carrying the `requestId` of the command that caused the change — or `0`
when the receiver changed state on its own. UPnP AV does it through GENA
eventing: on subscribe the renderer immediately pushes its **entire** state
table, and every subsequent event carries only what changed, explicitly so
that all control points keep a consistent view without polling.

HALO needs this because it is trying to be a transport *and* a control
plane at once. Adopt the established shape:

- **Full snapshot on connect, deltas afterwards** (UPnP's rule). Makes
  reconnect-after-drop correct by construction rather than by special-case
  recovery code.
- **Correlate status with the command that caused it** (Cast's `requestId`).
  HALO already has a per-connection `seq` in every header, currently
  documented as logging-only — have `STATE` echo the `seq` it is responding
  to, or `0` for spontaneous change. This distinguishes "the daemon
  acknowledged what I asked" from "the daemon changed on its own" from "this
  is a stale report from before my command", which is exactly the ambiguity
  that made the v1 desyncs hard to diagnose.

Concretely: the daemon emits `STATE` on the control channel periodically
(~1 s) and immediately after any state-changing message — active format_id,
pending format_id, ring occupancy, paused flag, position epoch and frames.
The sender reconciles against it instead of assuming its own model is
correct.

(The third option in the literature is to have no shared state at all:
HLS/DASH are stateless client-pull, so nothing can desync. That is not
available here — the daemon is deliberately a dumb sink that cannot fetch
or decode, so the sender must hold the playback state.)

Every desync bug from v1's implementation self-heals under this rule within
one STATE interval, without a timeout heuristic and without user
intervention. It converts the protocol from a fragile command sequence into
a convergent state machine, which is the right shape for two independent
processes that each hold real state.

## Ordering hazard this introduces

Splitting channels removes the ordering guarantee that v1 got for free, and
the affected message is FLUSH. Once control can overtake data, a FLUSH may
arrive while pre-seek AUDIO_DATA is still in flight on the data channel; if
the daemon simply drops what it holds and then accepts what arrives next,
stale audio lands at the new position.

The fix reuses machinery that already exists: `format_id` is monotonic and
every seek already emits a new one (a hard-cut FORMAT), so FLUSH carries
`discard_before_format_id` and the daemon drops any AUDIO_DATA tagged older
than that, whenever it arrives. Sequencing becomes explicit rather than
implied by transport ordering — which is also what makes a future QUIC
mapping safe.

## Compatibility

CAPS gains a feature-flag word. A v2 sender talking to a v1 daemon sees no
control-channel flag, skips the second connection, and behaves exactly as
v1.x. A v1 sender against a v2 daemon works unchanged — the daemon simply
never sees a control connection and keeps its v1 inference paths, which is
why the fallback heuristics documented above are retained rather than
deleted.

## Sequencing

Implement in pillar order and measure between steps. Pillar 1 is the only
one that needs the second connection; pillars 2–4 are message-layer changes
that would improve v1's single-stream design on their own. Do not implement
any of it against loopback numbers — the wire-backlog term this is meant to
address is near zero on loopback and only becomes real on the Pi5 over
Wi-Fi.
