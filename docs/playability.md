# Path to playable

This is the concrete engineering sequence from the current state to a running
single-player game, with exit criteria for each gate and what can be verified
without a Switch. It assumes the graphics decision in
[`graphics.md`](graphics.md): SDL2 → EGL → switch-mesa GLES 3.0 → FNA3D.

## Where the port actually stands

| Layer | State |
| --- | --- |
| STFS package parsing and asset verification | working, host-tested |
| `CastleMinerZ.exe` / `DNA.Common.dll` XNA API surface | fully resolved (111/368 and 148/551 members) |
| XNA compatibility shims (GamerServices, Net, Avatar) | compile and pass synthetic tests |
| Managed relinker | deterministic, verified, idempotent |
| mono-nx static AOT probe (no FNA) | builds and links |
| FNA managed code inside the AOT probe | ILLink keeps it, AOT succeeds, and the ELF links FNA3D + Mesa + SDL2 + FAudio with no undefined symbols |
| FNA `Game`/`GraphicsDevice` in the managed probe | creates a device, clears and presents 240 frames in Eden; hardware run pending |
| FNA3D + Mesa + SDL2 native stack | **links and produces a 6.3 MB NRO** (native probe) |
| Game assets read from the package at runtime | verified in Eden through STFS and the managed root device |
| Game `Main()` reached | verified; title screen and offline world reached in Eden |
| Offline world entry | verified in Eden; terrain, sky, water, HUD, hotbar icons, held item and pause menu render |
| First-person held items | correct lower-right placement verified in Eden using the game's fallback avatar skeleton |
| Xbox custom effects | ten project-authored replacements compile and pass MojoShader/interface checks |
| Xbox texture/model byte order | corrected sky, hotbar icons and held models verified in Eden; synthetic reader tests pass |
| Audio (XACT/FAudio) | PCM SFX path linked; 11 WMA streaming entries unsupported; Eden audren blocks all SDL playback; hardware validation pending |
| Local profile integration | selected local user and UID-scoped storage verified in Eden; synthetic facade tests pass; hardware validation pending |

The native graphics stack and package filesystem work in Eden. The reported
“Loading Player Info” step is passed: internal GPU screenshots confirm offline
world entry, generated terrain, sky, water, HUD, hotbar icons, held item, and the
pause menu. A checked FNA cubemap byte-order patch fixes the multicolored
corrupted sky. Raw model vertex/index buffers also need Xbox byte-order
conversion; that fix passes synthetic tests and restores the hotbar icons and
held models in Eden. First-person held items are now placed correctly because
`AvatarRenderer` reports `Unavailable`, which selects the game's own fallback
bind pose instead of a placeholder rig; the third-person player body is still
invisible. Inventory, entities, combat, audio, and a full save/load cycle still
need complete checks. No physical Switch run or full-playability claim is made.

Native EGL errors can mask an original managed error when the game's error
screen creates a second graphics device. Diagnostic hooks log the original
exception before that happens. Mono also exhausted its default 4,096 AOT
trampolines during startup; the build now reserves 16,384 for
`System.Private.CoreLib`. Manual Eden stops can produce host teardown faults;
those are not evidence of a new in-game crash.

## Gate A — FNA inside the static-AOT runtime

Resolved build issue: FNA's `FNADllMap` is a `[ModuleInitializer]`, so ILLink keeps
its whole body, including `XmlDocument`; `System.Private.Xml` drags in
`System.Net.Http` → `System.Net.Security` → OpenSSL `CryptoNative_*`, which
mono-nx does not ship. A second, independent blocker: `mono-aot-cross`
compiles every method of an assembly and the static AOT module table keeps all
of them alive, so FNA's unused SDL3 and Emscripten bindings emit external
references (`SDL_OpenGamepad`, `emscripten_set_main_loop`, ...) that cannot
link.

Both are handled by one project-owned Cecil pass, `tools/managed-aot-prep`,
which rewrites the compiled FNA assembly before ILLink and AOT. That preparation
pass does not modify source; separate checked source patches implement Xbox
effect replacement loading, Xbox texture/model byte order, and the native GLSL
ES precision fix.

1. Stub `Microsoft.Xna.Framework.FNADllMap::Init` to `return;`. On Switch the
   resolver is unnecessary: FNA3D, FAudio and SDL2 are statically linked and
   resolved through direct P/Invoke, and the iOS/tvOS branch in that method does
   not apply.
2. Neutralize every P/Invoke in the `SDL3` and `__Native` modules with a
   managed `default(T)` body (`--stub-pinvoke-module`). Those bindings are
   unreachable because the host forces `FNA_PLATFORM_BACKEND=SDL2`, and the
   Emscripten main loop is never selected. The tool verifies that no import
   remains and is deterministic and idempotent.
3. Pass `-d managed/bin/Debug/net9.0` to ILLink so `FNA.dll` and
   `CastleMinerZ.XnaCompat.dll` are actually kept.
4. Make the AOT step fail the build on any `Failed to load` line instead of
   emitting an NRO that is silently missing assemblies.
5. Link `libFNA3D.a`, `libmojoshader.a`, `libSDL2.a`, `libEGL.a`, `libglapi.a`,
   `libglad.a`, `libdrm_nouveau.a`, `libFAudio.a`, `libstdc++` and `libnx` into
   the AOT ELF, with `-lnx` **after** the Mesa archives (static link order).
6. Set the environment before managed startup: `FNA_PLATFORM_BACKEND=SDL2`,
   `FNA3D_FORCE_DRIVER=OpenGL`, `FNA3D_OPENGL_FORCE_ES3=1`,
    `FNA_SDL_FORCE_BASE_PATH` (see Gate B). The managed probe does this itself;
   the real host must do it before any FNA type is touched.
7. Call `console_dispose()` before SDL takes the display and
   `console_ensure_init()` afterwards, because the libnx console owns the
   framebuffer.

State: steps 1–6 are implemented and the probe NRO
(`castleminerz-runtime-probe.nro`, 85,662,773 bytes) contains
`FNA3D_CreateDevice`, `FNA3D_PrepareWindowAttributes`, `SDL_GL_CreateContext`,
`eglGetDisplay` and `FAudio_Initialize` with zero undefined symbols. The probe
creates a `GraphicsDevice`, clears and presents 240 frames, draws two sprites,
and writes `sdmc:/switch/castleminerz-logs/fna-graphics-probe.log`.

Exit criterion: on hardware, an AOT'd probe prints the FNA3D/GL strings, creates
a `GraphicsDevice`, clears and presents through FNA's own managed API, and
survives 60 seconds and a suspend/resume cycle.

## Gate B — the package is a filesystem

`ContentManager` uses `TitleContainer.OpenStream`, i.e. `System.IO.File`. The
cleanest way to serve the STFS package is a libnx `devoptab`, not managed I/O:

1. Implement `cmz-stfs` devoptab: mount the discovered package read-only at a
   fixed path (for example `sdmc:/switch/castleminerz-package` via a mount
   point, or a dedicated `stfs:` device).
2. Map package-relative paths onto the existing `StfsPackage` block math, add a
   4 KiB readahead cache, and keep bounds checking where it is today.
3. Point FNA at the mount with `FNA_SDL_FORCE_BASE_PATH` so `TitleLocation`
   and `ContentManager.RootDirectory` resolve inside the package.
4. Keep saves and configuration on `sdmc:` — never inside the package.

Exit criterion: managed code opens `Content/...xnb` through `File`/`Stream`,
and `ContentManager.Load<Texture2D>` returns a real texture.

State: verified in Eden. `stfs:/` serves the original package; `cmz:/` exposes
`/game`, `/save`, `/logs` and `/data`, and is the working directory. FNA's base
path is `/game/`. Its preference path is `/save/profiles/<local-uid>/` for a
selected local user, or `/save/guest/` without one. The earlier probe completed
239 presented frames at approximately 53.1 fps. This does not establish hardware
performance.

## Gate C — content formats

The package holds 224 XNB files, 2 `.xwb` wave banks, 1 `.xsb` sound bank and
1 `.xgs` global settings file.

1. XNB v5: FNA already decompresses the LZX container and dispatches readers,
   so this is mostly a runtime check that compressed and uncompressed assets
   both load from the devoptab.
2. Textures: the game's XNBs are DXT-compressed. Confirm
   `EXT_texture_compression_s3tc` on switch-mesa; if it is absent, decompress
   DXT1/3/5 on the CPU at load time (slow but correct) behind
    `FNA3D_SupportsDXT1`/`SupportsDXT5`-style checks. FNA already byte-swaps Xbox
    2D texture payloads, but its cube reader did not. A checked patch now applies
    Color/BGRA and DXT1/3/5 conversion to every cube face and mip. The sky fix is
    verified in Eden; 480 synthetic face/mip uploads check Xbox conversion,
    desktop passthrough, sub-block mip sizes and existing texture reuse.
3. Effects: the ten custom Xbox FXBs contain Xenos GPU microcode, not PC D3D9
   shaders; byte swapping cannot make MojoShader run them. Sources in
   `shaders/effects` provide project-authored approximations with the same
   callable parameters and technique/pass order. D3DX9 compiles them into
   MojoShader-compatible effects. A checked FNA patch substitutes only the
   allowlisted Xbox effect assets; textures/models still come from the package.
   The native translator patch declares `sampler3D` precision for strict GLES 3
   compilers. Visual parity and complete on-device coverage remain unverified.
4. Models and skinning: raw Xbox model vertices and indices are big-endian.
    Checked readers reverse 32-bit float/packed words and 16-bit short/half
    components according to the vertex declaration, preserving stride padding;
    16/32-bit indices are swapped separately. Synthetic tests cover all XNA
    vertex formats, interleaved layouts, desktop passthrough, index reuse and
    invalid declarations. Skinning paths and bone palette size against
    `GL_MAX_VERTEX_UNIFORM_VECTORS` still need runtime checks.
5. Fonts and strings: `SpriteFont` plus the six localized XNBs.
6. Avatar animation: `Content/AvatarAnimation/*.xnb` is loaded by the game, so
   the avatar API must not throw even where rendering is a placeholder.

Exit criterion: a probe renders one textured model, one stock `BasicEffect`
mesh, one custom effect, and a string drawn with `SpriteFont`.

The host sets `CMZ_FIRST_ENVIRONMENT_SLICE_ONLY=1`. Its FNA3D patch omits the
three unused layers of the game's four-layer environment texture because the
current `BlockEffect` samples only slice zero at Z `0.125`. This removes Eden's
layered-DMA warnings without changing the current shader's sampled data. Disable
or revise the optimization before sampling other layers.

## Gate D — audio (XACT)

FNA implements XACT on FAudio's FACT engine (`FACTAudioEngine_*`), so no audio
reimplementation is needed.

1. Link `libFAudio.a`; SDL's Switch audio driver (`SDL_AUDIO_DRIVER_SWITCH`) is
   already present in the installed SDL 2.28.5.
2. Load `GlobalSettings.xgs`, the `.xsb` and both `.xwb` banks through the
   package filesystem, including streaming cues.
3. Verify category volumes, 3D positional cues and loop regions.
4. Keep a silent no-op path if audio init fails so the game still runs.

State: the XACT/FAudio path is linked and the sound-effect bank is PCM
(`Sounds.xwb`, 127 entries). The 11 streaming music entries in
`SoundsStreaming.xwb` are WMA, which the current Switch FAudio build does not
decode. Eden cannot validate playback at all: SDL opens the `switch` driver, but
the emulator's audren renderer never advances wave buffers, so
`SWITCHAUDIO_PlayDevice` blocks in `audrenWaitFrame`. A standalone SDL tone NRO
receives one callback and then blocks, proving this is an emulator limit rather
than port code. Do not open an audio device in the native host before Mono
starts; that deadlocks startup.

Exit criterion: music and effects play, and stopping/restarting the engine does
not leak or deadlock.

## Gate E — input and lifecycle

1. Map Switch controller state onto XNA `GamePad`/`Keyboard` (A/B/X/Y, sticks
   with dead zone, ZL/ZR, plus/minus, d-pad) and expose it through the existing
    FNA SDL2 backend.
   The host selects Nintendo button labels and supplies an explicit mapping for
   SDL 2.28.5's built-in libnx controller. Its fixed default mapping ignored the
   label hint and made Switch A act as Xbox B/back. The override maps A/B/X/Y by
   label while retaining the original sticks, d-pad, shoulders and triggers.
2. Handle docked/handheld, suspend/resume, and controller connect/disconnect
   without leaking the GL context.
3. Map `+` (or `Home` exit) to a clean shutdown of mono and SDL.

Exit criterion: menus and gameplay respond correctly, and a 10-minute session
with sleep/resume behaves.

## Gate F — platform services

1. Saves: `StorageDevice`/`StorageContainer` uses
    `sdmc:/switch/castleminerz/save/profiles/<local-uid>/`, or `save/guest/`.
    Eden creates player statistics, world metadata and inventory files there.
    File creation is not yet proof of a successful reload or profile isolation.
    Check free-space reporting, flushing and a full restart/load cycle.
    Use Survival for persistent-world tests: the original game intentionally
    deletes its current Endurance world on a clean return to the main menu.
2. GamerServices: the selected console-local Switch profile now supplies the
   offline gamer's nickname, picture and UID-scoped storage; a linked Nintendo
   Account is not requested. See [local profiles](local-profiles.md). Confirm
   the game accepts the local profile,
   the deterministic Guide dialogs, and offline `NetworkSession` for any
   co-op menus.
3. Avatars: the game uses avatars as player characters and animates them. Xbox
   avatar assets cannot be obtained or shipped, so the honest implementation is
   a procedural placeholder rig driven by the same `AvatarAnimation` clips and
   `AvatarRenderer` transforms. Until then players render as nothing, which is
   playable but visually wrong.

Exit criterion: a save survives a full power cycle, and the player is visible.

## Gate G — performance

switch-mesa's tegra Gallium driver is the frame-rate risk, not the CPU.

1. Measure with the game's own frame counter at 1280x720 and 960x540.
2. Reduce Mesa state changes: `SpriteBatch` batching, avoid render-target
   ping-pong where the game allows it, keep effect permutations minimal.
3. Tune mono-nx (GC nursery, `config.ini`) and the AOT set.
4. Only if the result is unplayable does the **deko3d FNA3D driver** become
   worth its cost: a full `FNA3D_Driver` implementation plus Mojoshader→DKSH
   shader translation. That is a separate project, not a blocker.

Exit criterion: stable frame rate in the game's most demanding scene.

## Gate H — hardware loop

Every gate above ends in an NRO that must be run on the console, because the
emulator landscape does not model switch-mesa, mono-nx's AOT runtime, or the
applet memory limits faithfully. Each probe therefore:

- prints a complete, greppable status block before doing anything visual;
- waits for `+` so the status stays readable;
- writes the same log to `sdmc:/switch/castleminerz-logs/` for later analysis.

## Ordering rationale

Gates A and B are the only true blockers: without a managed FNA frame and a
filesystem, no game code can run. Gate C and D are format work with known
solutions. Gate F contains the one genuinely novel piece (a legal avatar
substitute). Gate G is measurement first, architecture second.
