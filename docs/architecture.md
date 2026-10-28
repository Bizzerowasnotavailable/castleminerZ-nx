# Architecture

## Design constraints

1. The repository and distributed bootstrap contain no CastleMiner Z code or
   assets.
2. Runtime assets are read from a package supplied by the user.
3. The original game logic is managed IL, not Xbox 360 PowerPC code.
4. Nintendo Switch homebrew cannot assume a desktop JIT, graphics stack, XNA
   services, or filesystem layout.
5. Generated or relinked game assemblies and AOT objects are private build
   products and must never be committed.

## Current bootstrap

```text
libnx application
  -> recursively discover LIVE/PIRS/CON candidates on SD
  -> StfsPackage (bounds-checked, read-only package access)
  -> inspect_game_image (known layout and representative content checks)
  -> console status UI
```

`StfsPackage` normalizes package paths, rejects unsafe path components, maps
logical 4 KiB blocks around STFS hash tables, and supports bounded range reads.
The current supported package stores its payload contiguously. Fragmented STFS
file chains are detected and rejected rather than read incorrectly.

The host-side `cmztool` uses the same library, which keeps package behavior
testable without deploying to hardware. It can list, verify, or privately
extract one file. The NRO only discovers and verifies the package today.

## Target runtime

```text
libnx native host
  + package-backed stream/VFS
  + Mono for Nintendo Switch
      - interpreter for bring-up and diagnostics, where practical
      - ARM64 static AOT for release/private builds
  + XNA 4 compatibility assemblies
      - FNA-derived framework core and content model
      - CastleMinerZ.XnaCompat for GamerServices, Net, and Avatar
      - Switch graphics/audio/input platform backend
  + CastleMiner Z service adapters
      - gamer/sign-in and Guide UI
      - save storage
      - local session/network transport
      - avatar rendering and animation
      - XACT-compatible audio
```

The original assemblies reference Microsoft's split XNA 4 assemblies. Because
those references are strong-named to assemblies that cannot ship here, the
private build relinks metadata instead of relying on unsigned facade binding:
each old XNA type reference is routed to the assembly that actually defines it.
The project-owned [`CastleMinerZ.XnaCompat`](../compat/managed) assembly covers
GamerServices, networking, and avatars; pinned `FNA.dll` covers the remaining
XNA 4 surface.

### Managed compatibility boundary

The compatibility split is deliberately narrow and testable:

- `CastleMinerZ.XnaCompat` is one unsigned assembly containing only the 34
  types FNA omits, plus their supporting enums and event arguments.
- GamerServices is offline-first: local profiles sign in deterministically,
  Guide dialogs complete synchronously with sensible defaults, and profile
  pictures are generated placeholders.
- `NetworkSession` is an in-process loopback. It preserves XNA's object model,
  events, packet buffers, and validation so single-player code paths run; it
  does not pretend to provide Xbox Live or System Link transport.
- `AvatarRenderer` exposes the complete API shape but draws nothing and
  distributes no Microsoft avatar assets.

The [`Cmz.ManagedRelink`](../tools/managed-relinker) tool rewrites type scopes
per type, replaces the seven or eight legacy XNA assembly references with `FNA`
and `CastleMinerZ.XnaCompat`, prunes the legacy references, and verifies every
type and member against the target assemblies. Output uses a content-derived
MVID, a zero PE timestamp, and a byte-identical pass-through for assemblies
that are already compatible. The synthetic
[`Cmz.ManagedRelinkTests`](../tools/managed-relinker-tests) suite proves per-type
routing across both targets, reference pruning, determinism, and idempotence.

### Managed execution modes

- **Bring-up mode:** load original assemblies and metadata from the STFS VFS
  into a Mono interpreter. This minimizes generated artifacts and helps expose
  missing APIs, but performance and Switch executable-memory constraints must
  be evaluated.
- **Static-AOT mode (current playable build):** `scripts/prepare-game.ps1`
  extracts original assemblies from the user's supported package into an
  external private workspace, relinks/trims them, invokes the Switch-capable
  Mono AOT toolchain, and links generated ARM64 objects into a private NRO.
  Mono loads the matching processed assembly metadata from
  `sdmc:/switch/castleminerz/managed/`; the original package metadata is not
  interchangeable with that output. Preparation collects the NRO, both
  processed assemblies, package and hash manifest as a complete SD folder.
  Runtime package-only preparation and a generic public game NRO are not
  implemented. See [installation.md](installation.md).

The available `mono-nx` static-AOT SDK is Linux-oriented. The validated build
uses WSL for ILLink and `mono-aot-cross`, path-translating wrappers for the
Windows devkitA64 binutils, and devkitPro MSYS for final linking. This preserves
the upstream Linux AOT path without requiring a second devkitPro installation.
Exact pins and commands are in [`toolchain.md`](toolchain.md).

## Subsystems

### Content and VFS

The XNA `ContentManager` bridge should request package paths and consume
bounded streams without extracting the full game. Required work includes XNB
v5 parsing, LZX decompression for compressed XNBs, Xbox-endian payload support,
and the custom content readers implemented by `DNA.Common`/the game.

The package parser intentionally has a narrow first milestone. Before
supporting other STFS layouts it needs hash-chain traversal for fragmented
files, broader conformance tests, and optional integrity/signature reporting.

### Graphics and shaders

An FNA-style API layer is a good semantic starting point, but its desktop
backend cannot simply be linked to libnx. A Switch-native rendering backend
must implement the XNA graphics objects and state used by the game.

Xbox Effect XNBs need a dedicated conversion path. The proposed private
pipeline is:

```text
STFS -> XNB stream -> decompress/parse Effect -> Xbox shader bytes
     -> adapted XenosRecomp -> backend shader cache
```

This stays provisional until the Effect payload inventory is complete.

### Audio

The package uses XACT `.xgs`, `.xsb`, and `.xwb` data. The runtime needs either
an XACT-compatible parser/player or a private conversion/cache step, while
preserving cue, category, variation, loop, and streaming behavior. The files
remain user-supplied.

### Platform services

The minimum single-player adapters are a deterministic local gamer, Guide
dialogs, trial/full-game policy, save selection/storage, controller mapping,
and a local `NetworkSession` facade. Online multiplayer can follow only after
single-player behavior is stable because networking is coupled deeply into
game state and entity replication.

Xbox avatars are also a first-class dependency, not cosmetic optional code.
The runtime needs a compatible skeleton, default character mesh/materials,
animation playback, attachment bones, facial state, and camera transforms.
No Microsoft avatar assets may be distributed by this project.

## Legal build boundary

Public source code may include parsers, compatibility APIs, build automation,
tests using synthetic data, and project-authored replacement assets. It must not
include extracted XNB/XACT data, original or decompiled assemblies, copied
decompiled source, private keys, generated AOT game code, or shader caches
derived from the game. Private intermediates belong under ignored directories
or outside the repository.
