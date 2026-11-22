# Prepare and install your own game

## What is supported

This is a **source-build workflow**, not a public prebuilt game release. Supply
your own supported CastleMiner Z Xbox 360 `LIVE`/STFS package. A rip may be a
single extensionless hexadecimal-named file, or the title folder it came from
(`584E07D2\00000002\<file>`). Select the file, not its parent directory. Other
GOD containers/layouts, fragmented payloads, and other game revisions are not
converted or supported.

The package contains `584E07D1/CastleMinerZ.exe` and `584E07D1/DNA.Common.dll`.
The build extracts them privately, relinks them for FNA/.NET, trims them with
ILLink, and compiles them to ARM64 AOT code. **The processed assemblies are
embedded in the private NRO** (under its RomFS `managed/` folder). The SD card
therefore needs only the NRO and your package. Nothing is extracted to the SD
card, and the package's art and audio are read from the package in place.

The NRO contains generated game code and the processed game assemblies. Neither
the NRO nor a prepared SD folder is a public release artifact. Share source,
scripts and docs only.

## Build PC prerequisites (Windows x64)

- Git, the Windows .NET 8 SDK, and Windows PowerShell.
- devkitPro at `C:\devkitPro` (or pass `-DevkitPro`), with devkitA64, libnx,
  CMake/Make, SDL2, Mesa and FFmpeg Switch portlibs. Install FFmpeg (the game's
  music decoder) from a devkitPro MSYS2 shell with `pacman -S switch-ffmpeg`;
  the native build stops with that command if it is missing. The tested
  packages and versions are listed in
  [`dependencies.lock.json`](../dependencies.lock.json).
- A working WSL Linux x64 distribution with `g++`, `binutils-aarch64-linux-gnu`,
  `bash`, and `tar`. WSL must already be installed and initialized.
- `d3dx9_43.dll` and `D3DCompiler_43.dll` for the x64 .NET host, used to compile
  the project-authored effects. Either install the Microsoft DirectX June 2010
  runtime, or extract the two `Jun2010_*_x64.cab` files from its redistributable
  into `<Workspace>\d3dx9\`. `build-effects.ps1` copies the workspace copies
  beside its compiler, so nothing is installed system-wide.
- Network access for pinned source/SDK downloads on the first build, and space
  outside the repository for dependencies, AOT outputs, and your private SD copy.

The script installs/verifies the pinned external source dependencies and Linux
.NET SDK and builds the native libraries. It does **not** install the Windows
SDK, devkitPro/portlibs, DirectX runtime, Linux OS packages, or WSL itself.
See [`toolchain.md`](toolchain.md) for the lower-level stages.

## One package-to-install command

From the repository root in PowerShell:

```powershell
./scripts/prepare-game.ps1 `
  -Package 'C:\Games\my-owned-package' `
  -OutputRoot 'C:\CMZ-private\sd-ready'
```

If local script policy blocks this command, invoke it with
`powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\prepare-game.ps1`
and the same arguments. No machine-wide policy change is needed.

`-Icon 'C:\path\to\image.png'` sets the hbmenu icon. The image may be in any
format that Windows can read, and it is converted to the 256×256 JPEG that the
NRO needs. Without `-Icon`, hbmenu shows libnx's default icon.

`OutputRoot` must be a **new directory outside the source repository**. It is
an output folder, not your live Eden SDMC directory or SD card. Without an
explicit output path the default is
`%LOCALAPPDATA%\Temp\opencode\castleminerz-deps\prepared-sd`.

The script:

1. Builds a host `cmztool` with WSL `g++` and runs the synthetic parser test.
   Alternatively, `-CmzTool 'C:\tools\cmztool.exe'` selects your built Windows
   host tool. It must not be the Switch executable.
2. Validates the supported package layout before fetching dependencies or
   compiling game code. This is not Xbox signature/hash-tree authentication.
3. Sets up pinned dependencies and builds native FNA3D/FAudio libraries.
4. Extracts only the original game EXE and DNA DLL into a unique private run
   directory and runs relink → ILLink → AOT → native link. The processed
   assemblies go into the NRO's RomFS.
5. Copies the NRO and your package, byte for byte, into the SD layout below.
6. Writes and verifies a manifest with SHA-256 and sizes for both files, then
   moves the finished tree to `OutputRoot`.

No live Eden directory is changed. Failed builds retain private diagnostics in
`<Workspace>\cmz-prepare-<id>\` and do not publish `OutputRoot`. Existing output
folders are never merged or overwritten. Do not run simultaneous game builds in
the same dependency workspace, because the build uses a shared
`cmz-game-stage` directory.

On an already prepared build PC, `-DependenciesReady` skips dependency fetching
and native-library rebuilds. It **does not** skip relinking, trimming or AOT:
the NRO must always be produced from the matching package.

Because it skips the native rebuild, run `./scripts/build-native-dependencies.ps1`
first after any change under `patches/fna3d`, `patches/mojoshader` or
`patches/faudio`, or after pulling such a change. Otherwise the NRO links the
previous `libFNA3D.a`/`libFAudio.a` without telling you. FNA and mono-nx
patches do not need this, because the game build applies and rebuilds those
itself.

## Transfer to Eden or Switch

The output is:

```text
sd-ready/
  switch/castleminerz.nro
  switch/castleminerz/game/584E07D2/00000002/<package file>
  switch/castleminerz/build-manifest.json
```

Copy **`switch\castleminerz.nro` and the `switch\castleminerz` folder** to the SD
root. The `game` folder is the GOD-style layout that the host searches for the
package, so it can be copied as it is. For Eden, use **File → Open Eden Folder**
and copy into its `sdmc` directory; portable/custom installations can use a
different data root from `%APPDATA%\eden`. The receiving PC only needs Eden and
this installation. It does not need devkitPro, WSL, DirectX, or any DLL
installed on the desktop.

Close the game/Eden before replacing files. For an existing install, back up
saves and profile data (`switch/castleminerz/save/`). Remove older files under
`switch/castleminerz/game/` before copying a new package, so discovery cannot
choose an old one. The NRO and the package must come from the same build.

With the game/Eden closed, check the destination with the source repository's
verifier:

```powershell
./scripts/verify-game-install.ps1 -SdRoot 'C:\path\to\eden\sdmc'
# Or a mounted physical SD:
./scripts/verify-game-install.ps1 -SdRoot 'E:\'
```

The manifest checks transfer integrity and build pairing, not publisher
authenticity. It contains relative paths only. Verification rejects missing or
changed files and extra files under `game`. It warns about a `managed` folder
left by an older build; that folder is no longer read and can be deleted.

No external `aot_config.ini` is required. The NRO embeds its defaults and creates
`switch/castleminerz-logs/`. A pre-existing config in that directory overrides
those defaults; inspect it for stale paths after moving installations.

Logging is on by default. `logging = 0` in `switch/castleminerz/config.ini`
turns it off; the NRO then writes no logs and does not create the log
directory. See [`performance.md`](performance.md#logging-toggle).

On Switch, launch through hbmenu in **full application mode** (hold R while
starting an installed game), not album/applet mode. The game boots on a
physical Switch (owner reports); frame rate, audio and full gameplay are still
being validated there, and playable Eden results do not establish them. See the
[README limitations](../README.md#current-status).

## If the game fails to start

- Run the verifier first. A changed NRO or package fails with a build mismatch;
  restore both from the same prepared build.
- Check `switch/castleminerz-logs/aot_config.ini` for a stale override.
- If there are no logs, check `switch/castleminerz/config.ini` for
  `logging = 0`, and remove it before reproducing the problem.
- Keep `host.log`, `runtime.log` and `perf.log` from
  `switch/castleminerz-logs/` with the exact error. `host.log` and `perf.log`
  are rewritten at each launch; `runtime.log` (Mono and console output) is
  appended to, so it also holds earlier launches. A Mono startup error appears
  at the end of `runtime.log`, after `host.log` stops at `mono_jit_init: begin`.
- Prefer a short portable Eden path on Windows. An isolated test under a deeply
  nested temporary directory logged save-path `CreateDirs` errors. Path length is
  suspected, not confirmed. Back up saves before relocating an installation.

## Validation status

`prepare-game.ps1 -DependenciesReady` builds the current layout (processed
assemblies embedded in the NRO, package in its GOD-style folder) end to end on
the build PC. Its output passes `verify-game-install.ps1`, runs in Eden, and
builds of this layout have booted on a physical Switch (owner reports, November
2026). Preparation on a newly provisioned build PC has not been tested.

## Regression tests

```powershell
./scripts/test-game-preparation.ps1
```

The 20 checks use synthetic package and compiler boundaries to exercise the
real orchestration and verification scripts: transfer to a different folder,
manifest validation, wrong or missing files, mixed hashes, extra files, failed
builds, missing NRO output, and output safety. They need no game content or Switch
SDK, and they do not replace a full build on a provisioned PC.
