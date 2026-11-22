# castleminerZ-nx

A Nintendo Switch homebrew port of the Xbox Live Indie Games release of
**CastleMiner Z**, built locally from your own copy of the game.

> [!IMPORTANT]
> This repository does not contain CastleMiner Z, its executable assemblies,
> art, audio, or other copyrighted game data. You must supply your own Xbox 360
> `LIVE`/STFS package. Do not open issues asking for game files.

## Status

**Version 0.5.2 is fully playable on a physical Switch** (owner testing). The
game reaches its title screen and creates and loads offline worlds. Inventory,
combat and the rest of the core game loop work.

- It runs smoothly. With logging turned off it feels like a steady 60 fps.
- Music and sound effects play cleanly.
- With SaltyNX installed, Status Monitor's FPS counter shows the game's frame
  rate.

The game also runs in the Eden emulator, but without audio, because Eden cannot
play SDL audio.

Known limitations:

- **Player body:** the third-person avatar body is invisible, because Xbox
  avatar assets cannot be shipped. The first-person view, held items and
  animations are correct.
- **Offline only:** there is no Xbox Live, so online features and multiplayer
  are not available. Your local Switch profile stands in for the Xbox gamer
  (name and picture), and no Nintendo Account is needed.
- **Lightning:** brief full-sky white flashes are the game's own lightning, not
  a rendering fault. Above ground there are 0–3 flashes every 10–39 s, followed
  by thunder.

## Install

You can't download a ready-made NRO. The NRO embeds code compiled from your copy
of `CastleMinerZ.exe` and `DNA.Common.dll`, so it has to be built on your PC
from your own package, and it only works with that package. The build PC needs
Windows with WSL, the .NET 8 SDK and devkitPro. The full prerequisites are in
[`docs/installation.md`](docs/installation.md).

```powershell
./scripts/prepare-game.ps1 -Package 'C:\Games\my-owned-package' -OutputRoot 'C:\CMZ-private\sd-ready'
```

Pass the `LIVE`/STFS file itself, which often has a long hexadecimal name and no
extension. Add `-Icon 'C:\path\to\image.png'` to give the game an hbmenu icon.
The script validates the package, builds the game and writes a new private SD
folder:

```text
sd-ready/
  switch/castleminerz.nro
  switch/castleminerz/game/584E07D2/00000002/<package file>
  switch/castleminerz/build-manifest.json
```

Copy `switch\castleminerz.nro` and the `switch\castleminerz` folder to the root
of your SD card, or into Eden's SDMC folder. Keep your saves, but remove older
packages from `switch/castleminerz/game/`. To check a copy, close the game and
run:

```powershell
./scripts/verify-game-install.ps1 -SdRoot 'E:\'
```

On the Switch, start the game from hbmenu in **full application mode**: hold R
while launching an installed game. Do not use the album, because the runtime
needs the full memory budget.

The NRO and the prepared SD folder are **private builds**. They contain code
compiled from the game, so do not share or publish them. Share this source
instead.

## Settings

All settings are optional. Put them in `sdmc:/switch/castleminerz/config.ini`:

```ini
logging = 0         ; write no logs at all, which runs smoother (default: logging on)
audio_buffers = 3   ; audio buffers kept queued, 2-4 (default 4); fewer means lower sound latency
saltynx = 0         ; do not publish the frame rate to SaltyNX tools (default: on)
```

With logging on, the game writes `host.log`, `perf.log` and `runtime.log` to
`sdmc:/switch/castleminerz-logs/`. [`docs/performance.md`](docs/performance.md)
explains them and the optional sampling profiler.

## How it works

CastleMiner Z is an XNA 4 game. Its `CastleMinerZ.exe` and `DNA.Common.dll` are
IL-only .NET assemblies, so there is no Xbox machine code to recompile. The
build relinks them onto [FNA](https://fna-xna.github.io/) and a project-owned
compatibility assembly, trims them, and AOT-compiles everything to ARM64 with
mono-nx into a libnx NRO. At runtime:

- Assets stream straight from the unmodified STFS package through a read-only
  filesystem. Nothing is extracted to the SD card.
- Graphics run on SDL2, switch-mesa (OpenGL ES 3) and FNA3D. Ten
  project-authored effects replace the Xbox shader binaries. The scene renders
  at 75% resolution and is scaled to 1280×720.
- Audio runs on FAudio (XACT), with FFmpeg decoding the xWMA music. A
  project-owned SDL audio backend keeps the sound output fed.

More detail is in [`docs/architecture.md`](docs/architecture.md),
[`docs/graphics.md`](docs/graphics.md), [`docs/toolchain.md`](docs/toolchain.md)
and [`docs/local-profiles.md`](docs/local-profiles.md).

## For developers

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

This builds and tests `cmztool`, which lists, verifies and privately extracts
package files (`cmztool list|verify|extract <package> ...`). Keep extracted
files in the ignored `private/` folder.

- **Dependencies:** mono-nx, FNA, FNA3D, FAudio and .NET for WSL are pinned in
  `dependencies.lock.json`. `scripts/setup-dependencies.ps1` fetches them
  outside the repository.
- **Native libraries:** `scripts/build-native-dependencies.ps1` builds FNA3D
  and FAudio. Rerun it after changing anything under `patches/fna3d`,
  `patches/mojoshader` or `patches/faudio`.
- **Checks:** run `scripts/check-repository.ps1` and the `scripts/test-*.ps1`
  scripts.
- **Bootstrap:** `make` at the repository root, or `scripts/build-switch.ps1`,
  builds only the package-discovery bootstrap, a diagnostic NRO. It does not
  build the game.

[`docs/toolchain.md`](docs/toolchain.md) describes the full build pipeline.

## Documentation

- [`CHANGELOG.md`](CHANGELOG.md): version history.
- [`JOURNEY.md`](JOURNEY.md): the project log, measurements and open work.
- [`docs/installation.md`](docs/installation.md): preparing, installing and
  troubleshooting.
- [`docs/performance.md`](docs/performance.md): `perf.log`, the profiler and
  hardware findings.
- [`docs/architecture.md`](docs/architecture.md),
  [`docs/graphics.md`](docs/graphics.md),
  [`docs/toolchain.md`](docs/toolchain.md) and
  [`docs/analysis.md`](docs/analysis.md): design, decisions and format notes.
- [`docs/playability.md`](docs/playability.md) and
  [`docs/roadmap.md`](docs/roadmap.md): status by area and remaining work.

## Legal

This is an independent interoperability and preservation project. CastleMiner Z
and related names are property of their respective owners. The project is not
affiliated with or endorsed by DigitalDNA Games, Microsoft, or Nintendo.

Project-authored code is provided under the MIT license. Third-party tools and
libraries retain their own licenses.
