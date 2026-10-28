> **Note:** this documents a decision and its evidence, not a shipped feature.

# Graphics backend decision

## Question

FNA renders through FNA3D. On Nintendo Switch the two candidate backends are:

1. **deko3d** — Nintendo's own low-level Switch GPU API (`libnx` ships it,
   devkitPro provides it); thin, explicit, fast, no driver overhead.
2. **Mesa via EGL/GLES** — `switch-mesa` in devkitPro portlibs, driven by
   SDL2's Switch video backend through EGL.

## What the pinned sources actually support

Checked against the pinned FNA3D commit
(`fd1b24671fb0f5d1b544cd7843d3ad9e80d92219`) and the installed devkitPro
packages:

| Item | deko3d | Mesa/EGL/GLES |
| --- | --- | --- |
| FNA3D driver in pinned tree | **none** | `FNA3D_Driver_OpenGL.c` |
| Works with stock FNA managed code | no | yes (`FNA_PLATFORM_BACKEND=SDL2`) |
| Context creation | manual | `SDL_GL_CreateContext` |
| Shader path | would need a Mojoshader→DKSH backend | existing Mojoshader GLSL/ES output |
| Present in portlibs | yes (`libnx`) | `libEGL.a`, `libglapi.a`, `libglad.a`, `libdrm_nouveau.a` (Mesa 20.1) |
| SDL video driver | n/a | `SDL_VIDEO_DRIVER_SWITCH`, `SDL_VIDEO_OPENGL_EGL=1`, `SDL_VIDEO_OPENGL_ES2=1` (SDL 2.28.5) |

FNA3D has exactly one driver that can work here. There is no deko3d driver in
FNA3D, in FNA, or in any public FNA fork, and deko3d is not an OpenGL/Vulkan
implementation — a DllImport-compatible `libGL` cannot be layered on it
without writing a full GL-on-deko3d translator (which is a multi-month project
of its own and would still have to translate GLSL to DKSH).

So "use deko3d" currently means **writing a new FNA3D driver**: implement the
`FNA3D_Driver` vtable (prepare window attributes, create/destroy device,
pipelines, buffers, textures, render targets, samplers, blend/depth/rasterizer
state, swap buffers, readback), plus a Mojoshader SPIR-V/DKSH path, plus
command-buffer and descriptor-heap management. That is worthwhile as a
performance project later, but it cannot be the first playable milestone,
because it is downstream of *everything else* still missing.

## Decision

**First playable target: SDL2 → EGL → switch-mesa GLES 3.0 → FNA3D OpenGL
driver.** Concretely:

```
FNA (managed, SDL2 backend)
  -> FNA3D openGL driver, FNA3D_OPENGL_FORCE_ES3=1
  -> SDL_GL_CreateContext (ES 3.0 profile)
  -> SDL2 Switch video driver (SDL_VIDEO_OPENGL_EGL)
  -> libEGL.a / switch-mesa (tegra Gallium)
  -> libnx nv services
```

`FNA3D_OPENGL_FORCE_ES3` matters because FNA3D only auto-selects ES on
iOS/tvOS/Android/Emscripten/Stadia: it compares `SDL_GetPlatform()` against a
hard-coded list. SDL on Switch reports `Switch`, so the hint must be set
explicitly before `FNA3D_PrepareWindowAttributes()`.

### Consequences and known risks

- switch-mesa is a 20.1-era tegra Gallium driver compiled for Cortex-A57; it is
  correct enough to run XNA-era workloads but is the main frame-rate risk. It
  reports a GLES 3.x profile, which is what FNA3D's GLES path requires
  ("OpenGL ES 3.0 support is required!").
- The driver is CPU-heavy for state changes. Mitigations, in order: run at
  1280x720 and let `switch-mesa` scale, keep the game's own render-target
  usage, batch by `SpriteBatch`, and only then consider a deko3d driver.
- Mesa needs `libdrm_nouveau.a`, `libglapi.a`, `libglad.a`; all are present in
  portlibs, so nothing is vendored into this repository.
- deko3d stays a **post-playable optimization**, tracked in the roadmap. It
  does not block anything, and no project code depends on the choice yet
  beyond the native host and the SDL2 build flags.

## How this is verified

`runtime-probe/graphics` is a native-only NRO (no Mono, no managed code) that:

1. sets `FNA3D_FORCE_DRIVER=OpenGL` and `FNA3D_OPENGL_FORCE_ES3=1`;
2. prints the SDL version, `SDL_GetPlatform()`, GL vendor/renderer/version;
3. creates an ES 3.0 window, calls `FNA3D_PrepareWindowAttributes()` and
   `FNA3D_CreateDevice()`;
4. clears the backbuffer with a moving colour and presents it until `+`.

If that NRO presents correctly, the entire SDL2/Mesa/FNA3D path is proven on
hardware and the remaining graphics work is game-side (Effects, render states,
textures), not platform-side.

The game-side path is now verified in Eden: the title screen, terrain, sky,
water, HUD, hotbar icons, first-person held item and pause menu all render with
the project-authored effects and Xbox cubemap/model byte-order fixes. Physical
hardware confirmation of the probe is still pending.
