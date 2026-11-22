# Changelog

Source releases of castleminerZ-nx. There are no binary releases: the game NRO
embeds code compiled from your own copy of CastleMiner Z, so every user builds
it locally from their own package with `scripts/prepare-game.ps1` (see
[`docs/installation.md`](docs/installation.md)). Measurements and reasoning are
in [`JOURNEY.md`](JOURNEY.md) and [`docs/performance.md`](docs/performance.md).

## [0.5.2] - unreleased (awaiting hardware test)

Built as perf-test-7 for the owner's hardware test. Not pushed until approved.

### Fixed

- **Choppy audio.** The cause is SDL 2.28.5's Switch audio backend. It keeps
  only two wave buffers, and it waits until a newly queued buffer starts
  playing before the mixer may produce the next. It also notices each buffer
  change 5-10 ms late, so the device ran dry between buffers.
  `runtime-probe/native/switch_audio.cpp` replaces it at link time
  (`-Wl,--wrap=SWITCHAUDIO_bootstrap`). Up to four buffers stay queued (the
  audio renderer's limit for one voice), and each is refilled as soon as it
  has played. A slow mixer callback now adds latency, at most 85 ms, instead
  of a gap.

### Added

- **Frame rate for SaltyNX tools.** When SaltyNX is installed, the host
  publishes the frame rate in the shared-memory block that NX-FPS uses, so
  Status Monitor's FPS counter works for this homebrew. SaltyNX itself does
  not inject into title-replacement processes. The handshake runs on a
  background thread, and nothing changes without SaltyNX.
- **config.ini keys** `audio_buffers = 2..4` (default 4) and `saltynx = 0`.
- **perf.log** audio field: `underruns=`, `drops=` and `queue=`.

## [0.5.1] - 2026-11-22

Follow-up to the perf-test-4 and perf-test-5 hardware measurements. The build
of this version (perf-test-6) is **not yet tested on hardware**.

### Changed

- **Render scale moved into FNA3D** (`patches/fna3d/0003-render-scale.patch`).
  The host sets `CMZ_RENDER_SCALE=75`. FNA3D then renders into a 960×540 faux
  backbuffer, scales viewports and scissor rectangles to match, and blits the
  result up to the 1280×720 window. The game still sees a 1280×720 backbuffer,
  so menus and the HUD keep their layout. perf-test-5 scaled the game-visible
  backbuffer in FNA instead, which cropped the menus. That FNA patch was
  dropped.
- **1024-sample audio quantum** (`patches/fna/0005-audio-1024-quantum.patch`).
  FNA creates FAudio with `FAUDIO_1024_QUANTUM`, so SDL opens the Switch audio
  device with 1024-sample (21 ms) buffers instead of 480 (10 ms). With 480-sample
  buffers, perf-test-5 ran about 85 mixer callbacks per second for a device that
  needs 100, and the music was choppy.
- The NRO reports version 0.5.1 to hbmenu. It reported 0.1.0 before.
- `prepare-game.ps1 -Icon <image>` passes the image to `build-game.ps1`, which
  embeds it as the 256×256 hbmenu icon.

### Added

- **Logging toggle.** A `sdmc:/switch/castleminerz/config.ini` containing
  `logging = 0` turns off all logging: `host.log`, `perf.log` and its threads,
  FNA3D call accounting, and mono-nx's unbuffered `runtime.log`. In that case
  the host starts Mono with an embedded quiet runtime config and does not create
  the log directory. Logging stays on if the file is missing or the value is
  anything else. An `aot_config.ini` override in the log directory still takes
  precedence.

### Fixed

- `build-native-dependencies.ps1` judges the CMake builds by their exit code,
  so compiler warnings on stderr no longer abort it under Windows PowerShell.
- `.gitattributes` keeps text files LF in the repository whichever Git client
  commits them. `runtime-probe/native/main.cpp` had been committed with CRLF.

### Notes

- After changing anything under `patches/fna3d`, `patches/mojoshader` or
  `patches/faudio`, rebuild the native libraries with
  `build-native-dependencies.ps1`, or run `prepare-game.ps1` without
  `-DependenciesReady`. Otherwise the NRO links the previous libraries. The
  first perf-test-6 attempt would have linked a `libFNA3D.a` built before the
  render-scale patch.
- `build-runtime-probe.ps1` copies only part of the shared host sources and has
  not been updated for the performance monitor. It cannot build the current
  host. This does not affect the game build.

## Before 0.5.1 (untagged)

- **2026-11-22.** FAudio decodes the game's xWMA music and ambience with
  FFmpeg's `wmav2` (devkitPro `switch-ffmpeg`, `patches/faudio/0002`), and
  plays anything it cannot open as silence of the correct length. SDL's audio
  thread runs at priority 0x2A. FNA3D runs queued off-thread calls at each
  effect apply instead of only at present. SGen's nursery stays at its default
  4 MB, because a 16 MB nursery aborted `mono_jit_init` on hardware. Hardware
  test builds perf-test-2 through perf-test-5.
- **2026-11-21.** GOD-folder SD layout, with the processed game assemblies
  embedded in the NRO. Faster STFS reads. The game boots on the owner's Switch.
  xWMA voices first played as silence (`patches/faudio/0001`), which fixed the
  hardware crash when the title music started. Added the frame/GC/audio
  performance log and the sampling profiler (perf-test-1).
- **2026-10-28.** Project-authored replacement effects, local Switch profile
  integration, and the first build that was playable in Eden. Package-to-SD
  preparation (`prepare-game.ps1`) and installation verification
  (`verify-game-install.ps1`), with installation and troubleshooting docs.
- **2026-10-27.** Initial STFS package VFS, `cmztool`, XNA compatibility
  assembly and managed relinker.
