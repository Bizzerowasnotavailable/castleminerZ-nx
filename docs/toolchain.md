# Toolchain and dependencies

Third-party source, SDK archives, generated managed assemblies, and AOT objects
must stay outside this repository. The scripts default to:

```text
%LOCALAPPDATA%\Temp\opencode\castleminerz-deps
```

Exact source commits, release hashes, reference-project revisions, and the
tested devkitPro versions are recorded in
[`dependencies.lock.json`](../dependencies.lock.json).

## Tested host split

The validated Windows setup uses:

- devkitPro MSYS for devkitA64 compilation and final libnx linking;
- WSL for the Linux `mono-aot-cross` executable and ILLink;
- a pinned Linux .NET 9 SDK installed in the external workspace; and
- generated shell wrappers that translate absolute WSL paths before invoking
  the Windows devkitA64 binutils.

The wrappers contain paths only. They do not copy the toolchain, game files, or
generated game code into the repository. `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT`
is used only to let the host .NET SDK run on a minimal WSL installation without
host ICU. The Switch build still links mono-nx's ICU libraries and data.

## Fetch and verify

From PowerShell:

```powershell
./scripts/setup-dependencies.ps1 -InstallWslDotnet
```

The script checks out pinned Git commits, validates the mono-nx release archive
hashes before extraction, and optionally installs the hash-pinned Linux .NET
SDK. It refuses to place the dependency workspace inside the repository and
does not download any CastleMiner Z content.

To verify an existing workspace without fetching or changing revisions:

```powershell
./scripts/setup-dependencies.ps1 -VerifyOnly -InstallWslDotnet
```

## Runtime smoke builds

Build the mono-nx interpreter and static-AOT examples with the installed
devkitPro and WSL environments:

```powershell
./scripts/test-mono-runtime.ps1
```

This performs the complete split-host AOT sequence:

1. build a .NET 9 managed assembly;
2. trim it with the mono-nx ILLink build;
3. run Linux `mono-aot-cross` for ARM64;
4. assemble through path-translating devkitA64 wrappers; and
5. link the resulting objects, Mono, ICU, and libnx into an NRO.

The generated examples remain under the external mono-nx checkout. Successful
host builds do not replace physical Switch testing.

Build the project-owned probe NRO with:

```powershell
./scripts/build-runtime-probe.ps1
```

This stages only tracked mono-nx host sources in the external workspace, AOT
compiles [`runtime-probe/managed/Program.cs`](../runtime-probe/managed/Program.cs),
and copies `castleminerz-runtime-probe.nro` to the repository root as an ignored
build output. On hardware it reports platform/runtime details and exercises the
managed entry point, reflection, a worker thread, garbage collection, a timer,
read-only SD-root discovery, the XNA compatibility shims, and then a real FNA
frame: it releases the libnx console, creates a 1280x720 `GraphicsDevice`
through FNA3D, clears and presents 240 frames, attempts a `SpriteBatch` draw,
reclaims the console, and writes `sdmc:/switch/castleminerz-logs/fna-graphics-probe.log`.

The most recent build is 85,662,773 bytes, SHA-256
`699973FAD69C750373399390A6E41179B3233FB806DFA9844CB29814994127B6`, and its ELF
contains `FNA3D_CreateDevice`, `FNA3D_PrepareWindowAttributes`,
`SDL_GL_CreateContext`, `eglGetDisplay` and `FAudio_Initialize` with no undefined
symbols. It contains no CastleMiner Z files.

## FNA native libraries

The current FNA managed layer can select its retained SDL2 platform with
`FNA_PLATFORM_BACKEND=SDL2`. Build static SDL2 variants of its native graphics
and audio libraries with:

```powershell
./scripts/build-native-dependencies.ps1
```

Outputs are `libFNA3D.a`, `libmojoshader.a`, and `libFAudio.a` in each external
dependency's `build-switch` directory. FNA integration still requires managed
assembly relinking/facades, P/Invoke registration, and Switch-specific API-gap
work; producing these archives alone does not make the game playable.

## Graphics probe

`scripts/build-graphics-probe.ps1` links the project-authored native probe in
[`runtime-probe/graphics`](../runtime-probe/graphics) against FNA3D,
MojoShader, SDL2 and the switch-mesa EGL/GLES stack, producing
`castleminerz-graphics-probe.nro` (6,303,744 bytes, SHA-256
`C375FD4A75EED3F9E98BC109451155F265B501A15E9736AA60396D34AFC6722E`).

It contains no Mono and no game code. On hardware it prints the SDL version and
platform, the video driver, GL vendor/renderer/version, then the
`FNA3D_PrepareWindowAttributes`/`FNA3D_CreateDevice` result, waits for `+`, and
presents a moving clear colour until `+` is pressed again. This single run
answers whether switch-mesa is usable at all, which is the largest open
graphics risk.

The Makefile sets `LD := $(CC)` because devkitA64's `base_rules` uses `$(LD)`
without defining it in a plain MSYS shell, and links `-lstdc++` because the
Mesa EGL archive contains C++ (nv50 IR) objects. Both are host-build details,
not Switch-specific hacks.

## Managed AOT preparation

`tools/managed-aot-prep` rewrites the compiled FNA assembly before ILLink runs.
Its transforms apply to the binary only. Separate checked dependency patches
implement the effect loader, Xbox cubemap/model-buffer byte order, GLSL ES
volume-sampler precision and reliable native file diagnostics:

- `--stub <Type>::<Method>` replaces a method body with `return;`. Used for
  `Microsoft.Xna.Framework.FNADllMap::Init`, a `[ModuleInitializer]` whose XML
  body would otherwise pull `System.Private.Xml` and the OpenSSL
  `CryptoNative_*` closure into the image.
- `--stub-pinvoke-module <library>` replaces every `DllImport` in that native
  library with a managed `default(T)` body. `mono-aot-cross` compiles all
  methods and the static AOT module table keeps them alive, so FNA's unused
  SDL3 and Emscripten bindings would otherwise leave `SDL_OpenGamepad`,
  `emscripten_set_main_loop` and hundreds of siblings undefined at link time.
  The build passes `SDL3` and `__Native`; FNA is forced onto SDL2.
- `--stub-pinvoke-name <entry point>` handles unused Android, WinRT and Apple
  SDL2 imports that the Switch SDL2 archive does not export. Unused video-decoder
  modules are also stubbed for the game build.

The tool is deterministic (content-derived MVID), idempotent, refuses
strong-named inputs and in-place rewrites, and re-verifies the written assembly
before exiting.

## Private managed compatibility audit

Given privately extracted managed assemblies, build pinned FNA, the
project-owned compatibility assembly, and Mono.Cecil, then print aggregate XNA
compatibility counts with:

```powershell
./scripts/audit-managed-compatibility.ps1 `
  -Assembly private/CastleMinerZ.exe,private/DNA.Common.dll
```

The report identifies XNA assembly identities and counts referenced types and
members that resolve against `FNA` plus `CastleMinerZ.XnaCompat`. It
intentionally does not emit method bodies, decompiled code, rewritten
assemblies, or a symbol-by-symbol game report.

## Private managed relinking

Build the compatibility assembly, then rewrite privately extracted game
assemblies into the external workspace:

```powershell
./scripts/relink-managed-assemblies.ps1 `
  -Assembly private/CastleMinerZ.exe,private/DNA.Common.dll
```

By default the relinker writes to
`%LOCALAPPDATA%\Temp\opencode\castleminerz-analysis\relinked`. Each old XNA
type reference is routed to the assembly that defines it, legacy XNA assembly
references are replaced with `FNA` and `CastleMinerZ.XnaCompat`, and every type
and member is re-verified after the file is written. Output is deterministic
(content-derived MVID, zero timestamp), and an already-compatible input is
copied through byte-for-byte.

Run the synthetic relinker test suite with:

```powershell
./scripts/test-managed-relinker.ps1
```

It generates a small assembly that references a synthetic legacy XNA scope,
proves that `GameTime` routes to FNA while `Guide` routes to
`CastleMinerZ.XnaCompat`, and checks reference pruning, determinism, and
idempotence. No game content is involved.

## Replacement effects

The ten `.fx` files in `shaders/effects` are project-authored approximations, not
decompiled or translated Xbox shaders. Only their public effect interfaces and
vertex layouts are preserved. Compile them on Windows with the installed DirectX
June 2010 runtime (`d3dx9_43.dll` matching the .NET host architecture):

```powershell
./scripts/build-effects.ps1
./scripts/test-effects.ps1
```

Outputs stay in the external workspace. Tests use the pinned MojoShader parser
and GLSL ES 3 translator and verify parameter names, selected layouts, semantics,
and numeric technique/pass order. These checks do not prove driver compilation
or visual correctness on a Switch. Xbox state enum numbers are not copied into
PC effects; sources use named D3D9 sampler/render states instead.

An optional WSL `glslangValidator` path enables strict compilation of every
translated shader stage (tested with glslang 16.2.0):

```powershell
./scripts/test-effects.ps1 -GlslangValidator /path/to/glslangValidator
```

All 58 stages pass this check. Driver linking, rendering and performance still
require runtime tests.

`scripts/apply-dependency-patches.ps1` checks each patch in both directions, is
idempotent, and refuses an unexpected dependency source state. The native build
applies the FNA3D and MojoShader patches; game builds apply the FNA and mono-nx
patches.

The FNA3D patch recognizes `CMZ_FIRST_ENVIRONMENT_SLICE_ONLY=1`, set by the
native host. The current `BlockEffect` samples the four-layer environment volume
only at Z `0.125`, the center of slice zero. The patch uploads only that slice
for level-zero, depth-four updates starting at Z zero; all other uploads retain
their original depth. This removes Eden's unsupported layered-DMA warnings.
Disable or revise this optimization if a shader begins sampling other layers.

## Content reader regression tests

```powershell
./scripts/test-content-readers.ps1
```

The test compiles the pinned FNA `TextureCubeReader` and `X360TexUtil` sources
against a recording graphics boundary. It checks all six faces and four mip
levels, including dimensions smaller than one DXT block, for Xbox Color/BGRA and
DXT1/3/5 data, desktop passthrough, and existing texture reuse. All 480 synthetic
face/mip uploads must match their expected linear bytes. No assets or native GPU
libraries are used. The test failed against the original unswapped reader and
passes with `0002-xbox-cubemap-byte-order.patch`.

The same harness compiles FNA's real vertex declaration, vertex buffer and index
buffer readers. It checks all XNA vertex formats, mixed/interleaved attributes,
untouched stride padding, empty buffers, both index widths, existing index
buffer reuse, desktop passthrough and malformed Xbox declarations. The recording
boundary links the dependency's actual graphics enums rather than inventing
different numeric format values.

For read-only diagnostics of a privately extracted model or cubemap:

```powershell
./scripts/audit-content.ps1 -Asset C:\path\to\private\Content\Torch.xnb
```

This bounded XNB v5/LZX audit prints only reader/layout metadata, position bounds
under each byte order, and index-range counts. It never writes decompressed or
converted content. It supports plain models with null tags and BasicEffect
resources, plus cubemaps; unknown layouts fail rather than guessing. The torch
and pickaxe have plausible big-endian model bounds and valid big-endian indices,
but invalid little-endian ranges before conversion.

## Private game build

### Recommended: start with your own package

Use the [preparation and installation guide](installation.md) for prerequisites
and complete SD deployment:

```powershell
./scripts/prepare-game.ps1 -Package C:\path\to\owned-package -OutputRoot C:\CMZ-private\sd-ready
./scripts/verify-game-install.ps1 -SdRoot C:\CMZ-private\sd-ready
```

This validates the package, extracts the original EXE/DLL privately, runs the
full game build and collects the matching NRO, processed assemblies, unmodified
package and hash manifest. It never stages files directly into live Eden and
refuses an existing output folder. `-DependenciesReady` is for an already
provisioned build workspace only; it still runs the full game compilation.

The NRO alone, or NRO plus package, is not sufficient. Original extracted
assemblies cannot substitute for the exact processed assemblies paired with
the NRO's AOT code. The complete prepared output remains private.

### Advanced: already extracted assemblies

With privately extracted original assemblies available outside the repository:

```powershell
./scripts/build-game.ps1 -ManagedSource C:\path\to\private\584E07D1
```

This lower-level command retains its development defaults: it writes the NRO
to the repository root (ignored) and stages the processed assemblies into
`%APPDATA%\eden\sdmc` if present. Override `-OutputDirectory` and `-SdRoot` to
isolate those outputs. It does not create a complete installation/manifest or
copy the original package; prefer `prepare-game.ps1` for installation.

Close Eden before replacing the NRO: Windows keeps the running image open.
If only the final copy fails, the completed NRO remains in the external
`cmz-game-stage/native/aot/` directory. Its matching trimmed game assemblies are
under `output/`; deploy both together after stopping the emulator.

The script relinks, trims and AOT-compiles the private assemblies. It keeps their
matching trimmed IL on the SD card under `switch/castleminerz/managed/`; neither
game assembly goes into RomFS. The host preloads `DNA.Common.dll` from that same
directory before opening the executable because Mono resolves static-AOT
dependencies before establishing the entry assembly's base directory.

RomFS contains the runtime/framework, compatibility assembly and project-authored
replacement FXBs, never extracted game content. The NRO contains private generated
game AOT machine code and is ignored, not a redistributable source-project build.
Never commit it, the extracted inputs, the AOT objects, or shader caches.

The corlib AOT step reserves 16,384 generic trampolines; 4,096 was exhausted during
game startup. All game AOT assembly and native linking outputs remain external.
The native host writes `host.log`, and the default config redirects runtime output
to `runtime.log`, under `sdmc:/switch/castleminerz-logs/`. An SD `aot_config.ini`
there overrides the embedded configuration. File diagnostics are unbuffered so
native crashes do not hide the last messages.

Local-profile behavior and tests are documented in
[`local-profiles.md`](local-profiles.md). Neither the native host nor managed
compatibility layer contacts Nintendo Account or Xbox Live services.

## Eden runtime capture

Use Eden's **Tools → Capture Screenshot** for visual checks. Windows desktop
captures can show a black game window even when rendering works. The optional
Windows helper targets an explicitly selected, already-running game process:

```powershell
./scripts/eden-input.ps1 -ProcessId <eden-pid> -Key Start -Capture
```

It assumes the development keyboard mapping `M` = Plus/Start, `C` = A/accept,
`X` = B/back, arrow keys = d-pad, and WASD = left stick. It does not launch
another emulator or change controls. Screenshots are asynchronous; enumerate the newest PNG in
Eden's screenshot directory in a subsequent call. These private GPU captures
contain game imagery and must not be committed.

The native host sets `FNA_GAMEPAD_IGNORE_PHYSICAL_LAYOUT=1`, but the pinned
Switch SDL backend ignores that label hint for its fixed controller mapping.
`SDL_GAMECONTROLLERCONFIG` therefore overrides only the built-in libnx GUID
with A/B/X/Y mapped by Nintendo label. The original stick, trigger, shoulder and
d-pad bindings are retained. Without that override, physical Switch A reaches
the game as Xbox B/back. Recheck the GUID and runtime mapping if the SDL port is
updated; the tested log shows `a:b0,b:b1,x:b2,y:b3`.
