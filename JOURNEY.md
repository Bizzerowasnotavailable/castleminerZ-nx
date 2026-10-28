# CastleMiner Z Nintendo Switch Port - Journey Report

## Overview
An experimental Nintendo Switch homebrew interoperability and static-AOT porting framework for the Xbox Live Indie Games release of **CastleMiner Z**.

> **Important**: This repository does not contain CastleMiner Z, its executable assemblies, art, audio, or other copyrighted game data. You must supply your own Xbox 360 `LIVE`/STFS package.

---

## Objective
Build a playable Nintendo Switch port of CastleMiner Z using Xbox 360 GOD/STFS package without redistributing game assets, mapping Xbox gamer identity to Switch's regular local user profile without Nintendo Account linking.

## Current Snapshot (October 2026)

- **Status:** locally prepared developer build playable in Eden; not a generic
  NRO-plus-rip release. Physical Switch validation pending.
- **Build:** private ARM64 NRO produced by the full relink/AOT pipeline
  (latest local SHA-256
  `DC00BCA5E1FA300BA3659F22C90F4A516F7778A82CFAC6CA538B1472A33BD7A3`).
- **Verified:** title screen, offline world entry and save load, terrain,
  sky/clouds, water, HUD, hotbar icons, first-person held item, pause menu,
  movement/camera, mapped controller input, local-profile identity, and
  UID-scoped saves. Boot with no `aot_config.ini` override also reaches the
  world flow, but the package, processed managed files and saves remained
  present; this was not an empty-SD/package-only installation test.
- **Preparation:** `scripts/prepare-game.ps1` accepts the user's supported
  package and produces a complete private SD folder with a matching NRO,
  processed EXE/DLL, unmodified package and SHA-256 manifest. It no longer
  assumes the user already has extracted/relinked files or this PC's Eden data.
- **Open:** audio in Eden (the emulator's audren renderer never consumes SDL
  buffers), WMA streaming music, invisible third-person avatar body,
  inventory/combat/full save-cycle validation, and physical-hardware testing.

## Project Structure
- **Primary Code**: `castleminerZ-nx/` - Main Switch port framework
- **Dependencies**: `%LOCALAPPDATA%\Temp\opencode\` - External dependencies and private build artifacts
- **Assets**: Read directly from original package, no RomFS staging
- **Managed Code**: matching processed `CastleMinerZ.exe` and `DNA.Common.dll`
  on SD under `switch\castleminerz\managed`, generated from the user's package

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
- [x] Synthetic preparation/transfer/failure regression tests (17 checks)

---

### 🔄 ACTIVE (Ongoing Work)

#### Debugging & Refinement
- [x] AvatarRenderer always reports `Unavailable` so `DNA.Avatars.Avatar` keeps its built-in `DefaultBindPose`/`DefaultParentBones` fallback instead of rebuilding the skeleton from placeholder identity transforms
- [x] Removed the unused placeholder mesh/identity-bone path that collapsed animation, eye-point, and held-item transforms onto the origin
- [x] Confirmed first-person held-item placement in Eden: the tool is anchored at
      the lower-right of the view, not on the player's head
- [ ] Player body still invisible (no Xbox avatar assets; placeholder mesh rendering is future work)

#### Gameplay Features
- [ ] Inventory system testing
- [ ] Combat system validation
- [ ] Audio system testing (XACT/FAudio integration) — PCM SFX bank supported;
      11 WMA streaming entries unsupported by the Switch FAudio build; Eden's
      audren renderer blocks all SDL playback; hardware validation pending
- [ ] Camera/recording for item placement

#### Platform Integration
- [ ] Hardware validation (physical Switch testing)
- [ ] Controller mapping validation on hardware
- [ ] Profile selector and save/load on hardware

---

### ⏸️ BLOCKED (Stalled)

#### Gameplay Features
- Some gameplay features need validation
- Full gameplay testing pending

#### Hardware Dependencies
- No physical Switch run completed
- Full playability claim not made

---

## Current Runtime State

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
- **Game NRO**: `castleminerz.nro`, 97.5 MB in the local private build
  (includes AOT code; never committed)
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
1. **Hardware Validation**: Run the current NRO on a physical Switch and confirm
   rendering, input, audio, profile selection, and save/load
2. **Audio System**: Choose a WMA policy (decoder, host-side transcode, or safe
   silence), then validate PCM SFX and music on hardware
3. **Player Visibility**: Prototype a project-authored placeholder body driven by
   the fallback skeleton (no Xbox avatar assets)
4. **Inventory System**: Complete inventory implementation and testing
5. **Combat System**: Validate combat mechanics and entity interactions
6. **Save/Load Cycle**: Confirm saves survive a full power cycle and remain
   profile-isolated

### Platform Validation
1. **Hardware Testing**: Run on physical Switch hardware
2. **Controller Validation**: Test all button mappings on hardware
3. **Profile Testing**: Validate account selector and save/load
4. **Performance**: Measure frame rates on retail hardware

### Completion Criteria
- [ ] Save/load survives full power cycle
- [ ] Player is visible (not just placeholder)
- [ ] All gameplay features (inventory, combat, audio) functional
- [ ] Hardware validation completed
- [ ] Frame rate stable at 720p/540p on retail hardware

---

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
  streaming music cannot be decoded as-is.
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
```

---

## Hardware Status

### Emulator Environment
- **Tool**: libnx development mode in Eden
- **Graphics**: switch-mesa Gallium driver
- **Performance**: ~53 fps in test builds
- **Limitations**: Emulator does not model Switch memory limits or applet constraints

### Physical Hardware Target
- **Status**: Pending hardware testing
- **Focus**: Frame rate optimization, memory usage, input responsiveness
- **Risk**: switch-mesa CPU-heavy for state changes

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
  no SDL-based game can produce sound in the emulator. PCM SFX are supported by
  the linked FAudio build; the 11 streaming WMA entries are not.
- **Hardware Testing**: No physical Switch validation completed.
- **Eden Host Stability**: one host-side `ucrtbase.dll` crash
  (`0xc0000409`) occurred after an extended session that had rendered normally;
  it is not reproduced reliably and is treated as an emulator issue until
  isolated.
- **Performance**: switch-mesa CPU overhead may limit frame rate on retail
  hardware; Eden measurements are not representative.
- **Lightning Flashes**: brief full-sky white flashes are the game's original
  `drawLightning` storm behavior (10–40 s cadence above ground), not a corrupted
  skybox.

### Unaddressed Items
- deko3d FNA3D driver implementation (post-playable optimization)
- Full multiplayer networking (single-player focus)
- Complete graphics/effect parity validation
- Inventory, combat, and full power-cycle save/load validation
- WMA streaming music policy and hardware audio validation

---

## Conclusion
The project has successfully built a functional framework for running CastleMiner
Z on Nintendo Switch using static-AOT Mono and FNA. In Eden the game boots to the
title screen, loads an offline world, and renders terrain, sky, water, HUD,
hotbar icons, and the first-person held item correctly; movement, camera, pause
menu, mapped controller input, local-profile identity, and UID-scoped saves work.
The main remaining challenges are audio (emulator and WMA limits), the invisible
third-person avatar body, and validation on physical hardware.

**Status**: Playable in Eden with correct rendering; physical-hardware validation
pending.
**Next Focus**: Hardware validation, audio policy, and a project-authored avatar
body.
