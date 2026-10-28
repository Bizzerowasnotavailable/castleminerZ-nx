# Prepare and install your own game

## What is supported

This is a **source-build workflow**, not a public prebuilt game release. Supply
your own supported CastleMiner Z Xbox 360 `LIVE`/STFS package. A rip may be a
single extensionless hexadecimal-named file; select that file, not its parent
directory. Other GOD containers/layouts, fragmented payloads, and other game
revisions are not automatically converted or supported.

The original package contains `584E07D1/CastleMinerZ.exe` and
`584E07D1/DNA.Common.dll`. You do not obtain them from a DLL download site. The
preparation script extracts them privately, processes them for FNA/.NET and
builds matching ARM64 AOT code. **The original extracted files cannot replace
the processed files used by a particular NRO.**

The current runtime still loads those processed assemblies from SD; it does
not derive them from the package at startup. Static AOT also embeds generated
game code in the NRO. Consequently, neither the NRO nor the processed assemblies
are public release artifacts. Share source/scripts/docs only, not a prepared SD
folder, extracted assemblies, or generated game code.

## Build PC prerequisites (Windows x64)

- Git, Windows .NET 8 SDK, and Windows PowerShell.
- devkitPro at `C:\devkitPro` (or pass `-DevkitPro`), with devkitA64, libnx,
  CMake/Make, SDL2 and Mesa Switch portlibs. The tested packages and versions
  are listed in [`dependencies.lock.json`](../dependencies.lock.json).
- A working WSL Linux x64 distribution with `g++`, `binutils-aarch64-linux-gnu`,
  `bash`, and `tar`. WSL must already be installed and initialized.
- The Microsoft DirectX June 2010 runtime, including `d3dx9_43.dll` for the
  architecture of the Windows .NET host, for compiling project-authored effects.
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

`OutputRoot` must be a **new directory outside the source repository**. It is
an output folder, not your live Eden SDMC directory or SD card. Without an
explicit output path the default is
`%LOCALAPPDATA%\Temp\opencode\castleminerz-deps\prepared-sd`.
The dependency workspace also stays outside the repository; override it with
`-Workspace 'D:\CMZ-private\dependencies'` if desired.

The script:

1. Builds a host `cmztool` with WSL `g++` and runs the synthetic parser test.
   Alternatively, `-CmzTool 'C:\tools\cmztool.exe'` selects your built Windows
   host tool. It must not be the Switch executable.
2. Validates the supported package layout before fetching dependencies or
   compiling game code. This is not Xbox signature/hash-tree authentication.
3. Sets up pinned dependencies and builds native FNA3D/FAudio libraries.
4. Extracts only the original game EXE and DNA DLL into a unique private run
   directory and invokes the existing relink → ILLink → AOT → native pipeline.
5. Collects the matching NRO and processed metadata with a byte-for-byte copy
   of your package. Art/audio remain in that package, not the NRO's RomFS.
6. Writes and verifies a manifest with SHA-256 and sizes for all four files,
   then moves the finished SD tree to `OutputRoot`.

No live Eden directory is changed. Failed builds retain private diagnostics in
`<Workspace>\cmz-prepare-<id>\` and do not publish `OutputRoot`; incomplete
outputs must not be used. Existing output folders are never merged/overwritten.
Do not run simultaneous game builds in the same dependency workspace: the
underlying build still uses a shared `cmz-game-stage` directory.

On an already prepared build PC, `-DependenciesReady` skips dependency fetching
and native-library rebuilds. It **does not** skip relinking, trimming or AOT:
the NRO and SD assemblies must always be produced together. No game files are
downloaded by either mode.

## Transfer to Eden or Switch

The output is:

```text
sd-ready/
  switch/castleminerz.nro
  switch/castleminerz/game/CastleMinerZ.stfs
  switch/castleminerz/managed/CastleMinerZ.exe
  switch/castleminerz/managed/DNA.Common.dll
  switch/castleminerz/build-manifest.json
```

Close the game/Eden before replacing files. Copy the **entire `switch` folder**
to the SD root. For Eden, use **File → Open Eden Folder** and copy into its
`sdmc` directory; portable/custom installations can use a different data root
from `%APPDATA%\eden`. The receiving PC only needs Eden and this complete
private installation, not devkitPro, WSL, DirectX, or a desktop DLL installation.
Close Eden before preparation too if your input package is inside its active
SDMC folder and it has the package open.

For an existing install, back up saves and preserve `switch/castleminerz/save/`
and profile data. Move older package files out of `switch/castleminerz/game/`
before copying the prepared one, so recursive discovery cannot choose an old
package. Copy the NRO, both managed files, package and manifest as one set.

With the game/Eden closed (the running game can lock the package), check the
destination using the source repository's verifier:

```powershell
./scripts/verify-game-install.ps1 -SdRoot 'C:\path\to\eden\sdmc'
# Or a mounted physical SD:
./scripts/verify-game-install.ps1 -SdRoot 'E:\'
```

The manifest checks transfer integrity/build pairing, not publisher authenticity.
It contains relative paths only. Verification does not modify the installation
and rejects missing files, mixed hashes, and extra files in the package folder.

No external `aot_config.ini` is required. The NRO embeds defaults and creates
`switch/castleminerz-logs/`. A pre-existing config in that directory overrides
those defaults; inspect it for stale assembly paths after moving installations.
The earlier successful no-override boot retained the package, managed files and
saves—it was **not** proof that an NRO plus a raw package was sufficient.

On Switch, launch through hbmenu in **full application mode** (hold R while
starting an installed game), not album/applet mode. Physical Switch validation
is still pending; playable Eden results do not establish hardware compatibility
or working audio. See the [README limitations](../README.md#current-status).

## Missing or failed-to-load DNA.Common.dll

- Run the destination verifier. The DLL must be at
  `sdmc:/switch/castleminerz/managed/DNA.Common.dll`, alongside the matching
  processed `CastleMinerZ.exe`.
- If you copied only the NRO and rip, run the preparation workflow and copy
  its complete result. The current runtime does not prepare assemblies on boot.
- If hashes differ, restore **all matching outputs** from the same prepared
  build. Do not substitute the original package DLL, a PC edition DLL, or
  assemblies from another build, and do not register/install it in Windows.
- A manually staged older build has no manifest. Rebuild through preparation
  to obtain a verifiable set; the verifier does not certify an arbitrary NRO.
- If verification passes but launch still fails, check the active Eden SDMC
  root and any `switch/castleminerz-logs/aot_config.ini` override. Preserve
  `host.log` and `runtime.log` from that directory with the exact error.
- Prefer a short portable Eden path on Windows. An isolated test under a deeply
  nested temporary directory rendered a world but logged save-path `CreateDirs`
  errors. Path length is suspected, not confirmed; that test does not establish
  working fresh-install saving. Back up saves before relocating an installation.

## Validation status

A real supported package was prepared through extraction, relinking, ILLink,
AOT and native linking on the existing build PC (`-DependenciesReady`). The
complete output passed manifest verification, then loaded both assemblies and
rendered an offline world in an isolated portable Eden data directory on the
same PC, without old managed files, package, saves or an SD config override.
This is not a newly provisioned toolchain test, an actual second-PC test, or
physical Switch validation. Save/load remains subject to the caveat above.

## Regression tests

```powershell
./scripts/test-game-preparation.ps1
```

The 17 checks use synthetic package/compiler boundaries to exercise actual orchestration,
transfer to a different folder, manifest validation, wrong inputs, missing/mixed
files, failed builds and output safety without game content or the Switch SDK.
They do not replace a real full-pipeline build and runtime test, nor a build on
a newly provisioned PC.
