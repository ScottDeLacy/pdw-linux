# pdw-linux

A Linux/macOS port of [PDW](https://discriminator.nl/pdw/index-en.html), the
POCSAG/FLEX pager decoder, so it can run with an RTL-SDR dongle instead of a
Windows sound card. PDW was written by Jason Petty and Peter Hunt and open-sourced
under the GPL in 2013 — see [NOTICE](NOTICE). This port keeps their decoding code
essentially verbatim (see "How it is built") and adds a portable front end.

**Status: early (slice 1).** Headless CLI + library. The GUI, filters, logging,
ACARS, Mobitex and ERMES are not ported yet.

## Use

```sh
cmake -S . -B build && cmake --build build
# live, from an RTL-SDR (any FM-discriminator audio source works):
rtl_fm -f 152.4M -M fm -s 22050 -g 40 | build/pdw-cli --rate 22050 -
# from a recording:
build/pdw-cli capture.wav
build/pdw-cli --json --notices capture.wav
```

Input is mono audio from an FM *discriminator*. Any WAV sample rate works; it is
resampled to the 44.1 kHz PDW expects. If decoding is flaky, adjust `--gain` or try
another `--config` level preset (same list as PDW's Interface dialog).

## Tests

`python3 tests/run_tests.py build/pdw-cli [--multimon]` — generates synthetic
POCSAG (512/1200/2400) and FLEX (1600) audio at several sample rates, both
polarities and with noise, and checks the decoded messages. `--multimon` also
cross-checks the generated audio with `multimon-ng`.

## What is verified, and what is not

Verified (synthetic audio, x86-64 Linux): POCSAG 512/1200/2400 alpha, numeric and
tone-only; FLEX 1600 2-level alpha; polarity inversion; 22.05/44.1/48 kHz input.

**Not verified yet:** real radio signals; FLEX 3200/6400 and 4-level; FLEX numeric
and group calls; ARM and macOS builds; long-run soak.

## How it is built

`tools/import_upstream.sh` copies the upstream decoder files (`Pocsag.cpp`,
`Flex.cpp` and the signal-processing parts of `sound_in.cpp`/`Misc.cpp`) with only
mechanical edits, and a small Win32 shim (`libpdw/compat/`) lets them compile
unchanged. `libpdw/src/support.cpp` replaces the GUI/logging layer with a plain
message callback. Upstream state is global, so one decoder per process.
