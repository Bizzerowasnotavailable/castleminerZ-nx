# CastleMiner Z Nintendo Switch Port - Journey Report

## Overview
An experimental Nintendo Switch homebrew interoperability and static-AOT porting framework for the Xbox Live Indie Games release of **CastleMiner Z**.

> **Important**: This repository does not contain CastleMiner Z, its executable assemblies, art, audio, or other copyrighted game data. You must supply your own Xbox 360 `LIVE`/STFS package.

---

## Objective
Build a playable Nintendo Switch port of CastleMiner Z using Xbox 360 GOD/STFS package without redistributing game assets, mapping Xbox gamer identity to Switch's regular local user profile without Nintendo Account linking.

## Current Snapshot (November 2026)

- **Status:** 0.5.1 is fully playable on a physical Switch (owner hardware
  testing, November 22, 2026). It runs smoothly and feels like 60 fps with
  logging off, and inventory, combat and the rest of the core loop work. The
  audio is still choppy (see "Open tasks" below). It is also playable in Eden.
  This is not a generic NRO-plus-rip release.
- **Build:** private ARM64 NRO produced by the full relink/AOT pipeline.
  - Confirmed booting on hardware: SHA-256
    `31E28CFA247CECC56CB78B2ED456885C1A2207B5D267360847F32F33C3AD43EF`.
  - First build with performance instrumentation:
    `1C122A1081A31C131BC952E24B1B123B3169AD33C25B676B0F575391AA3A3053`.
  - Current build, version 0.5.1 (perf-test-6, November 22):
    `1BAC748C16DFEE0AC97FA6502D8551EEC79E130567168486F6781E71587708B9`.
    Tested on hardware by the owner: fully playable.
- **Verified:** title screen, offline world entry and save load, terrain,
  sky/clouds, water, HUD, hotbar icons, first-person held item, pause menu,
  movement/camera, mapped controller input, local-profile identity, and
  UID-scoped saves. Boot with no `aot_config.ini` override also reaches the
  world flow, but the package, processed managed files and saves remained
  present; this was not an empty-SD/package-only installation test.
- **Preparation:** `scripts/prepare-game.ps1` accepts the user's supported
  package and produces a complete private SD folder. It holds the matching NRO
  (with the processed EXE/DLL embedded), the unmodified package in its GOD-style
  folder, and a SHA-256 manifest. It no longer
  assumes the user already has extracted/relinked files or this PC's Eden data.
- **Open:**
  - choppy audio on hardware, which 0.5.1's 1024-sample buffers improved but did
    not fix (task 1 under "Open tasks");
  - FPS readout for SaltyNX-based tools such as the Status Monitor overlay
    (task 2 under "Open tasks");
  - audio in Eden, because the emulator's audren renderer never consumes SDL
    buffers;
  - the invisible third-person avatar body;
  - a full power-cycle save/load test.

## Project Structure
- **Primary Code**: `castleminerZ-nx/` - Main Switch port framework
- **Dependencies**: `%LOCALAPPDATA%\Temp\opencode\` - External dependencies and private build artifacts
- **Assets**: Read directly from original package, no RomFS staging
- **Managed Code**: matching processed `CastleMinerZ.exe` and `DNA.Common.dll`,
  generated from the user's package and embedded in the private NRO's RomFS
  under `managed/`; the SD card holds only the NRO and the package's GOD folder

---

## Technical Architecture

### Graphics Stack
- **Backend**: SDL2 → EGL → switch-mesa GLES 3.0 → FNA3D OpenGL
- **Rationale**: Mesa is the only FNA3D backend available on Switch; deko3d driver not in any public tree
- **Decision**: FNA3D with MojoShader shader conversion for Xbox effects

### Runtime
- **Mono**: `mono-nx rel-3` static AOT with 16,384 corlib trampolines
- **Package Mount**: `cmz:/` as managed cwd with `/game`, `/save`, `/logs`, `/data`
- **Profile Integration**: Local Switch profile via `acc:u0` with `acc:u1` fallback

### Controller Mapping
- **Original**: Xbox button labels on Switch positions (incorrect)
- **Current**: Switch labels mapped to Nintendo Switch (A=accept, B=back, X/Y as regular buttons)

---

## Progress Status

### ✅ COMPLETED (Major Milestones)

#### Package & Filesystem
- [x] STFS parsing, package discovery, VFS implementation
- [x] 239 presented frames at ~53 fps in test builds
- [x] Offline world entry achieved in Eden
- [x] Xbox cubemap byte order fixes (sky rendering)
- [x] Model buffer byte order fixes (vertex/index swap)
- [x] Original Xbox Endurance world deletion behavior preserved
- [x] Save state management for both Endurance and Survival modes

#### Graphics & Shaders
- [x] FNA3D + Mesa + SDL2 native stack integration
- [x] Ten project-authored replacement effects with MojoShader verification
- [x] Hotbar item icons and held models now render correctly
- [x] First-person held item renders in the correct lower-right pose using the
      game's fallback avatar skeleton (verified in Eden)
- [x] Xbox custom effects compiled and pass MojoShader/interface checks
- [x] Title screen, terrain, sky/clouds, water, HUD and pause menu render in Eden

#### Runtime & Compatibility
- [x] mono-nx static AOT setup with proper trampoline allocation
- [x] FNA `FNADllMap` stubbed to avoid OpenSSL dependencies
- [x] XNA compatibility shims (GamerServices, Net, Avatar surfaces)
- [x] Managed relinker (deterministic, verified, idempotent)
- [x] Local Switch profile integration (nickname, picture, UID storage)
- [x] Offline gamer API without Nintendo Account linking
- [x] Controller mapping working with Nintendo labels

#### Build & Pipeline
- [x] libnx ARM64 NRO build system
- [x] STFS package VFS reading without extraction
- [x] Host-side `cmztool` for package validation and extraction
- [x] Deterministic private extraction/relink/AOT pipeline
- [x] Package-to-install orchestration and portable complete SD output with
      build manifest; verifier catches missing DLLs, mixed builds and old packages
- [x] Synthetic preparation/transfer/failure regression tests (20 checks)

---

### 🔄 ACTIVE (Ongoing Work)

#### Debugging & Refinement
- [x] AvatarRenderer always reports `Unavailable` so `DNA.Avatars.Avatar` keeps its built-in `DefaultBindPose`/`DefaultParentBones` fallback instead of rebuilding the skeleton from placeholder identity transforms
- [x] Removed the unused placeholder mesh/identity-bone path that collapsed animation, eye-point, and held-item transforms onto the origin
- [x] Confirmed first-person held-item placement in Eden: the tool is anchored at
      the lower-right of the view, not on the player's head
- [ ] Player body still invisible (no Xbox avatar assets; placeholder mesh rendering is future work)

#### Gameplay Features
- [x] Inventory system testing (owner hardware test, 0.5.1)
- [x] Combat system validation (owner hardware test, 0.5.1)
- [ ] Audio system testing (XACT/FAudio integration). The PCM SFX bank is
      supported. The 11 xWMA streaming entries are decoded with FFmpeg's
      `wmav2` (`patches/faudio/0002`); a decoder of any kind fixed the hardware
      crash. Eden's audren renderer blocks all SDL playback. On hardware the
      music plays but was choppy with 480-sample buffers; 0.5.1's 1024-sample
      buffers are untested.
- [ ] Camera/recording for item placement

#### Platform Integration
- [x] Hardware validation (physical Switch testing): 0.5.1 is fully playable
      (owner hardware test). Audio is still choppy (open task 1).
- [ ] Controller mapping validation on hardware
- [ ] Profile selector and save/load on hardware

---

### ⏸️ BLOCKED (Stalled)

#### Gameplay Features
- Nothing blocked. A full power-cycle save/load test is still to do.

#### Hardware Dependencies
- None. 0.5.1 is fully playable on the owner's Switch.

---

## Current Runtime State

### Open tasks (requested November 22, 2026, after the 0.5.1 hardware test)

**Workflow.** Build a test NRO and hand it to the owner. The owner tests it on
the Switch. Push to GitHub only after the owner approves. Nothing from these
tasks is pushed before that.

1. **Fix choppy audio.** 0.5.1 made the game smooth, but the audio is still
   choppy.
   - **Evidence:** at 480 samples (perf-test-4/5), the mixer callback ran about
     85 times per second for a device that plays 100 buffers per second. A
     producer that stays one buffer ahead runs at exactly the device rate. A
     steady shortfall therefore means the device runs dry between buffers.
   - **Cause, confirmed in the linked library:** this is SDL 2.28.5's Switch
     backend (devkitPro `switch-sdl-2.28`, `SDL_switchaudio.c`). The
     disassembly of `SWITCHAUDIO_PlayDevice` in the `libSDL2.a` the NRO links
     matches the source.
     - It has only two wave buffers.
     - After queuing one, it spins until that buffer is `Playing`, and only
       then returns for the next mix.
     - Its loop runs `audrvUpdate()` before `audrenWaitFrame()`, so it tests
       state from before the wait and sees each buffer change 5-10 ms late, on
       top of the renderer's 5 ms frames.

     At 10 ms buffers the next buffer often came after the device had run dry.
     At 21 ms (0.5.1) the margin is thin, so any slow callback still opens a
     gap; perf.log showed spikes up to 12 ms.
   - **Ruled out:** the FFmpeg xWMA decoder. It decodes incrementally, and FACT
     submits each streaming xWMA wave as one buffer ("load the whole thing"),
     so the decoder resets only when a track starts or loops.
   - **Fix (`runtime-probe/native/switch_audio.cpp`):** a replacement SDL audio
     backend, linked in place of SDL's with `-Wl,--wrap=SWITCHAUDIO_bootstrap`.
     - It keeps up to four buffers queued, the audio renderer's limit for one
       voice (libnx `audrv`).
     - The mixer writes straight into the next free slice of the renderer's
       memory pool.
     - `WaitDevice` refreshes the renderer state before checking, so it returns
       at the first frame after the oldest buffer has played.
     - A late wake-up or a slow callback now costs latency (at most 85 ms with
       4 x 1024 samples), not a gap.
     - SDL's private structures are declared for 2.28.5 only. `static_assert`s
       pin the offsets that SDL's own backend uses in the linked library,
       checked by disassembly: device `spec` 8, `hidden` 144, and driver entry
       points 8/16/32/40/48/72/128. A `SDL_VERSIONNUM` check rejects any other
       SDL.
   - **Measurement:** perf.log's audio field adds `underruns=` (times every
     queued buffer had played before the next arrived), `drops=` (the
     renderer's voice drops) and `queue=4x1024`. A working fix shows
     `underruns=0` in play. `audio_buffers = 2..4` in `config.ini` changes the
     queue depth without a rebuild.
2. **SaltyNX FPS support.** Make the game's frame rate readable by SaltyNX
   tools, such as Status Monitor's FPS counter. NX-FPS normally provides it by
   hooking a retail game's graphics calls. That does not apply to a homebrew
   NRO with a statically linked Mesa, and SaltyNX does not inject into
   title-replacement processes ("hbloader") at all.
   - **Protocol, from the sources of SaltyNX `saltysd_proc`, NX-FPS and Status
     Monitor:**
     - SaltyNX serves the named port `SaltySD` with legacy SFCI IPC. Command 6
       reserves shared memory (a bump allocator), command 7 returns the 4 KiB
       shared-memory handle, and command 0 ends the session.
     - Unlike SaltyNX's other commands, 6 and 7 are not limited to injected
       processes.
     - Status Monitor scans the block for the first NX-FPS magic `0x465053`. It
       clears `pluginActive`, waits 100 ms, and treats the game as running if
       the flag is set again. It then shows `FPS`, or the average of the ten
       `FPSticks`.
   - **Implementation (`runtime-probe/native/saltynx_fps.c`):**
     - A background thread does the handshake, so SaltyNX being busy or
       missing never stalls the game. It reserves the 310-byte block and maps
       the memory.
     - Every present updates the block the way NX-FPS's `PostFrame` does.
       This is independent of logging.
     - SaltyNX zeroes its memory, except the first word, whenever an
       application starts. The reservation counter is reset only when SaltyNX
       injects into a retail game, so if no space is left the block uses
       NX-FPS's usual offset 4.
     - Stale NX-FPS blocks from an earlier run in the same hbloader process are
       withdrawn, because Status Monitor would read the first one. The block is
       withdrawn again at exit.
     - `saltynx = 0` in `config.ini` turns it off.
   - **Verified on the build PC:** the update logic, compiled from the source
     against a simulated clock, gives Status Monitor's displayed value exactly
     at 60, 30 and 45 fps. It recovers 2.5 s after a 3 s stall and re-sets
     `pluginActive`. The block layout is pinned with `_Static_assert`s (310
     bytes; Status Monitor reads the first 174).
3. **Config file:** `sdmc:/switch/castleminerz/config.ini` now has three keys:
   `logging`, `audio_buffers` (2-4, default 4) and `saltynx`. The parser
   passed 13 host cases.

**Status:** built as perf-test-7 (version 0.5.2), NRO SHA-256
`C7AFD06F32B52D7F064A69C8C5D760BD68F0E2FCF0C6C4501ABDBF23B5F1D3D1`, and handed
to the owner for a hardware test.
- The linked ELF contains `__wrap_SWITCHAUDIO_bootstrap` and none of SDL's
  `SWITCHAUDIO_*` code.
- All six host regression scripts pass.
- Committed locally, not pushed.
- Neither change has run on hardware yet.

**Owner checks for perf-test-7:**

1. **Audio:** is the music smooth? In perf.log, the `audio` field should show
   `underruns=0` in play, `queue=4x1024`, and about 234 callbacks per 5 s.
2. **SaltyNX:** with SaltyNX and Status Monitor installed, does Status
   Monitor's FPS counter show a frame rate in game? perf.log has a
   `# saltynx:` line with the shared-memory offset, or the reason it is
   absent.
3. **Latency:** if sound effects feel late, try `audio_buffers = 3` in
   `config.ini` (64 ms instead of 85 ms).

### Hardware performance and 0.5.1 (November 22, 2026)

- **Evidence:** `perf.log` from perf-test-4 and perf-test-5 on the owner's
  console (firmware 22.5.0, handheld). The full table is in
  `docs/performance.md`.
  - perf-test-4, no render scale: 13–25 fps in the world, with a median frame
    of 68–83 ms at 1.0–1.3 M primitives per frame. The main thread waited in
    the frame's first `clear`, which holds the wait for a free swap-chain
    buffer, and used only 26–48% CPU, so the game is GPU-bound.
  - perf-test-5 scaled the game-visible backbuffer to 75% in FNA. It ran at
    17–33 fps, with a median frame of about 50 ms at similar load. The wait
    moved to `present_ms`, because a faux backbuffer first touches the window
    framebuffer in the present blit. The menus were cropped (owner report),
    because the game no longer saw a 1280×720 backbuffer.
  - Audio in both builds: 426–429 mixer callbacks per 5 s with 480-sample
    buffers, about 85 per second for a device that needs 100, at about 0.3–0.5 ms
    each. The owner reported choppy music.
  - No GC collections after world entry.
- **0.5.1 changes (perf-test-6):**
  - The render scale moved into FNA3D (`patches/fna3d/0003-render-scale.patch`,
    `CMZ_RENDER_SCALE=75`). The game keeps a 1280×720 backbuffer, and FNA3D
    renders at 960×540 and blits up. The FNA-level patch was dropped.
  - FAudio is created with `FAUDIO_1024_QUANTUM`
    (`patches/fna/0005-audio-1024-quantum.patch`).
  - `config.ini` with `logging = 0` turns off all logging, including
    `runtime.log` (see Logging Configuration below).
  - The NACP version is 0.5.1, and an hbmenu icon can be set with `-Icon`.
- **Found while building:** `-DependenciesReady` had kept a `libFNA3D.a` built
  before the render-scale patch was applied, so the first build attempt would
  have shipped without the scale. `build-native-dependencies.ps1` rebuilt it.
  `docs/installation.md` now says to rebuild after native patch changes. The
  same script now judges its CMake builds by exit code, like the NRO link.
- **Verified on the build PC (not on hardware):**
  - `libFNA3D.a`'s OpenGL driver object reads `CMZ_RENDER_SCALE`, and the
    RomFS `FNA.dll` no longer contains the FNA-level scale.
  - `FAudioContext.Create` passes `32768` (`FAUDIO_1024_QUANTUM`) to
    `FAudioCreate`.
  - RomFS holds `aot_config.ini` and `aot_config_quiet.ini`, and the NACP
    reports 0.5.1.
  - The `config.ini` parser passes ten host cases, including `logging = 0` with
    spaces, which the first version of the parser ignored.
  - `prepare-game.ps1` produced and verified `C:\CMZ-private\perf-test-6`.
  - All six host regression scripts pass.
- **Needed next (hardware):**
  1. Run perf-test-6 with logging on. The menus should fill the screen, and the
     `# audio: device` line should report 1024 samples with about 234 callbacks
     per 5 s. Compare the in-world frame time at a similar primitive count, and
     check whether the music is still choppy.
  2. Delete `switch/castleminerz-logs`, then run again with `logging = 0`. It
     should boot without recreating the folder.

### Hardware boot failure analysis (November 21, 2026)

- **Status**: superseded for the user's console. The owner reports that
  `01736465908_01007b5017a12000.log` did not come from their Switch, so the
  conclusions below do not describe their device. A fresh NRO built from the
  current tree is the next test. The findings are kept as leads.
- **Evidence**: Atmosphère report `01736465908_01007b5017a12000.log` (process
  `hbloader`, Result `0x2A8`, Instruction Abort). The faulting thread `0x2d5`
  jumped to address 0 (`PC: 0`). Its LR is `castleminerz + 0x22b6f00`, the call
  site. The thread has a 132 KB stack and had used about 4.6 KB at the fault, so
  this is not a stack overflow.
- **How far the boot got**: `host.log` from the same run stops after
  `mono_jit_exec: begin`. Every earlier step succeeded on hardware: the package was
  found and recognized (`known build: yes`), `cmz:/` was mounted, and both
  assemblies opened. The fault is in game code, after managed startup began.
- **Ruled out by this evidence**: SD discovery, STFS parsing and mount, profile
  initialization, Mono initialization and assembly loading.
- **Not yet identified**: the function. Mapping the offsets needs the unstripped
  `castleminerz.elf` from the build that produced the crashed NRO.
  `scripts/symbolize-crash.ps1` does the lookup. The build workspace is empty
  here, and WSL is not installed, so the NRO cannot be rebuilt on this PC.
- **Hypotheses, most likely first**:
  1. Trimmed game code. `CastleMinerZ.exe` is an application, and ILLink was given
     it with plain `-a`, which roots only `Main`. Members reached through
     reflection, serialization or content readers are removed, and Eden seldom
     exercises those paths. Weakened by a Windows ILLink run on the same inputs: plain
      `-a` and `-a ... all` gave identical framework sets, and each game assembly
      shrank by only about 1 KB, so trimming is unlikely to be the cause. The `all`
      rooting stays as a harmless guard.
  2. The audio callback path. Eden's audren never consumes SDL buffers, so audio
     callbacks have never run in an emulator. FAudio also has no WMA decoder for the
     11 streaming entries.
  3. A null native pointer: a weak or missing native symbol reached through
     `direct-pinvoke` or `direct-icalls`, or a delegate with no AOT code.
- **Changes in this pass** (none built or run on hardware):
  - STFS read path (`source/stfs.cpp`): reads one physically contiguous run per
    hash-table group instead of one per 4 KiB block, skips `fseek` on sequential
    reads, and uses a 64 KiB stdio buffer. A Python model of the same logic
    reads the real package (239 files, expected sizes match `game_image.cpp`) with
    identical bytes. Seek-plus-read operations fall from 7,938 to 284 for the whole
    package. Not timed on an SD card.
  - ILLink rooting (`scripts/build-game.ps1`): `CastleMinerZ.exe` now uses `all`.
    Checked on Windows with the same inputs: the framework closure is unchanged, so
    the OpenSSL risk did not materialize in that run. The WSL build is the real test.
  - `scripts/symbolize-crash.ps1`: resolves crash frames against an ELF. Tested with
    a synthetic aarch64 ELF, not against this build.
  - `docs/reference-minecraft-switch.md`: notes on the wick800/minecraft-switch
    reference project.
- **Needed next**: the `castleminerz.nro` that crashed (copy it from the SD card's
  `switch` folder), the matching `castleminerz.elf` if it still exists, and a
  hardware run with a rebuilt NRO.

### Installation workflow correction (October 28, 2026)

- A second-PC report exposed the incomplete manual installation instructions:
  the user had the NRO/package but not the locally generated DNA assembly.
- Both original assemblies are inside the user's STFS package. They must be
  privately extracted, relinked, trimmed and paired with the AOT build; a raw
  extracted DLL or arbitrary downloaded DNA DLL is not a fix.
- Added `scripts/prepare-game.ps1`: validates the supported package, prepares
  dependencies, extracts only EXE/DLL privately, runs the existing full build,
  copies the unmodified package, and publishes only a complete verified SD tree.
  Output stays outside Git, never overwrites an existing output directory and
  never touches live Eden by default. Failed runs retain private diagnostics.
- Added `scripts/verify-game-install.ps1`: checks NRO/package/processed-assembly
  SHA-256/size pairing after transfer, rejects missing/mixed files and leftover
  package files, and warns about SD config overrides. The manifest records no
  machine-specific absolute paths and is not a publisher-authenticity signature.
- `build-game.ps1` now accepts `-OutputDirectory` so preparation can collect the
  NRO externally without replacing the previously working repository-root NRO.
- Added `docs/installation.md` and corrected README/toolchain/architecture docs:
  local source preparation is required, the destination PC needs no toolchain,
  and no-override boot was not a clean-install test. No public game NRO, runtime
  extraction/relinking, online authentication, or Nintendo Account was added.
- Validation: real supported package → private extraction → relink/ILLink/AOT
  → native NRO → verified SD folder completed on the existing build PC using
  `-DependenciesReady`. The prepared NRO SHA-256 is
  `BBDD7054EA75B7141363D3F9EBE9C723951EFF0CFB0A9716763A053AFC506259`;
  the earlier repository-root NRO and live Eden managed files were not replaced.
  All 17 synthetic checks pass, including default dependency-stage orchestration
  and locked files; local-profile tests, dependency verification, PowerShell
  syntax checks, repository guard and `git diff --check` also pass.
- Transfer/runtime check: copied only the prepared `switch` tree into a new
  portable Eden user directory on the same PC, retaining renderer/input settings
  but no old package/managed files, keys, firmware, saves, or SD runtime override.
  Host logs confirm both assemblies load from the new SDMC tree; Eden's native
  screenshot confirms a rendered offline world with terrain, sky, HUD/hotbar
  and held pickaxe. The destination manifest verifies after stopping Eden.
  Desktop PrintWindow captures were black/menu-only, so native screenshot
  evidence was used (private, outside Git).
- Caveat: this isolated run also logged repeated `CreateDirs` errors for world
  save paths under the deeply nested temporary portable directory. Windows path
  length is suspected, not established; fresh-install save/load is not certified.
  Newly provisioned build PC, actual second-PC runtime and physical Switch
  validation remain pending. Test Eden was stopped; existing Eden was left alone.

### Eden Development Build
- **Build**: private ARM64 NRO from the full relink/AOT pipeline
- **SHA-256**:
  `DC00BCA5E1FA300BA3659F22C90F4A516F7778A82CFAC6CA538B1472A33BD7A3`
- **Framework**: FNA 3.1 with mono-nx static AOT
- **Graphics**: SDL2 → switch-mesa GLES 3.0 → FNA3D OpenGL
- **Rendering verified**: title screen, offline world, terrain, sky, water, HUD,
  hotbar icons, held item, pause menu
- **Input verified**: mapped Switch controller input, first-person camera and
  movement
- **Package**: STFS VFS mounted at `cmz:/`
- **Profile**: Local Switch user selected (nickname, picture, UID storage)

### Build Outputs
- **Game NRO**: `castleminerz.nro`, about 115 MB in the 0.5.1 private build
  (includes AOT code and the processed game assemblies; never committed or
  published)
- **Graphics Probe**: `castleminerz-graphics-probe.nro`, ~6.3 MB
- **Runtime Probe**: `castleminerz-runtime-probe.nro`, ~81.8 MB
- **AOT Code**: external `%LOCALAPPDATA%\Temp\opencode\castleminerz-deps\cmz-game-stage\native\aot\`
- **Prepared Install**: `<Workspace>\prepared-sd\` by default, or the new
  external `-OutputRoot`; transfer its entire `switch` folder, not just the NRO
- **Logs**: `sdmc:/switch/castleminerz-logs/` directory

### Code Locations
- **Main Host**: `castleminerZ-nx/runtime-probe/native/main.cpp`
- **Profile Handling**: `castleminerZ-nx/runtime-probe/native/local_profile.cpp`
- **Shaders**: `castleminerZ-nx/shaders/effects/` (10 project-authored replacements)
- **FNA Patches**: `castleminerZ-nx/patches/fna/` and `patches/fna3d/`

---

## Next Moves (Phase 1: Gameplay Validation)

### Immediate Tasks
0. **Installation Validation**: Exercise package preparation on a newly
   provisioned PC, verify the transferred manifest and launch on a second PC
1. **Hardware Validation**: Run perf-test-6 (0.5.1) on the Switch, using the
   checks in the November 22 entry. Then confirm rendering, input, audio,
   profile selection and save/load.
2. **Audio System**: The WMA policy is settled: FFmpeg decodes it, and anything
   it cannot open plays as silence. Confirm that 1024-sample buffers stop the
   choppy music, and validate the PCM SFX on hardware.
3. **Player Visibility**: Prototype a project-authored placeholder body driven by
   the fallback skeleton (no Xbox avatar assets)
4. **Inventory System**: Complete inventory implementation and testing
5. **Combat System**: Validate combat mechanics and entity interactions
6. **Save/Load Cycle**: Confirm saves survive a full power cycle and remain
   profile-isolated

### Platform Validation
1. **Hardware Testing**: Boots and runs on the owner's Switch. A full gameplay
   pass is still to do.
2. **Controller Validation**: Test all button mappings on hardware
3. **Profile Testing**: Validate account selector and save/load
4. **Performance**: Measured on retail hardware (`perf.log`): the game is
   GPU-bound. Next, compare 0.5.1's FNA3D render scale against the earlier
   measurements.

### Completion Criteria
- [ ] Save/load survives full power cycle
- [ ] Player is visible (not just placeholder)
- [ ] All gameplay features (inventory, combat, audio) functional. Inventory
      and combat work on hardware; audio is still choppy.
- [x] Hardware validation completed (0.5.1, owner test)
- [ ] Frame rate stable at 720p/540p on retail hardware

---

## Logging Configuration

- **File:** `sdmc:/switch/castleminerz/config.ini`, which is optional:
  ```ini
  [logging]
  logging = 0
  ```
  Only the `logging` key is read. It is case-insensitive, spaces around `=` are
  optional, and the section is not checked. A value starting with `0` turns
  logging off. A missing file, a missing key or any other value leaves it on.
- **Off means:**
  - no `host.log`;
  - `cmz_perf_initialize(..., false)`, so no `perf.log`, no reporter or
    sampler thread, no notes, no Mono profiler callbacks, and no FNA3D call
    accounting beyond the tick read in each wrapper;
  - no `runtime.log`;
  - no `castleminerz-logs` directory.
- **Why there is a second runtime config:** mono-nx's `application_initialize`
  treats a `file_io_redirect` that cannot open its file as fatal ("Failed to
  redirect stdio to file"). The romfs `aot_config.ini` redirects stdio,
  unbuffered, to `castleminerz-logs/runtime.log`. So with logging off, the host
  selects `romfs:/aot_config_quiet.ini`, which has no redirect and
  `logging = false`. Only then does it skip the directory. An NRO without the
  quiet config, such as the runtime probe, keeps the normal config and the
  directory. An SD `aot_config.ini` override still takes precedence.
- **Code:** `logging_enabled()` and the config choice in
  `runtime-probe/native/main.cpp`, the `g_enabled` checks in `perf_monitor.cpp`,
  and both configs written by `scripts/build-game.ps1`.
- **Status:** built into perf-test-6 (0.5.1). The parser was tested on the host
  with ten config variants. Not yet run on hardware with either setting.

## Technical Details

### Switch Profile Integration
- **Service**: libnx `acc:u0` with `acc:u1` fallback
- **UID**: 32-character hexadecimal directory key (not Nintendo Account ID)
- **Storage**: `sdmc:/switch/castleminerz/save/profiles/<local-uid>/`
- **Picture**: `sdmc:/switch/castleminerz/data/profiles/<local-uid>/picture.jpg`
- **Nickname**: UTF-8 from AccountProfileBase.nickname
- **Online Status**: Always offline (`IsSignedInToLive` = false)

### Graphics Pipeline
- **FNA3D Driver**: OpenGL (via switch-mesa)
- **Shader Conversion**: MojoShader (D3D9 → GLSL ES 3.0)
- **Texture Formats**: DXT compression via switch-mesa
- **Cubemap Fix**: Color/BGRA and DXT1/3/5 conversion applied
- **Model Buffers**: Big-endian vertex/index byte-swapped

### Audio Findings (October 2026)
- **Banks parsed**: `Sounds.xwb` contains 127 PCM entries;
  `SoundsStreaming.xwb` contains 11 WMA entries (format tag 3).
- **Switch FAudio**: the linked FAudio build has no `HAVE_WMADEC`, so the
  streaming music cannot be decoded as-is. *Update, November 2026:*
  `patches/faudio/0002` decodes it with FFmpeg's `wmav2` (see
  `docs/toolchain.md`).
- **Eden blocker**: SDL opens the `switch` audio driver at 48 kHz stereo S16,
  but the emulator's audren renderer never transitions the wave buffer from
  `Queued` to `Playing`. `SWITCHAUDIO_PlayDevice` then spins in
  `audrenWaitFrame`.
- **Proof it is not the port**: a standalone SDL tone NRO receives exactly one
  audio callback and then blocks; loopback capture is silent. No SDL-based
  homebrew can produce sound in this Eden build.
- **Safety**: never open or play an audio device in the native host before Mono
  starts. A test device open hangs `SDL_CloseAudioDevice` and freezes startup.
  The host now logs SDL version/driver information only.

### Build Configuration
```cpp
Environment Variables:
- FNA_PLATFORM_BACKEND=SDL2
- FNA3D_FORCE_DRIVER=OpenGL
- FNA3D_OPENGL_FORCE_ES3=1
- CMZ_REPLACEMENT_EFFECT_DIRECTORY=romfs:/effects
- CMZ_FIRST_ENVIRONMENT_SLICE_ONLY=1
- CMZ_RENDER_SCALE=75   (read by the patched FNA3D, 0.5.1)
```

---

## Hardware Status

### Emulator Environment
- **Tool**: libnx development mode in Eden
- **Graphics**: switch-mesa Gallium driver
- **Performance**: ~53 fps in test builds
- **Limitations**: Emulator does not model Switch memory limits or applet constraints

### Physical Hardware Target
- **Status**: boots and runs on the owner's console (firmware 22.5.0). It runs
  at 13–25 fps in the world at full resolution and 17–33 fps with a 75% scale.
- **Focus**: Frame rate optimization, memory usage, input responsiveness
- **Risk**: the game is GPU-bound, not CPU-bound (the main thread waits for the
  GPU and uses 26–53% CPU), so rendering cost decides the frame rate

---

## Legal & Compliance

### Build Boundaries
- **No Assets**: Repository contains no game code, art, audio, or assets
- **Private Intermediates**: Generated AOT code and extracted assemblies in ignored directories
- **Public Code**: Parsers, compatibility APIs, tests, project-authored replacements
- **Licensing**: Project-authored code under MIT license
- **No Binary Distribution**: the game NRO, generated game AOT code, and relinked
  game assemblies are private build outputs. They embed or load the user's
  `CastleMinerZ.exe`/`DNA.Common.dll` and must not be published; ship the source
  and build scripts instead.

### Preservation Focus
- **Static Analysis**: Decompiled game code not included
- **Interop**: Uses original package directly (no redistribution)
- **Platform**: Independent of Xbox Live or Nintendo ecosystems

---

## Testing & Verification

### Host Tests
- **Package Discovery**: STFS package found on SD card
- **Content Validation**: 224 XNB files, 2 .xwb wave banks, 1 .xsb sound bank
- **Managed Relinker**: Deterministic routing tests
- **Effect Loading**: MojoShader interface verification

### Runtime Probes
- **Native Probe**: `castleminerz-runtime-probe.nro` (6.3 MB)
- **Graphics Probe**: FNA3D/GL diagnostics
- **Managed Probe**: GraphicsDevice creation and 240-frame test
- **Eden Tests**: Offline world entry and save load with terrain, sky, water,
  HUD, hotbar icons, first-person held item, and pause menu

### Exit Criteria
Each gate defines concrete pass/fail conditions:
1. **Gate A**: FNA frame + filesystem (true blockers)
2. **Gate B**: Package as filesystem (format work)
3. **Gate C**: Content formats (known solutions)
4. **Gate D**: Audio (FNA implements XACT)
5. **Gate E**: Input and lifecycle
6. **Gate F**: Platform services (novel avatar substitute)
7. **Gate G**: Performance (measurement first)
8. **Gate H**: Hardware loop

---

## References & Documentation

### Key Files
- `docs/installation.md` - Package preparation, complete SD transfer and DLL troubleshooting
- `docs/roadmap.md` - Engineering sequence and status tracking
- `docs/playability.md` - Gate-by-gate playability plan
- `docs/local-profiles.md` - Switch profile integration details
- `docs/graphics.md` - Graphics backend decision evidence
- `docs/architecture.md` - System design and constraints

### Build Scripts
- `./scripts/prepare-game.ps1` - Supported package to complete private game installation
- `./scripts/verify-game-install.ps1` - Destination package/NRO/managed hash verification
- `./scripts/test-game-preparation.ps1` - Synthetic preparation/transfer/error checks
- `./scripts/build-game.ps1` - Lower-level private relink/trim/AOT/native game build
- `./scripts/build-switch.ps1` - Package-discovery bootstrap only, not the playable game
- `./scripts/setup-dependencies.ps1` - Host dependency management
- `./scripts/test-local-profiles.ps1` - Profile system tests

---

## Current Limitations

### Known Issues
- **Avatar Rendering**: No third-person player body mesh; the game's fallback
  skeleton is used for transforms. First-person held items and the camera are
  correct. The local profile supplies nickname/picture metadata only; Mii
  geometry is not read or rendered.
- **Audio in Eden**: Eden's audren renderer never consumes SDL wave buffers, so
  no SDL-based game can produce sound in the emulator. The linked FAudio build
  plays the PCM SFX and decodes the 11 streaming WMA entries with FFmpeg.
- **Hardware Testing**: boots and runs, and perf.log measurements exist. Music
  was choppy with 480-sample buffers. Full gameplay validation has not been
  done.
- **Runtime probe build**: `build-runtime-probe.ps1` copies only part of the
  shared host sources and predates the performance monitor, so it cannot build
  the current host. The game build is unaffected.
- **Eden Host Stability**: one host-side `ucrtbase.dll` crash
  (`0xc0000409`) occurred after an extended session that had rendered normally;
  it is not reproduced reliably and is treated as an emulator issue until
  isolated.
- **Performance**: GPU-bound on retail hardware, at 13–33 fps in the world
  depending on scene and render scale. Eden measurements are not
  representative.
- **Lightning Flashes**: brief full-sky white flashes are the game's original
  `drawLightning` storm behavior (10–40 s cadence above ground), not a corrupted
  skybox.

### Unaddressed Items
- deko3d FNA3D driver implementation (post-playable optimization)
- Full multiplayer networking (single-player focus)
- Complete graphics/effect parity validation
- Inventory, combat, and full power-cycle save/load validation
- Hardware audio validation (the WMA music policy is settled: FFmpeg decoding)

---

## Conclusion
The project has successfully built a functional framework for running CastleMiner
Z on Nintendo Switch using static-AOT Mono and FNA. In Eden the game boots to the
title screen, loads an offline world, and renders terrain, sky, water, HUD,
hotbar icons, and the first-person held item correctly; movement, camera, pause
menu, mapped controller input, local-profile identity, and UID-scoped saves work.
On a physical Switch it boots and runs. There it is GPU-bound at a low frame
rate, and the music was choppy. The main remaining challenges are frame rate
and audio on hardware, the invisible third-person avatar body, and full
gameplay validation on hardware.

**Status**: Playable in Eden with correct rendering. It boots and runs on
hardware at 13–33 fps. 0.5.1 (FNA3D render scale and 1024-sample audio) awaits
its hardware run.
**Next Focus**: the perf-test-6 hardware run, frame rate, audio on hardware,
and a project-authored avatar body.
