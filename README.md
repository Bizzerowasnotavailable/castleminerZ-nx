# castleminerZ-nx

An experimental Nintendo Switch homebrew interoperability and static-AOT
porting framework for the Xbox Live Indie Games release of **CastleMiner Z**.

> [!IMPORTANT]
> This repository does not contain CastleMiner Z, its executable assemblies,
> art, audio, or other copyrighted game data. You must supply your own Xbox 360
> `LIVE`/STFS package. Do not open issues asking for game files.

## Current status

**Playable in Eden; physical-hardware validation pending.** CastleMiner Z boots
to its title screen, enters and loads an offline world, and renders the complete
first-person scene. Terrain, sky, clouds, water, HUD, hotbar icons and the held
item draw correctly; movement, the first-person camera, the pause menu and the
mapped Switch controller input all work. Assets are read directly from the
user's STFS package and are never copied into the NRO or RomFS.

**Installation is a local source build, not a generic NRO plus a game rip.**
The NRO needs matching processed `CastleMinerZ.exe` and `DNA.Common.dll` metadata
on SD. Use the [package preparation workflow](docs/installation.md) below to
generate all three from your own package; do not download the DLL separately.

Verified in Eden:

- title screen, offline world entry, and loading of existing save data;
- generated terrain, sky/clouds, water, HUD, hotbar icons, and first-person held
  items;
- first-person camera, movement, and pause menu;
- local Switch profile nickname/picture with UID-scoped storage, no linked
  Nintendo Account; and
- mapped controller input (Nintendo labels: A=accept, B=back).

Known limitations:

- **Player body:** the third-person avatar body is invisible. Xbox avatar assets
  cannot be shipped; reporting the renderer as `Unavailable` keeps the game's own
  fallback skeleton, which preserves held-item, eye-point and animation
  transforms. The local profile supplies nickname/picture metadata only; no Mii
  geometry is read or rendered. A project-authored placeholder body is future
  work.
- **Audio:** FNA/FAudio and the XACT bank loader are linked, and the sound-effect
  bank is PCM. The 11 streaming music entries are WMA, which the current Switch
  FAudio build does not decode. Eden's audren renderer also never consumes SDL
  wave buffers (a standalone SDL tone gets one callback and then blocks), so no
  SDL audio can be heard in Eden. Hardware audio validation and a WMA policy
  remain open.
- **Validation:** inventory, combat, and a full power-cycle save/load test still
  need complete passes; no physical Switch run has been completed.
- **Behavior note:** brief full-sky white flashes are the game's original
  `drawLightning` storm effect (10–40 s cadence above ground), not a corrupted
  cubemap.

The project also provides:

- a real ARM64 `.nro` built with devkitA64/libnx;
- recursive STFS package discovery and a read-only package VFS without
  installing or embedding game assets;
- a host-side `cmztool` for listing, validating, and privately extracting
  individual files;
- a project-owned `CastleMinerZ.XnaCompat` assembly that covers the
  GamerServices, networking, and avatar surface FNA omits;
- a deterministic relinker that routes every legacy XNA type and member onto FNA
  or the compatibility assembly;
- the full native stack (SDL2 → EGL → switch-mesa GLES 3.0 → FNA3D →
  MojoShader, plus FAudio) linked into the Mono static-AOT image with no
  undefined symbols;
- ten project-authored replacement effects for the Xbox GPU binaries, with
  MojoShader GLSL ES 3 translation and interface tests;
- Xbox cubemap and raw model-buffer byte-order fixes, verified in Eden; and
- the local-profile integration described above
  (see [`docs/local-profiles.md`](docs/local-profiles.md)).

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

After installing the [Windows/WSL build prerequisites](docs/installation.md), run:

```powershell
./scripts/prepare-game.ps1 -Package 'C:\Games\my-owned-package' -OutputRoot 'C:\CMZ-private\sd-ready'
```

Supply the actual `LIVE`/STFS file, even if it has a long hexadecimal name and
no extension. The script validates it, privately extracts the two assemblies,
relinks/trims/AOT-compiles them and produces a **new, complete private SD folder**:

```text
sd-ready/
  switch/castleminerz.nro
  switch/castleminerz/game/CastleMinerZ.stfs
  switch/castleminerz/managed/CastleMinerZ.exe
  switch/castleminerz/managed/DNA.Common.dll
  switch/castleminerz/build-manifest.json
```

Copy the **entire `switch` folder** to your SD root, or into the SDMC directory
of the destination Eden installation. Use **File → Open Eden Folder** to locate
the active Eden data folder; portable installations need not use `%APPDATA%`.
The destination PC does not need the build tools. Keep existing saves, but move
old packages out of `switch/castleminerz/game/` so discovery cannot pick one.

With the game/Eden closed, verify the destination after copying:

```powershell
./scripts/verify-game-install.ps1 -SdRoot 'C:\path\to\eden\sdmc'
```

The verifier checks SHA-256 hashes of the package, NRO and both processed
assemblies against the build manifest. Do not mix builds or substitute the
original extracted assemblies. A package-only copy is **not** an installation.
See [installation and troubleshooting](docs/installation.md) for prerequisites,
rebuilds and the missing-`DNA.Common.dll` error.

Assets still stream directly from the unmodified package. Advanced/manual builds
can use any package filename under `sdmc:/switch/castleminerz/game/` (including
the original `584E07D2/00000002/` layout); the prepared workflow uses the fixed
filename above so the transfer can be checked without machine-specific paths.

## Build the Switch bootstrap

This builds the package-discovery bootstrap, **not the playable game**. For the
game use `prepare-game.ps1`; runtime/graphics probes below are also diagnostics.

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
- [`docs/installation.md`](docs/installation.md) covers preparing your own
  package, transferring the complete private build, and troubleshooting.
- [`docs/graphics.md`](docs/graphics.md) records the deko3d-versus-switch-mesa
  decision and the evidence behind it.
- [`docs/playability.md`](docs/playability.md) is the gate-by-gate plan to a
  running game, with exit criteria.
- [`docs/roadmap.md`](docs/roadmap.md) tracks the remaining work to gameplay.

## Distributing builds

`castleminerz.nro` is a **private build**, not a self-contained release. The
full game build AOT-compiles the user's `CastleMinerZ.exe` and `DNA.Common.dll`
into the NRO and reads the original STFS package at runtime. Publishing or
redistributing that NRO, its generated AOT objects, or the relinked managed
assemblies would redistribute copyrighted game code and assets. Share the
source, scripts, and documentation instead; each user must supply their own
package and build locally.

For personal use, transfer the complete output of `prepare-game.ps1`, not just
the NRO. It includes your unmodified package, matching **processed** assemblies
and a build manifest:

```text
sdmc:/switch/castleminerz.nro
sdmc:/switch/castleminerz/game/CastleMinerZ.stfs
sdmc:/switch/castleminerz/managed/CastleMinerZ.exe
sdmc:/switch/castleminerz/managed/DNA.Common.dll
sdmc:/switch/castleminerz/build-manifest.json
```

The NRO embeds a default runtime config and creates
`sdmc:/switch/castleminerz-logs/` automatically, so no extra config file is
required. An `aot_config.ini` in that folder overrides the embedded defaults for
debugging. A boot without the override was verified in Eden **with the required
package and processed assemblies still present**; this was not an empty-SD or
package-only installation test.

On hardware, launch in **full application mode** (hold R while starting an
installed game to open hbmenu), not from the album. The Mono runtime needs the
full memory budget and refuses applet mode.

The NRO also is not yet hardware-validated; Eden is the only tested
environment, and audio remains blocked there.

## Legal

This is an independent interoperability and preservation project. CastleMiner Z
and related names are property of their respective owners. The project is not
affiliated with or endorsed by DigitalDNA Games, Microsoft, or Nintendo.

Project-authored code is provided under the MIT license. Third-party tools and
libraries retain their own licenses.
