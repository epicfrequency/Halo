# Handing this off to Claude Code

This folder is the Pi5 half of HALO, a custom point-to-point hi-res
PCM/native-DSD network audio protocol built for Audio Lounge (a macOS
player currently limited to local DAC/speaker output via CoreAudio). The
Pi5 daemon is done, compiled, and protocol-level smoke tested. The macOS
sender side — the actual Audio Lounge integration — has not been written.
That's the part to hand to Claude Code, since it needs to live inside and
understand the real Audio Lounge codebase, which this conversation never
had access to.

## Before starting a Claude Code session

Move or copy this whole `pi5-audio-daemon/` folder somewhere inside (or
alongside) the Audio Lounge repo, so Claude Code's working directory can
actually read `PROTOCOL.md` / `src/protocol.h` when you reference them.
`docs/halo-protocol/` inside the repo is a reasonable spot.

## Suggested first prompt

Open a terminal in the Audio Lounge project root, run `claude`, and paste
something like this (fill in the bracketed parts — Claude Code hasn't seen
Audio Lounge's actual code, so it needs pointers into it, not a rewrite of
what's already in PROTOCOL.md):

```
I'm adding a new audio output target to Audio Lounge (a macOS music player
app). Right now it only outputs to the local DAC/speaker via CoreAudio.
The receiving end already exists: a Pi5 daemon speaking a custom TCP
protocol called HALO — bit-perfect hi-res PCM and native DSD, multi-channel,
gapless track transitions, position feedback. It's documented in
docs/halo-protocol/PROTOCOL.md and docs/halo-protocol/README.md ("Integration
on the macOS side" section). The authoritative wire structs are in
docs/halo-protocol/src/protocol.h — match these byte-for-byte, don't
reinterpret them.

Read PROTOCOL.md first, all of it, before writing anything — the gapless
preannounce timing, the "format_id is a routing key not a change flag"
rule, and the seek/flush ack sequencing are all easy to get subtly wrong
if skimmed.

What I need built inside Audio Lounge:
1. A TCP client (Network.framework / NWConnection) connecting to a Pi5's
   IP:port, TCP_NODELAY on.
2. HELLO -> parse CAPS -> never request a format CAPS didn't advertise.
3. Tap [WHERE AUDIO LOUNGE'S DECODE OUTPUT CURRENTLY FEEDS LOCAL COREAUDIO
   — point me at the file/class] and mirror that buffer into FORMAT +
   AUDIO_DATA messages, in addition to or instead of local playback.
4. Gapless handover: preannounce (is_preannounce=1) the next track's
   FORMAT + first AUDIO_DATA before the current track ends. Only one
   track can be preannounced at a time. format_id must increment on every
   FORMAT message, even for back-to-back identical formats.
5. Drive the playback-position UI from incoming POSITION messages, not
   local decode progress — there's a jitter buffer on the Pi5 end, local
   decode position lies about what's actually audible.
6. Seek: stop sending AUDIO_DATA, send FLUSH, wait for FLUSH_ACK before
   sending anything at the new position.

Before writing code: look at [WHERE AUDIO LOUNGE'S PLAYBACK ENGINE LIVES]
and propose where a new output implementation (parallel to the existing
CoreAudio path) should plug in, and flag anything in PROTOCOL.md that
doesn't fit Audio Lounge's existing architecture before committing to it.
```

## Files worth telling Claude Code to read, in order

1. `PROTOCOL.md` — the spec itself, read in full
2. `src/protocol.h` — authoritative struct/enum layout, must match byte-for-byte
3. `README.md` — build/run notes + the same 6-point integration list
4. `tools/smoke_test_client.py` — not Swift, but a working reference for
   the exact message sequencing (HELLO→CAPS→FORMAT→AUDIO_DATA→FLUSH→FLUSH_ACK)
