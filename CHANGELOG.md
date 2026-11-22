# CHANGELOG

## [0.5.1] - 2026-11-22
### Added
- **Logging Configuration**: Added `sdmc:/switch/castleminerz/config.ini` file to enable/disable host and performance logging. Default is enabled.
- **Logging Toggle**: `cmz_perf_initialize` now accepts `bool enabled` parameter. Early-return logic added when disabled.
- **Performance**: `cmz_perf_note` calls now skip when logging is disabled.
- **Performance Monitor**: `cmz_perf_attach_runtime` now skips when disabled.
- **Build Version**: Updated `APP_VERSION` to `0.5.1` in `scripts/build-game.ps1`.
- **Performance Documentation**: Updated `docs/performance.md` to reflect:
  - perf.log is on by default (toggleable via config.ini)
  - GC nursery reduced from 16MB to 4MB (SGen default)
  - Render scale 75% (960×540 internal) improves perf; game sees 1280×720
  - Audio 1024 quantum passed to `FAudioCreate`, not `FAudio_CreateMasteringVoice`
  - Logging toggle implementation documented
- **JOURNEY.md**: Added Logging Configuration section documenting file location, format, impact, usage, and related changes.

## [0.5.0] - 2026-11-21
### Changed
- Render scale 75% (960×540 internal) implemented via FNA3D, fixing menu crop issue.
- Audio 1024 quantum patch applied to FNA (0005-audio-1024-quantum.patch).
- FNA render scale patches applied and reverted to fix backbuffer/viewport scaling.
- Perf-test-5 built and hardware-tested with 75% render scale, 480-sample audio.
- Configuration file parsing logic implemented for logging toggle.

## [0.5.0] - 2026-10-28
### Changed
- Updated installation workflow with `scripts/prepare-game.ps1` and `scripts/verify-game-install.ps1`.
- Added private build outputs and verification steps.
- Added installation documentation and troubleshooting guides.
- Added configuration file support for logging toggle.
- Updated README and documentation to reflect private build boundaries and legal compliance.
