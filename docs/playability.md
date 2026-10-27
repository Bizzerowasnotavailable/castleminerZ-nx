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
| FNA `Game`/`GraphicsDevice` in the managed probe | creates a device, clears and presents 240 frames — **awaiting a hardware run** |
| FNA3D + Mesa + SDL2 native stack | **links and produces a 6.3 MB NRO** (native probe) |
| Game assets read from the package at runtime | not implemented |
| Game `Main()` reached | not reached |

The native graphics stack is no longer a research question, and the managed FNA
path now links into the same NRO. What remains is a hardware run, exposing the
package as a filesystem, and then grinding through the game's content and
services.

## Gate A — FNA inside the static-AOT runtime

Blocking issue: FNA's `FNADllMap` is a `[ModuleInitializer]`, so ILLink keeps
its whole body, including `XmlDocument`; `System.Private.Xml` drags in
`System.Net.Http` → `System.Net.Security` → OpenSSL `CryptoNative_*`, which
mono-nx does not ship. A second, independent blocker: `mono-aot-cross`
compiles every method of an assembly and the static AOT module table keeps all
of them alive, so FNA's unused SDL3 and Emscripten bindings emit external
references (`SDL_OpenGamepad`, `emscripten_set_main_loop`, ...) that cannot
link.

Both are handled by one project-owned Cecil pass, `tools/managed-aot-prep`,
which rewrites the compiled FNA assembly before ILLink and AOT. The pinned FNA
source is never modified.

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
   `FNA_SDL2_FORCE_BASE_PATH` (see Gate B). The managed probe does this itself;
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
3. Point FNA at the mount with `FNA_SDL2_FORCE_BASE_PATH` so `TitleLocation`
   and `ContentManager.RootDirectory` resolve inside the package.
4. Keep saves and configuration on `sdmc:` — never inside the package.

Exit criterion: managed code opens `Content/...xnb` through `File`/`Stream`,
and `ContentManager.Load<Texture2D>` returns a real texture.

## Gate C — content formats

The package holds 224 XNB files, 2 `.xwb` wave banks, 1 `.xsb` sound bank and
1 `.xgs` global settings file.

1. XNB v5: FNA already decompresses the LZX container and dispatches readers,
   so this is mostly a runtime check that compressed and uncompressed assets
   both load from the devoptab.
2. Textures: the game's XNBs are DXT-compressed. Confirm
   `EXT_texture_compression_s3tc` on switch-mesa; if it is absent, decompress
   DXT1/3/5 on the CPU at load time (slow but correct) behind
   `FNA3D_SupportsDXT1`/`SupportsDXT5`-style checks.
3. Effects: `BlockEffect`, `WaterEffect` and `Tracer` are compiled XNA FX. FNA
   runs them through Mojoshader, which is built with effect support in the
   pinned FNA3D; verify GLSL-ES 3.0 translation and uniform binding on device.
4. Models and skinning: `SkinnedEffect`/`SkinnedModel` paths, bone palette size
   against `GL_MAX_VERTEX_UNIFORM_VECTORS` (a common Tegra limit).
5. Fonts and strings: `SpriteFont` plus the six localized XNBs.
6. Avatar animation: `Content/AvatarAnimation/*.xnb` is loaded by the game, so
   the avatar API must not throw even where rendering is a placeholder.

Exit criterion: a probe renders one textured model, one stock `BasicEffect`
mesh, one custom effect, and a string drawn with `SpriteFont`.

## Gate D — audio (XACT)

FNA implements XACT on FAudio's FACT engine (`FACTAudioEngine_*`), so no audio
reimplementation is needed.

1. Link `libFAudio.a`; SDL's Switch audio driver (`SDL_AUDIO_DRIVER_SWITCH`) is
   already present in the installed SDL 2.28.5.
2. Load `GlobalSettings.xgs`, the `.xsb` and both `.xwb` banks through the
   package filesystem, including streaming cues.
3. Verify category volumes, 3D positional cues and loop regions.
4. Keep a silent no-op path if audio init fails so the game still runs.

Exit criterion: music and effects play, and stopping/restarting the engine does
not leak or deadlock.

## Gate E — input and lifecycle

1. Map Switch controller state onto XNA `GamePad`/`Keyboard` (A/B/X/Y, sticks
   with dead zone, ZL/ZR, plus/minus, d-pad) and expose it through the existing
   FNA SDL2 backend.
2. Handle docked/handheld, suspend/resume, and controller connect/disconnect
   without leaking the GL context.
3. Map `+` (or `Home` exit) to a clean shutdown of mono and SDL.

Exit criterion: menus and gameplay respond correctly, and a 10-minute session
with sleep/resume behaves.

## Gate F — platform services

1. Saves: `StorageDevice`/`StorageContainer` onto `sdmc:/switch/castleminerz/saves`.
   The game's world saves are large — check free-space reporting and flushing.
2. GamerServices: already shimmed; confirm the game accepts the local profile,
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
