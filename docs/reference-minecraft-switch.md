# Reference notes: wick800/minecraft-switch

Reviewed for hardware-only boot failures. Source: `wick800/minecraft-switch`
(a fork of `etonedemid/minecraft-switch`), README, `build-a64.sh`, `build-nro.sh`,
`run-host.sh`, `prof.txt`, the commit list through Oct 28, 2026, and the
reachability commits `1dbfc75` and `b887051`. Nothing was copied into this
repository. `LEIA-ME.txt` returned 404 and was not reviewed.

## What it is

Minecraft Java 1.12.2 with OptiFine, compiled ahead of time by GraalVM
`native-image` into an aarch64 NRO. It runs on a Horizon "lxnx" personality, a
musl-nx libc, and a GLES/EGL shim. It is not Mono, and it has no ILLink step.
Its problem is the same as ours, though: AOT code only contains what the build
could reach, and the emulator run only exercises part of that.

## Lessons that apply here

### 1. Reachability metadata must cover everything, not what one run touched

- `1dbfc75`: netty socket channels were registered "only the local channels ...
  from singleplayer runs", so multiplayer failed until they were added.
- `b887051`: authlib's Gson-built session requests were added for joining online
  servers. The generator comment says it plainly: "a tracing run only meets the
  ones it happened to spawn". Their fix generates metadata for every class in
  the jars, not only what a trace touched.

CastleMiner Z analogue: `build-game.ps1` passes `CastleMinerZ.exe` to ILLink
with a plain `-a`. In ILLink that roots only the entry point of an application
assembly (`docs/tools/illink/illink-options.md`), so unreferenced game members are
trimmed. Reflection, serialization and content-reader paths are exactly what
Eden seldom exercises. The build now passes `all` for that assembly. This is
unverified until it is built. See the JOURNEY entry.

### 2. Resources need the same completeness

Their generator also embeds "every asset and root resource", because "a tracing
run only embeds what it happened to load (unseen mobs came out purple/black)".

CMZ analogue: content is read from the STFS package at runtime, so it is not
embedded. The risk is the same, though. Anything Eden never loads is untested on
hardware. Examples are the 11 WMA streaming entries and sound paths that depend on
audio callbacks.

### 3. Horizon limits they designed around (`build-a64.sh`, `build-nro.sh`)

- `-H:-MremapImageHeap`: no `mremap` on the console, so the heap is not remapped.
- `-H:-SpawnIsolates`: no isolates; the image heap is used in place.
- `-Wl,-z,notext`: text relocations allowed in the NRO link.
- `--wrap=fatalThrow,--wrap=diagAbortWithResult`: libnx fatal paths are wrapped so
  the message can be logged before the abort.

CMZ checklist: confirm mono-nx, SDL and FAudio never call `mremap` on this target.
Consider wrapping libnx fatal paths the same way so the last message reaches
`host.log` before the abort. (`runtime-probe` already uses unbuffered logs.)

### 4. Thread stacks

`-Xss8m` is in the commit "OpenAL ..., -Xss8m, sound and worlds working on the
console". World loading failed on console before the stacks were enlarged.

CMZ analogue: the faulting thread in the 2026 crash report has a 132 KB stack,
while most other threads have 1 to 2 MB. At the fault it had used about 4.6 KB,
so this crash is not a stack overflow. Managed worker threads should still get an
explicit stack size if the game's worker code recurses deeply.

### 5. Audio

They use openal-soft with the OSS backend, and sound works on the console. The
CMZ path is FAudio over SDL. Eden's audren never consumes SDL buffers, so the
audio thread and its callbacks have never run in an emulator. Test audio on
hardware early, and treat any audio callback path as untested until then.

### 6. Symbolized profiles

`tools/prof.py` symbolizes their runtime profile. The CMZ equivalent for crash
reports is `scripts/symbolize-crash.ps1`, which resolves Atmosphère
`castleminerz + 0x...` frames against the unstripped ELF.

## Not applicable

GraalVM reflection configuration format, LWJGL and Pojav layers, OpenAL itself,
Minecraft-specific tuning (render distance, GC flags) and the musl and lxnx
runtime.
