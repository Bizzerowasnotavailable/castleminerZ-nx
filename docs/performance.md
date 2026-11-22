# Performance measurement

The runtime host measures the game while it runs, so frame rate problems can be
traced on real hardware instead of guessed. Nothing here changes what the game
does. The code is in `runtime-probe/native/perf_monitor.cpp`.

## perf.log (on by default, toggleable)

Every 5 seconds the host appends one line to
`sdmc:/switch/castleminerz-logs/perf.log` (the file is rewritten at each launch).
Fields:

| Field | Meaning |
| --- | --- |
| `t` | Seconds since the host started. |
| `mode` | `docked` or `handheld`; GPU clocks differ between the two. |
| `fps` | Presented frames per second in the interval. |
| `frame_ms` | Present-to-present interval: average, p50/p95/p99, maximum, and how many frames took over 33.5 ms. Percentiles have 0.25 ms resolution below 50 ms. |
| `gfx_ms` | Main-thread time per frame inside FNA3D calls, excluding present. |
| `present_ms` | Time per frame inside `FNA3D_SwapBuffers`. |
| `<category>=a/b` | `a` ms per frame and `b` calls per frame for `draw`, `effect` (apply/technique/pass restore), `state` (blend, depth, rasterizer, viewport, scissor, vertex bindings), `sampler`, `upload` (vertex/index data), `texture` (texture data), `clear`, `target` (render targets, resolve), `resource` (create/dispose buffers and textures) and `readback` (GPU-to-CPU reads). |
| `prims`, `upload_kb`, `texture_kb` | Primitives drawn, and vertex/index and texture bytes uploaded, per frame. |
| `offthread` | FNA3D calls made by other threads in the interval and their total duration. FNA3D's OpenGL driver queues such calls and runs them only once per frame, inside `SwapBuffers`, so each one blocks its thread until the next present. |
| `main_cpu` | CPU time of the main thread over wall time. Near 100% means the main thread is CPU-bound. |
| `gc` | Mono collections started in the interval (`minor`, `major`), total stop-the-world time and the longest pause. The host leaves the nursery at SGen's default 4 MB; the major collector is SGen's default concurrent mark-sweep. |
| `audio` | Audio device callbacks (FAudio's mixer) in the interval, mixing time per callback (average and maximum), the highest output sample, callbacks that produced only silence, and the audio thread priority (`raised` = 0x2A). `audio closed` means no device was opened; `open-failed` counts failed opens, and `host.log` has SDL's reason. |
| `threads` | Managed threads that used at least 1% of a core, by name, and the number of live managed threads. |

Mesa acquires the next swap-chain buffer lazily, at the first use of the window
framebuffer in a frame. The wait for a free buffer (vsync) is therefore counted
under `clear`, normally in the frame's first `Clear`, not under `present_ms`.
A `clear` close to the frame budget with low `main_cpu` means the game is
waiting for vsync, not drawing.

FNA3D runs FNA3D calls made by other threads on the main thread. Upstream does
that only once per frame, in `SwapBuffers`; the port's FNA3D patch also runs
them before every effect pass, so `offthread` time per call should stay well
under a frame.

### Render Scale & Audio Fixes

- **Render scale**: Set to **75%** (`CMZ_RENDER_SCALE=75`) via FNA3D so FNA3D's faux backbuffer scales the image up to 1280×720 while the internal framebuffer is 960×540. This cuts terrain fragment work nearly in half without changing display mode. Implemented by patch `patches/fna3d/0003-render-scale.patch` (added to live tree) and reverted on FNA with `patches/fna/0004-render-scale.patch`.
- **Audio**: 1024-sample quantum passed to `FAudioCreate` (via `patches/fna/0005-audio-1024-quantum.patch`). SDL audio driver measured; mixer output recorded in `perf.log`.

Eden reports no thread CPU time, so `main_cpu` and `threads` read 0% there.

The FNA3D timing uses link-time wrappers (`-Wl,--wrap=FNA3D_...`).
`build-game.ps1` derives the wrapper list from `perf_monitor.cpp` and fails the
build if a wrapper lacks its compile-time signature check against `FNA3D.h`.

## Logging toggle

Host and performance logging can be turned off without rebuilding the NRO by
creating `sdmc:/switch/castleminerz/config.ini` on the SD card:

```ini
[logging]
logging=0
```

- `logging=0` disables host log creation (`host.log`) and the perf monitor
  threads/initialization. Any other value (including an absent file) leaves
  logging on by default.
- When disabled, the host skips the log directory, host log file, perf monitor
  initialization, and perf notes.

## Sampling profiler (opt-in)

Create `sdmc:/switch/castleminerz-logs/profile.ini` to enable it:

```ini
; all keys are optional
hz = 500          ; samples per second, 50..2000
threads = all     ; "main" (default) or "all" managed threads at a quarter of the rate
delay = 0         ; seconds to wait before sampling
duration = 0      ; seconds to sample, 0 = until exit
```

It writes `profile.bin` next to it, flushing about once a second. Each sample
pauses the thread with `svcSetThreadActivity`, reads its registers with
`svcGetThreadContext3`, walks the AArch64 frame-record chain within the
thread's stack mapping, and resumes it. Nothing allocates, locks or writes to
the SD card while a thread is paused. Delete `profile.ini` to switch it off;
sampling costs roughly 1-2% of the sampled thread's time.

Analyze on the build PC with the ELF and linker map of the **same** build:

```powershell
./scripts/analyze-profile.ps1 -Profile .\profile.bin `
    -Elf <workspace>\cmz-game-stage\native\aot\castleminerz.elf -Thread main
```

The map defaults to `build\castleminerz.map` next to the ELF. The report lists
time by module (AOT assembly, Mono runtime, Mesa, libnx, FNA3D, ...), the top
functions exclusive and inclusive, and the callers of the hottest functions.
Keep a copy of the ELF and map of any NRO you test; the next build replaces the
staging directory, and offsets from another build symbolize to the wrong code.
The ELF, map and profile contain or describe private game code; never commit or
publish them.

## Findings so far

See the November 21, 2026 entry in [`JOURNEY.md`](../JOURNEY.md). In short, for
the title screen in Eden: the main thread waits for vsync most of the frame;
worker threads issue hundreds of FNA3D calls per second that each wait up to a
frame; loading triggers bursts of major collections with pauses up to 38 ms.
Hardware measurements are still needed before changing behavior.
