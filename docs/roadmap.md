# Roadmap

This is an engineering sequence, not a release schedule. A checked item means
the work exists and has been validated against the currently supported World
XBLIG package.

## M0 — package bootstrap

- [x] Build a libnx ARM64 NRO.
- [x] Discover a user-supplied STFS package recursively on SD.
- [x] Parse the supported STFS file table and read contiguous payload ranges.
- [x] Reject unsafe paths, malformed bounds, and unsupported fragmentation.
- [x] Validate the expected game layout and managed PE header.
- [x] Share parser code with a host list/verify/extract utility.
- [x] Add a synthetic parser test and repository asset/key guard.
- [ ] Test SD-card discovery and package reads on physical Switch hardware.

## M1 — content and build pipeline

- [x] Add a synthetic STFS read test across a hash-table boundary.
- [ ] Expand the malformed-package and alternate-layout test corpus.
- [ ] Implement fragmented STFS hash-chain traversal.
- [x] Add streaming package handles suitable for managed `Stream` consumers.
- [x] Use FNA's XNB v5 readers and LZX decompression through the package VFS.
- [x] Inventory all 224 XNB reader tables and the ten custom effect assets.
- [x] Add synthetic cubemap and model-buffer reader fixtures beyond effect interfaces.
- [x] Replace Xenos-only custom effects with project-authored D3D9 effect sources;
  validate MojoShader parsing, GLSL ES 3 translation and interface contracts.
- [x] Create a user-side private extraction/relink/AOT pipeline with checks that
  prevent proprietary outputs from entering source control.

## M2 — managed runtime bring-up

- [x] Reproduce the `mono-nx` runtime and AOT toolchain from pinned sources.
- [x] Validate the pinned `mono-nx` release SDK by rebuilding its interpreter
  and static-AOT example against the installed devkitA64/libnx toolchain.
- [x] Resolve the Linux/WSL-hosted AOT workflow and document prerequisites.
- [x] Build a project-owned static-AOT managed runtime probe NRO.
- [ ] Load assembly metadata from the package-backed VFS.
- [x] Build a project-owned XNA compatibility assembly for the
  GamerServices/Net/Avatar surface FNA omits.
- [x] Add a deterministic per-type Mono.Cecil relinker that routes legacy XNA
  references to FNA or the compatibility assembly.
- [x] Prove relinker routing, pruning, determinism, and idempotence with a
  synthetic self-test.
- [x] Extend the private-input-safe aggregate audit to prove zero unresolved
  XNA types and members against FNA plus compatibility.
- [x] Integrate FNA managed code and the static native libraries into the AOT
  staging pipeline.
- [x] Reach the managed entry point with diagnostics and exception reporting.
- [x] Validate garbage collection, reflection, threading, timers, and file I/O.
- [x] Neutralize FNA's XML module initializer and its unused SDL3/Emscripten
  P/Invokes with the project-owned `managed-aot-prep` pass; fail the AOT build on
  any `Failed to load` line.
- [x] Link FNA3D, MojoShader, SDL2, Mesa and FAudio into the AOT image with no
  undefined symbols.

## M3 — first frame

- [x] Decide and document the graphics backend
  ([`graphics.md`](graphics.md)): SDL2 → EGL → switch-mesa GLES 3.0 → FNA3D.
- [x] Link FNA3D, MojoShader, SDL2 and the Mesa stack into a static ARM64 NRO.
- [x] Add a native hardware probe that prints SDL/GL/FNA3D diagnostics and
  presents a clear loop.
- [x] Host FNA under mono-nx: the managed probe creates a `GraphicsDevice`,
  clears and presents 240 frames, and attempts a `SpriteBatch` draw.
- [ ] Confirm the native and managed probes present on physical hardware and
  record the GL vendor/renderer/version, device result and frame rate.
- [x] Implement the STFS devoptab so `File`/`ContentManager` read the package
  directly.
- [ ] Implement the graphics resources and render states needed by startup.
- [x] Compile and load the initial project-authored replacement effects in Eden.
- [x] Load startup textures, fonts and models directly from the package in Eden.
- [x] Reach the title screen in Eden.
- [x] Complete and visually verify the main-menu/offline-world-entry path in Eden.
- [x] Correct Xbox cubemap byte order and verify sky rendering in Eden.
- [x] Verify corrected Xbox model vertex/index uploads in a running world:
  hotbar icons and held models render in Eden.

## M4 — offline gameplay

- [ ] Implement XACT global settings, sound banks, wave banks, and streaming.
- [x] Map the selected local Switch profile's nickname, picture and UID-scoped
  storage onto the offline gamer APIs without requiring a Nintendo Account.
- [ ] Validate profile selection and save/load on hardware; implement real
  keyboard/message dialogs rather than deterministic compatibility responses.
- [ ] Implement the local `NetworkSession` behavior assumed by game logic.
- [ ] Implement a legally distributable avatar replacement renderer and map the
  packaged animation data.
- [ ] Correct first-person held-item poses with the replacement avatar rig.
- [x] Enter an offline world in Eden and render generated terrain, sky and HUD.
- [ ] Validate chunk workers, inventory, entities, combat, saving, and loading.
- [ ] Profile memory and frame time on retail-frequency Switch hardware.

## M5 — compatibility and multiplayer

- [ ] Complete graphics/effect coverage and visual comparisons.
- [ ] Add robust sleep/resume, controller disconnect, and error handling.
- [ ] Design an independent transport behind the XNA networking facade.
- [ ] Validate session discovery, host/client replication, disconnects, and
  migration behavior without relying on retired Xbox Live services.
- [ ] Publish reproducible compatibility results and known limitations.

## Post-playable — optional deko3d backend

Only if switch-mesa cannot hold a playable frame rate:

- [ ] Implement an `FNA3D_Driver` on deko3d (device, pipelines, buffers,
  textures, render targets, state objects, present, readback).
- [ ] Add a Mojoshader→DKSH shader path.
- [ ] Re-validate first frame and gameplay before making it the default.

Nothing in the playable path depends on this; see
[`graphics.md`](graphics.md) for why it is not the first target.

## Not planned as shortcuts

- Running XenonRecomp on the managed PE: there is no PowerPC input.
- Committing extracted/decompiled game files or generated game code.
- Shipping a pre-populated asset pack, key set, or converted shader/audio
  cache.
- Claiming playability before first-frame and gameplay milestones are tested on
  hardware.
