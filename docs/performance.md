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
| `audio` | Audio device callbacks (FAudio's mixer) in the interval, mixing time per callback (average and maximum), the highest output sample, callbacks that produced only silence, and the audio thread priority (`raised` = 0x2A). From 0.5.2 the project's audio backend adds `underruns` (times every queued buffer had played before the next one arrived, each an audible gap), `drops` (voice drops reported by the audio renderer) and `queue` (buffers x samples). `audio closed` means no device was opened; `open-failed` counts failed opens, and `host.log` has SDL's reason. |
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

### Render scale and audio buffer size (0.5.1)

- **Render scale.** The host sets `CMZ_RENDER_SCALE=75`. With
  `patches/fna3d/0003-render-scale.patch`, FNA3D's OpenGL driver allocates the
  backbuffer at 75% (960×540) and scales viewports and scissor rectangles into
  it. At present it blits the result to the 1280×720 window with linear
  filtering. The game and FNA still see a 1280×720 backbuffer, so menus keep
  their layout. This cuts fragment work by about 44% without changing the
  display mode. FNA3D accepts values from 25 to 100 and ignores anything else.
  Render targets are not scaled.

  A backbuffer readback (`GetBackBufferData`) still requests the game's
  1280×720 rectangle from the smaller buffer, so a screenshot taken that way
  would be partly undefined. No such read happened in the logged sessions
  (`readback=0`). perf-test-5 scaled the game-visible backbuffer in FNA
  instead, which cropped the menus; that patch was dropped.
- **Audio buffer size.** `patches/fna/0005-audio-1024-quantum.patch` creates
  FAudio with `FAUDIO_1024_QUANTUM`, so SDL opens the device with 1024-sample
  (21.3 ms) buffers instead of 480. The `# audio: device ... opened` header in
  `perf.log` shows the size SDL actually granted. A device that keeps up runs
  about 234 callbacks per 5-second interval at 1024 samples, or 500 at 480.

### Audio backend (0.5.2, awaiting hardware test)

Larger buffers alone did not stop the choppy audio, because the gaps come
from SDL 2.28.5's Switch backend. It keeps two wave buffers. After queuing one,
it waits until that buffer starts playing before the mixer may produce the
next. Its wait loop also checks renderer state from before each 5 ms renderer
frame. The disassembly of the linked `libSDL2.a` matches the source.

`runtime-probe/native/switch_audio.cpp` replaces that backend at link time
(`-Wl,--wrap=SWITCHAUDIO_bootstrap`):

- Up to four buffers stay queued, the renderer's limit for one voice.
- The oldest buffer is refilled at the first renderer frame after it has
  played.
- A slow mixer callback or a late wake-up adds latency instead of a gap. That
  is at most 85 ms with 4 x 1024 samples, or 64 ms with `audio_buffers = 3`.

In `perf.log`, `underruns=0` during play means the device never ran dry.

Eden reports no thread CPU time, so `main_cpu` and `threads` read 0% there.

The FNA3D timing uses link-time wrappers (`-Wl,--wrap=FNA3D_...`).
`build-game.ps1` derives the wrapper list from `perf_monitor.cpp` and fails the
build if a wrapper lacks its compile-time signature check against `FNA3D.h`.

## Logging toggle

All logging can be turned off without rebuilding the NRO. Create
`sdmc:/switch/castleminerz/config.ini` on the SD card:

```ini
[logging]
logging = 0
```

The host reads only the `logging` key. The key is case-insensitive, spaces
around `=` are optional, and the section name is not checked. A value starting
with `0` turns logging off. A missing file, a missing key or any other value
leaves it on.

With logging off:

- There is no `host.log` and no `perf.log`, and the reporter and sampler threads
  do not start.
- The FNA3D wrappers keep only the tick read before each call.
- There is no `runtime.log`: the host starts Mono with the quiet RomFS config
  (`romfs:/aot_config_quiet.ini`), which has no stdio redirect and turns
  mono-nx diagnostics off.
- The `castleminerz-logs` directory is not created.

An `aot_config.ini` override in that directory still takes precedence over both
embedded configs. Turn logging back on before collecting logs for a report.

## Other config.ini settings (0.5.2)

The same file holds two more keys. Both use the same rules as `logging`:

```ini
audio_buffers = 3   ; audio buffers kept queued, 2-4 (default 4)
saltynx = 0         ; do not publish the frame rate to SaltyNX tools
```

Fewer audio buffers lower the latency of sound effects (21 ms per buffer) and
leave less room for a slow mixer callback. Values outside 2-4 are ignored.

## SaltyNX frame rate (0.5.2, awaiting hardware test)

Status Monitor's FPS counter normally gets its numbers from SaltyNX's NX-FPS
plugin, which hooks a retail game's present calls from inside the game process.
SaltyNX does not inject into homebrew in title-replacement mode, so the host
publishes the same data itself (`runtime-probe/native/saltynx_fps.c`).

At startup a background thread connects to SaltyNX's `SaltySD` port. It
reserves an NX-FPS block in SaltyNX's shared memory, using commands 6 and 7,
which SaltyNX allows for any process. Every present then updates `FPS`,
`FPSavg`, the last ten frame times, `frameNumber` and `pluginActive`, as
NX-FPS does. This works with logging on or off.

Without SaltyNX the port does not exist and nothing else changes. With logging
on, `perf.log` has a `# saltynx:` line: the shared-memory offset in use, or the
reason there is none.

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

**Eden, title screen (November 21).** The main thread waits for vsync most of
the frame. Worker threads issue hundreds of FNA3D calls per second, and each
waits up to a frame. Loading triggers bursts of major collections with pauses
up to 38 ms. Eden does not model the Switch GPU, so its frame rates say little
about hardware.

**Hardware, owner's console (November 22; firmware 22.5.0, handheld).** The
figures below are for in-world play after loading:

| Build | Render scale | Frame time p50 | Load per frame | Main thread |
| --- | --- | --- | --- | --- |
| perf-test-4 | none | 34–83 ms; 68–83 ms at the heaviest load | 1.0–1.3 M primitives, 370–470 draws | waits in `clear` (22–56 ms); `main_cpu` 26–48% |
| perf-test-5 | 75%, FNA (game sees 960×540) | 33–51 ms; 50–51 ms at the heaviest load | 1.1–1.4 M primitives, 390–490 draws | waits in `present_ms` (18–34 ms); `main_cpu` 34–53% |

- **GPU-bound.** The main thread spends most of the frame waiting for a free
  swap-chain buffer, not computing. Without a faux backbuffer that wait falls
  in the frame's first `clear`. With one, the window framebuffer is first
  touched by the present blit, so the same wait shows up in `present_ms`.
  Draw, effect and state calls together cost 3–10 ms per frame.
- **Resolution matters.** At comparable load, the 75% build cut the median
  frame from 68–83 ms to about 50 ms. The two sessions did not show the same
  view, so this is an estimate, not a controlled comparison.
- **GC is not the problem in play.** After the first seconds in the world,
  neither session had any collections. Loading and world entry had bursts, with
  single pauses up to 6.7 ms.
- **Audio was starved.** With 480-sample buffers the mixer ran 426–429
  callbacks per 5 s, about 85 per second for a device that needs 100. Each
  callback took about 0.3–0.5 ms on average, so the audio thread had CPU time to
  spare but was not scheduled often enough. The owner reported choppy music.

**Checks for perf-test-6 (0.5.1).**

- The menus fill the screen.
- The `# audio: device` line reports 1024 samples, with about 234 callbacks per
  interval.
- Compare the in-world frame time against the table at a similar primitive
  count.
- For a second run, set `logging = 0` and first delete
  `switch/castleminerz-logs`. The game should boot without recreating the
  folder. Existing logs are never deleted. With no log, this run can only be
  judged by feel.
