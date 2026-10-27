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
It has two transforms, both applied to the binary only so the pinned FNA source
stays pristine:

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
