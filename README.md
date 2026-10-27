# castleminerZ-nx

An experimental Nintendo Switch homebrew interoperability and static-AOT
porting framework for the Xbox Live Indie Games release of **CastleMiner Z**.

> [!IMPORTANT]
> This repository does not contain CastleMiner Z, its executable assemblies,
> art, audio, or other copyrighted game data. You must supply your own Xbox 360
> `LIVE`/STFS package. Do not open issues asking for game files.

## Current status

**Managed compatibility milestone; not yet playable.** The project currently:

- builds a real ARM64 `.nro` with devkitA64/libnx;
- finds a user-provided STFS package on the SD card;
- exposes its file table through a read-only package VFS without installing or
  embedding game assets;
- verifies the expected managed assemblies and representative XNA assets;
- provides a host-side `cmztool` for listing, validating, and privately
  extracting individual files;
- provides a project-owned `CastleMinerZ.XnaCompat` assembly that covers the
  GamerServices, networking, and avatar surface FNA omits;
- deterministically relinks privately extracted game assemblies onto FNA plus
  that compatibility assembly, with every type and member verified;
- links the full native graphics stack (SDL2 → EGL → switch-mesa GLES 3.0 →
  FNA3D → MojoShader) into a 6.3 MB native hardware probe NRO; and
- runs FNA itself inside the Mono static-AOT image: the managed probe creates a
  real `GraphicsDevice`, clears and presents 240 frames, attempts a
  `SpriteBatch` draw, and links the same FNA3D/Mesa/FAudio archives with no
  undefined symbols.

**Next milestone:** confirm both probes on physical hardware, then serve the
package through a libnx filesystem so `ContentManager` can load the game's XNBs.

The graphics backend is settled: switch-mesa behind SDL2, not deko3d. deko3d
has no FNA3D driver in any public tree, so using it now would mean writing a
driver and a shader backend before anything renders. The reasoning and the
measured package facts are in [`docs/graphics.md`](docs/graphics.md), and the
gate-by-gate path to a playable build is in
[`docs/playability.md`](docs/playability.md).

Static analysis of the supplied release established that `CastleMinerZ.exe`
and `DNA.Common.dll` are IL-only, AnyCPU CLR 2.0 assemblies. CastleMiner Z is an
XNA 4 XBLIG game; it has no game XEX or PowerPC instruction stream for
XenonRecomp to translate. The correct static recompilation route is therefore
managed IL AOT to ARM64 with an XNA compatibility runtime. See
[`docs/architecture.md`](docs/architecture.md).

## SD-card layout

Copy your unmodified package to either location (the filename is arbitrary):

```text
sdmc:/switch/castleminerz/game/<package>
sdmc:/switch/castleminerz/game/584E07D2/00000002/<package>
```

The NRO recursively searches `sdmc:/switch/castleminerz/game` and reads assets
directly from the package. It does not copy game data into the NRO or RomFS.

## Build the Switch bootstrap

Requirements:

- devkitPro with devkitA64 and libnx
- GNU Make

From a devkitPro shell:

```sh
make -j$(nproc)
```

On Windows PowerShell with the standard `C:\devkitPro` installation:

```powershell
./scripts/build-switch.ps1
```

If local policy blocks scripts, use a process-scoped override:

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/build-switch.ps1
```

The output is `castleminerz-nx.nro`. Generated binaries are intentionally
gitignored.

## Build host tools and tests

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Examples:

```sh
cmztool list /path/to/package
cmztool verify /path/to/package
cmztool extract /path/to/package 584E07D1/Content/LoadScreen.xnb private/LoadScreen.xnb
```

`private/` is ignored. Never commit extracted files.

## Managed runtime dependencies

The Mono AOT/FNA work uses pinned sources and release archives kept outside the
repository. On Windows, the validated workflow combines devkitPro MSYS and WSL:

```powershell
./scripts/setup-dependencies.ps1 -InstallWslDotnet
./scripts/test-mono-runtime.ps1
./scripts/build-native-dependencies.ps1
./scripts/build-runtime-probe.ps1
./scripts/build-graphics-probe.ps1
./scripts/audit-managed-compatibility.ps1 -Assembly private/CastleMinerZ.exe,private/DNA.Common.dll
./scripts/relink-managed-assemblies.ps1 -Assembly private/CastleMinerZ.exe,private/DNA.Common.dll
./scripts/test-managed-relinker.ps1
```
See [`docs/toolchain.md`](docs/toolchain.md) for the exact host split, hashes,
outputs, and limitations. These smoke builds contain no CastleMiner Z code or
assets.

`castleminerz-graphics-probe.nro` is the hardware test for the graphics stack
alone: it prints the SDL platform, the GL vendor/renderer/version and the
FNA3D device result, then clears and presents a moving colour until `+` is
pressed. If it presents, the SDL2/Mesa/FNA3D path is proven and the remaining
graphics work is game-side.

## Research notes

- [`docs/analysis.md`](docs/analysis.md) records reproducible format and managed
  metadata findings without containing decompiled game code.
- [`docs/architecture.md`](docs/architecture.md) explains where XenonRecomp,
  XenosRecomp, and native Ghidra analysis do or do not apply and describes the
  Mono AOT/FNA path.
- [`docs/toolchain.md`](docs/toolchain.md) documents the pinned WSL/devkitPro
  runtime and native dependency builds.
- [`docs/graphics.md`](docs/graphics.md) records the deko3d-versus-switch-mesa
  decision and the evidence behind it.
- [`docs/playability.md`](docs/playability.md) is the gate-by-gate plan to a
  running game, with exit criteria.
- [`docs/roadmap.md`](docs/roadmap.md) tracks the remaining work to gameplay.

## Legal

This is an independent interoperability and preservation project. CastleMiner Z
and related names are property of their respective owners. The project is not
affiliated with or endorsed by DigitalDNA Games, Microsoft, or Nintendo.

Project-authored code is provided under the MIT license. Third-party tools and
libraries retain their own licenses.
