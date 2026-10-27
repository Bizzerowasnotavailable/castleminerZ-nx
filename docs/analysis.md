# Analysis notes

This document records format and metadata facts needed to build the
compatibility layer. It intentionally contains no extracted assets or
decompiled game source.

## Supported input

The current validator targets the World XBLIG package used during research:

| Item | Value |
| --- | --- |
| Container type | Xbox 360 `LIVE`/STFS |
| Package size | 32,776,192 bytes |
| Package SHA-256 | `c18aa355c449b7517ffbdf706a528d402c6e5bb155984051b373ff7db7309e0c` |
| Files | 239 |
| Payload bytes | 31,947,256 |

The extracted inventory, used only in a private analysis directory, consists
of 224 XNB files, six managed DLLs (including five localized resource
assemblies), one managed executable, two XACT wave banks, one sound bank, one
global-settings file, two PNGs, one XML file, and one BIN file. All 224 XNBs
have the Xbox platform marker `XNBx` and XNA content version 5. Of these, 214
set the compressed-content flag and 10 do not.

`cmztool verify` checks package structure, expected paths and sizes, the CLR
data directory in `CastleMinerZ.exe`, an Xbox XNB marker, and the 239-file
inventory. It is a compatibility check, not an STFS signature verifier or a
substitute for the package SHA-256 above.

## Managed executable evidence

The package does not contain `default.xex`. Its executable payload is a normal
managed PE image:

| Assembly | Size | SHA-256 | Metadata inventory |
| --- | ---: | --- | ---: |
| `CastleMinerZ.exe` | 439,296 | `e378b91ed3c088e8d89094213e5eb064737957c05164ae84a22e4fc766f903a4` | 350 types / 1,954 methods |
| `DNA.Common.dll` | 610,304 | `642853463e0bbdf310ee88956af41cb5c2db5814a12255674ed61e383f8b3d59` | 528 types / 4,393 methods |

Both primary assemblies are IL-only AnyCPU assemblies targeting the CLR 2.0
metadata/runtime generation. The game references the XNA 4 Framework,
Graphics, Game, Net, GamerServices, Xact, and Avatar assemblies, as well as
`DNA.Common` and standard framework libraries. No game-side P/Invoke was found
in the initial metadata and IL survey.

The compatibility surface is nevertheless broad. The game uses XNA network
sessions and gamers, Guide UI and sign-in state, avatars and avatar animation,
XACT audio, storage/save abstractions, worker threads, and custom content
types. These APIs need native Switch implementations or compatibility shims;
loading the IL alone is not sufficient.

## Tool applicability

### XenonRecomp

[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) translates PowerPC code
from an Xbox 360 XEX into C++. There is no XEX or PowerPC game code in this
XBLIG package, so running it against `CastleMinerZ.exe` would be a category
error. It is not part of the CastleMiner Z game-code pipeline.

### XenosRecomp

[XenosRecomp](https://github.com/hedge-dev/XenosRecomp) translates Xbox 360
Xenos shader binaries to HLSL and can produce a SPIR-V cache. CastleMiner Z's
Xbox-targeted XNB content can contain compiled Effect payloads, so the tool may
be useful after XNB decompression and Effect extraction. It is not yet wired
in: the shader set, container differences, and suitability for the Switch
graphics backend still need to be measured. XenosRecomp explicitly expects
game-specific adaptation rather than working unmodified for every title.

### Ghidra

Native Ghidra analysis was attempted against the executable, but the native
loader correctly rejected it as a managed PE rather than native machine code.
Managed metadata inspection and IL tooling are appropriate for the game
assemblies. Ghidra remains useful if a future dependency or platform shim has
native code requiring analysis; no such game binary is present today.

## Reference-project lessons

[MarathonRecomp](https://github.com/sonicnext-dev/MarathonRecomp) and
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) establish useful
project patterns:

- keep original game files in ignored private inputs;
- validate user-provided, unmodified content;
- generate translated code and shader caches during a private build;
- separate generated game code from the native host/runtime; and
- implement explicit graphics, audio, input, storage, and service adapters.

Their CPU path cannot be copied directly because both projects begin with a
native PowerPC XEX. CastleMiner Z instead needs a managed-runtime path while
retaining the same legal separation between project code and user-owned game
data.

## Reproducing the public checks

After building the host tools:

```sh
cmztool verify /path/to/your/STFS/package
cmztool list /path/to/your/STFS/package
```

Individual extraction is available for private research, but extracted output
must remain outside version control. The repository check blocks common Xbox
asset formats, executable payloads, and key material from tracked files.
