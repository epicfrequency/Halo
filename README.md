# HALO Audio Transport

Pi5-side receiver for a custom point-to-point hi-res/DSD audio protocol
(codename HALO). One macOS sender (your player), one Pi5 + DAC endpoint,
no third-party ecosystem in the middle — see `PROTOCOL.md` for the wire
format and the design rationale.

Status: **working foundation, not yet hardware-validated.** Every line
compiles clean (`-Wall -Wextra`, zero warnings) against real ALSA headers,
and the full control-plane round trip (HELLO→CAPS, FORMAT, AUDIO_DATA,
FLUSH→FLUSH_ACK, PAUSE/RESUME, BYE) has been exercised end-to-end with a
throwaway Python client — see `tools/smoke_test_client.py`. What has **not**
been tested yet is a real DAC on real Pi5 hardware: no sandbox here has
sound hardware attached, so `snd_pcm_open` failing gracefully is as far as
verification could go without you plugging in the real thing.

## Offline self-tests

```
make check          # seconds, no dependencies beyond a C compiler + python3
make check-linux    # stricter: real libasound, Pi architecture, needs docker
```

`make check` builds against a bundled ALSA stub (`tools/alsa-stub/`).
`make check-linux` builds against the **real** libasound headers on aarch64
in a container, which additionally exercises the `snd_pcm_open` failure path
— a container has no sound card, so every open genuinely fails, which is the
only way to test `FORMAT_REJECTED` off-hardware.

Both are verified against Debian bookworm (gcc 12 / libasound 1.2.8) and
trixie (gcc 14 / libasound 1.2.14) with `-Werror`. DietPi is Debian-derived
and takes its toolchain from the same repositories, so this covers it;
override the base with `HALO_CHECK_IMAGE=... make check-linux` if needed.
No docker? `brew install colima docker && colima start --arch aarch64`.

Builds the daemon against a bundled ALSA stub (`tools/alsa-stub/`) and drives
the running binary through every message in PROTOCOL.md, asserting the
replies and the runtime files it publishes. No ALSA, no DAC, no root, no Pi —
runs on a Mac or any Linux box in a couple of seconds, with `-Werror`.

It catches type errors, missing declarations and protocol-logic regressions.
It cannot catch what only real hardware shows: `hw_params` negotiation,
whether the DAC really accepts native DSD, DSD bit order, or how it sounds.
The stub says yes to everything.

## Native DSD format (handled for you)

Which ALSA format a DAC accepts native DSD as is a property of the device,
and picking the wrong one is **not** a graceful failure — the byte order is
reversed and you hear noise, with no error anywhere.

The daemon settles this at runtime, twice: it probes every packing it can
drive when a sender connects, and if the one it picked turns out not to open,
it walks the rest before giving up. Nothing is chosen at build time, so the
same binary drives any DAC — build it once, copy it anywhere.

To see what your device reports, if you're curious:

```
cat /proc/asound/card0/stream0
```

```
    Altset 3
    Format: SPECIAL DSD_U32_BE      <- this is what matters
    DSD raw: DOP=0, bitrev=0        <- DOP=0 means real native DSD;
                                       bitrev=0 means the driver passes bit
                                       order through unchanged
```

A device that lists no `DSD_*` altset has no native DSD at all — use DoP or
PCM in the sender's HALO DSD Mode. `S32_LE` alone is normal and fine.

## Running alongside MPD (or any other ALSA player)

No conflict in service name, port (5555 vs MPD's 6600), Avahi service type,
user or runtime directory. The one shared resource is the ALSA device, and
`hw:` devices are exclusive — one process at a time.

That works out cleanly because of *when* halo-daemon holds the device: it
queries capabilities at startup and immediately closes, opens for real only
when the sender announces a FORMAT (i.e. playback actually starts), and
closes again as soon as the client disconnects. So both services can stay
enabled — whichever is playing owns the DAC, and it is handed back when that
player stops. No `systemctl stop` dance needed.

If the other player *is* holding the device, the open fails with EBUSY and
the sender is told (`FORMAT_REJECTED`, reason `DEVICE_BUSY`) instead of
hanging, so it surfaces as a clear message rather than a stuck play button.

One MPD setting to check: `always_on "yes"` in an `audio_output` block makes
MPD hold the device permanently, which would starve everything else.

```
grep -A6 audio_output /etc/mpd.conf | grep -i always_on
```

## DietPi notes

DietPi is a minimal image, so three things Raspberry Pi OS gives you for free
are not there by default:

| Need | Why | Install |
|---|---|---|
| `libasound2-dev`, gcc/make | to build at all | `apt install libasound2-dev build-essential` (or `dietpi-software install 17` for the toolchain) |
| `avahi-daemon` | **the entire discovery mechanism** — the daemon does no mDNS itself | `dietpi-software install 152` |
| `alsa-utils` | `aplay` for inspecting device capabilities | `apt install alsa-utils` |

Avahi is the one that bites: without it the daemon runs perfectly and the Mac
app simply never lists the endpoint, with no error on either side. `install.sh`
checks for it and says so rather than letting you discover that the hard way.

DietPi normally logs you in as root, so `./install.sh` works directly — `sudo`
is only needed if you've set up a non-root user.

## Quick start

One command from a fresh machine to a running endpoint — it installs whatever
is missing (compiler, ALSA headers, Avahi), builds, lists your DACs so you can
pick one, and registers the service:

```
sudo ./install.sh
```

`install.sh` also takes flags for unattended use:

```
sudo ./install.sh --device hw:1,0 --port 5555 --yes
```

It is safe to re-run to upgrade an existing install (it stops the service
first, so replacing the binary can't fail with ETXTBSY). `sudo ./uninstall.sh`
removes everything.

Why use it rather than the manual steps below: the ALSA device and TCP port
have to appear in *two* files — the systemd unit's `ExecStart` and the Avahi
`<port>` — and editing one but not the other produces a daemon the sender can
discover but not connect to (or the reverse). `install.sh` generates both from
the same pair of values, and verifies the substitution actually took.

The manual steps below are still accurate if you'd rather do it by hand or
need to adapt it to a non-systemd setup.

## Build (on the Pi5)

```
sudo apt install libasound2-dev build-essential
make
```

## Run

```
./halo-daemon hw:1,0 5555
```

First arg is the ALSA device (find yours with `aplay -l`, then use
`hw:CARD,DEVICE` — not `plughw` or `default`, see PROTOCOL.md / README
below for why). Second arg is the TCP port (defaults to 5555 if omitted).

Find your real device name and confirm what it actually supports before
wiring up the sender side:

```
aplay -l
aplay --dump-hw-params -D hw:1,0 /dev/null
```

That second command is the one that tells you the truth about whether your
DAC's driver exposes native DSD formats (`DSD_U8`/`DSD_U16_LE`/`DSD_U32_LE`)
or only PCM — don't assume, check.

## Running as a systemd service

`systemd/halo-daemon.service` runs the daemon as an unprivileged `halo`
user rather than root, using `AmbientCapabilities=CAP_SYS_NICE` +
`LimitRTPRIO`/`LimitMEMLOCK` so `pthread_setschedparam(SCHED_FIFO)` and
`mlockall()` in `halo_set_realtime_priority()` actually succeed instead of
printing the "could not set SCHED_FIFO" warning you'll see if you just run
the binary by hand as your own user.

```
sudo useradd --system --no-create-home --shell /usr/sbin/nologin halo
sudo usermod -aG audio halo

sudo cp halo-daemon /usr/local/bin/halo-daemon
sudo cp systemd/halo-daemon.service /etc/systemd/system/

# edit the ExecStart line first — it hardcodes hw:1,0 and port 5555,
# change to match your actual `aplay -l` output
sudo systemctl edit --full halo-daemon.service   # or just edit the file directly

sudo systemctl daemon-reload
sudo systemctl enable --now halo-daemon
```

Check on it:

```
systemctl status halo-daemon
journalctl -u halo-daemon -f
```

`main.c` installs handlers for `SIGINT`/`SIGTERM` (no `SA_RESTART`, so the
blocking `accept()`/`recv()` calls wake with `EINTR` instead of restarting)
so `systemctl stop halo-daemon` / a reboot triggers a clean exit rather
than a hard kill — you'll see `halo: shutdown signal received, exiting
cleanly` in the journal instead of nothing.

## Discovery (mDNS/Bonjour) — no more hardcoded IPs

`avahi/halo-daemon.service` is a static Avahi service definition, not code.
Avahi is already running on virtually any Raspberry Pi OS install and
watches `/etc/avahi/services/` — drop the file in, it starts advertising
immediately, no daemon restart, no changes to `halo-daemon` itself needed.
This is deliberately not implemented as custom mDNS code in `main.c`: mDNS
(RFC 6762/6763) has enough probing/conflict-detection/TTL subtlety that
reusing Avahi (mature, already on the box) beats hand-rolling it, and Avahi
and Apple's Bonjour are the same protocol — a macOS `NWBrowser` looking for
`_halo._tcp` finds an Avahi-advertised service with zero extra software.

```
sudo apt install avahi-daemon avahi-utils   # avahi-daemon usually preinstalled on Raspberry Pi OS
sudo cp avahi/halo-daemon.service /etc/avahi/services/
avahi-browse -r _halo._tcp   # confirm it's visible, from another machine on the LAN too
```

If you ever change the port in the systemd unit's `ExecStart`, update the
`<port>` in `avahi/halo-daemon.service` to match — nothing cross-checks
these two right now.

**macOS (sender) side** — not built here (this repo is the Pi5 half only),
but the shape of it with `Network.framework`:

```swift
import Network

let browser = NWBrowser(for: .bonjour(type: "_halo._tcp", domain: nil), using: .tcp)
browser.stateUpdateHandler = { state in /* handle .failed, .ready, etc. */ }
browser.browseResultsChangedHandler = { results, _ in
    for result in results {
        guard case let .service(name, _, _, _) = result.endpoint else { continue }
        // Resolve the endpoint to get host+port before connecting:
        let connection = NWConnection(to: result.endpoint, using: .tcp)
        connection.stateUpdateHandler = { state in
            if case .ready = state {
                // send HALO_MSG_HELLO here, then proceed per PROTOCOL.md
            }
        }
        connection.start(queue: .main)
        // In practice: surface `name` (e.g. "HALO Audio Transport on raspberrypi")
        // in a picker UI rather than auto-connecting to the first result —
        // there's nothing stopping more than one halo-daemon showing up on
        // a LAN, discovery just answers "which one," playback is still
        // strictly one-endpoint-at-a-time (see PROTOCOL.md).
    }
}
browser.start(queue: .main)
```

Not live-tested against a running `avahi-daemon` in this environment (no
D-Bus/Avahi stack in the sandbox this was built in) — the service file
itself is validated as well-formed XML matching Avahi's documented schema,
but do run `avahi-browse -r _halo._tcp` from a second machine once this is
on real hardware to confirm it actually announces.

## Things you will likely need to tune per-DAC

- **Buffer/period sizing** in `open_locked()` (`src/alsa_output.c`):
  currently targets a 500ms ALSA buffer / 8 periods. This is a starting
  point for "never underrun on a home LAN," not a measured optimum — once
  you have real hardware, watch for `UNDERRUN` messages and adjust.
- **Real-time priority**: `halo_set_realtime_priority()` needs
  `CAP_SYS_NICE` and a raised `RLIMIT_MEMLOCK` to actually take effect —
  see "Running as a systemd service" below, the provided unit grants both.
  Without it, the daemon still runs correctly, just with weaker jitter
  guarantees under system load — the warnings you'll see at startup are
  exactly this.

## What's a stub / explicitly not implemented

- **DoP (DSD-over-PCM) encoding** (`HALO_FMT_DSD_DOP` in the protocol) is
  wired up structurally (format negotiation, rate math) but the actual
  0x05/0xFA marker-byte interleaving is **not implemented** —
  `pcm_format_for()` / `alsa_rate_for()` treat it as a 32-bit PCM container
  without writing the markers. Given your Pi5 setup confirmed native DSD
  support, this was deprioritized. If you ever need DoP for a different
  DAC, that's the one piece of real DSP work left — everything else in the
  daemon is agnostic to it.
- **mDNS/discovery**: the sender is expected to know the Pi5's IP. Layer
  Avahi/Bonjour advertisement on top later if needed — it wouldn't touch
  this protocol at all, just how the sender learns the IP before dialing.
- **Multi-endpoint / multi-room**: intentionally out of scope — see
  PROTOCOL.md's "what v1 deliberately does not do."

## Architecture in one paragraph

`main.c` owns two ring buffers (`ring[0]`/`ring[1]`, lock-free SPSC, see
`ring_buffer.h`) and an `active_idx` pointing at whichever one is currently
feeding ALSA. The network-reading thread (the connection's main loop)
writes incoming `AUDIO_DATA` bytes into whichever ring matches the
message's `format_id` — either the active stream or a preannounced
"next track." A dedicated `alsa_writer_thread` drains the active ring into
`snd_pcm_writei()` in a loop; when that ring empties and a preannounced
next track is waiting, it either just flips `active_idx` (same format —
zero-gap) or drains+closes+reopens ALSA first (format changed — one
unavoidable small gap). A third thread reports playback position back to
the sender every 200ms so the sender's UI reflects what's actually audible,
not what's been decoded locally.

## Integration on the macOS (Audio Lounge) side

Not built here — this repo is the Pi5 half only. What the sender needs to
implement, in order:

1. TCP client, `TCP_NODELAY` on, connects to the Pi5's IP:port.
2. Send `HELLO`, parse `CAPS`, and refuse to request any format the CAPS
   response didn't advertise.
3. Tap your existing decode pipeline's output — the same buffer that
   currently feeds your local CoreAudio output — and mirror it into
   `FORMAT` + `AUDIO_DATA` messages per `PROTOCOL.md`.
4. For gapless: when you know the next track's format and have decoded
   data ready, send a preannounced `FORMAT` (`is_preannounce=1`) + its
   `AUDIO_DATA` before the current track ends.
5. Drive your UI's playback position from incoming `POSITION` messages,
   not from local decode progress.
6. On seek: stop sending `AUDIO_DATA`, send `FLUSH`, wait for
   `FLUSH_ACK`, then resume sending from the new position.

## Files

```
PROTOCOL.md              wire protocol spec (read this first)
src/protocol.h            shared struct/enum definitions
src/ring_buffer.h          lock-free SPSC ring buffer
src/alsa_output.{h,c}      direct hw: ALSA device management
src/net_io.h               framed-message socket helpers
src/main.c                 TCP server, gapless state machine, threads
tools/smoke_test_client.py throwaway protocol exerciser (not a real sender)
systemd/halo-daemon.service systemd unit (unprivileged user + RT capabilities)
avahi/halo-daemon.service  mDNS/Bonjour service advertisement (static, no code)
Makefile
```
