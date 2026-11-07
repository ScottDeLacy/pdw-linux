# pdw-linux

A Linux/macOS (64-bit) port of [PDW](https://discriminator.nl/pdw/index-en.html), the
POCSAG / FLEX / ACARS / Mobitex / ERMES pager decoder, driven by an RTL-SDR dongle instead of
a Windows sound card. PDW was written by Jason Petty and Peter Hunt and open-sourced under the
GPL in 2013; see [NOTICE](NOTICE). This port keeps their decoding code essentially verbatim
(see "How it is built") and adds a portable Qt front end and a command-line decoder.

It reads and writes PDW's own files (`pdw.ini`, `filters.ini`, log files), so an existing
Windows PDW folder works as it is.

**Status: feature-complete, tested on synthetic signals only.** No real radio signal has been
run through it yet; see "What is verified".

## Build

Needs CMake, a C++20 compiler and, for the GUI, Qt 6 (Widgets, Multimedia). OpenSSL is optional
(TLS for the e-mail forwarder).

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-multimedia-dev libssl-dev rtl-sdr
# macOS: brew install cmake ninja qt openssl rtl-sdr

cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build            # QT_QPA_PLATFORM=offscreen for the GUI tests on a headless box
sudo cmake --install build        # optional
```

Without Qt only `pdw-cli` is built (`-DPDW_BUILD_GUI=OFF` forces that).

## Use

GUI: `pdw --rtl 152.4` tunes the dongle with `rtl_fm` and decodes. File or pipe input also work:

```sh
pdw --play capture.wav                       # add --fast to skip real-time pacing
rtl_fm -f 152.4M -M fm -s 22050 -g 40 | pdw --stdin --rate 22050
```

Setup dialog: choose the source (rtl_fm with frequency/gain/ppm/squelch, a sound input
device, or none), audio level preset, polarity, DC removal and input gain. All other dialogs,
menus, filters and options are PDW's own, built from its resource script.

Command line (headless; messages go to stdout):

```sh
rtl_fm -f 152.4M -M fm -s 22050 -g 40 | build/pdw-cli --rate 22050 -
build/pdw-cli --json --dir ~/pdw capture.wav
```

`pdw-cli --help` lists everything (`--dir`, `--filters`, `--set KEY=VALUE` for any pdw.ini
setting, `--record`, `--stats`, mode switches, ...). The data folder defaults to
`~/.local/share/pdw` for the GUI.

The decoder wants audio from an FM *discriminator*. FM audio often carries a DC offset, so a
DC blocker is on by default for piped input (`--no-dc-block` to turn it off). If decoding is
flaky, adjust `--gain` or try another audio level preset.

## What is verified

Everything below was checked with generated signals (`tests/gen/`, written from the public
protocol descriptions, not from PDW code), under normal and ASan/UBSan builds:

- POCSAG 512/1200/2400 (alpha, numeric, tone); FLEX 1600/3200/6400, 2- and 4-level, alpha,
  numeric, tone, short instructions, fragments, group calls; ACARS; noise and burst robustness
  in every mode; polarity inversion; 22.05/44.1/48 kHz input; DC offset.
- Filters, labels, reject, monitor-only, blocking, logging, `pdw.ini` round trip, command
  file, filter sounds, language tables, statistics, SMTP including STARTTLS.
- Every dialog opens, and pressing OK changes nothing.

**Not verified:** real signals; macOS and aarch64 builds (CI builds them, nobody has looked at
the output); Mobitex and ERMES decoding beyond "does not crash"; long-run soak.

## Known limitations (inherited from the original)

- The sound-card path slices FSK into two levels, so FLEX 4-level phases B and D are not
  decoded, and 3200/2 and 6400/4 decode phases A and C only. ERMES is disabled on the audio
  path upstream.
- Which of FLEX phase A/C comes out first at 3200/6400 depends on sub-sample timing; in
  synthetic tests about 70% of alignments (100% at 48 kHz) decode both. The crossover skip is
  centred at 1089 samples (upstream: 1080).
- A few dropped characters on weak signals or collisions are normal. The original manages
  roughly 98% typical, over 95% worst case.
- Upstream's global state means one decoder per process.

## How it is built

`tools/import_upstream.sh` copies the upstream decoder files (pinned to commit `6e18d3c`) with
only mechanical edits (line endings, character set, `#include` paths, a handful of
documented bug fixes). A small Win32 shim in `libpdw/compat/` lets them compile unchanged.
`libpdw/src/support.cpp` replaces the GUI layer with message/line callbacks. The imported
files are checked in, so building needs no upstream checkout.

## Licence

GPL-3.0-or-later, same as upstream. Not imported on purpose: upstream's `helper_funcs.*`
(its header forbids redistribution); the few utilities in it are re-implemented.
