# Offline ALSA stub

Enough of `alsa/asoundlib.h` to compile, link and *run* halo-daemon on a
machine with no ALSA — a Mac, or a Linux box without `libasound2-dev`. The
stub device accepts every format and consumes every write instantly.

Point: catch type errors, missing declarations, wrong argument counts and
protocol-logic regressions in seconds, locally, instead of discovering them
on the Pi. Run it with `make check` from the project root.

What it does **not** test, and cannot: real `hw_params` negotiation, whether
the DAC actually accepts native DSD, DSD bit order, and sound quality. The
stub says yes to everything; real hardware does not.
