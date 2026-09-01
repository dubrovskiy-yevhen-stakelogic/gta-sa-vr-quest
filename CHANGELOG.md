# Changelog

## 0.1.3 alpha

### Added

- Added Vice City-style volumetric bullet tracers with a thin core and a
  multi-fin smoke volume.
- Added in-game tracer colour selection (`VICE CITY` or `GOLD`) and live smoke
  spread adjustment from 25% to 400%.
- Added the full retail mission list to the mission launcher instead of the old
  fixed 20-mission limit.
- Added the settings export helper for copying VR calibration files from Quest
  to a PC without including saves, logs, screenshots, game data, or the device
  serial number.

### Improved

- Shortened tracer length and lifetime so shots read as fast projectiles rather
  than persistent laser beams.
- Stabilized parachute visibility distance against altitude changes and moved
  the custom VR risers farther toward the canopy.
- Hid the redundant retail parachute strings and ripcord while the VR risers
  are active.
- Included the UltimateXR open-palm basketball hand meshes in player builds.
- Updated the Classic HUD health crop to the current tested `244 x 84` source
  region without changing the selected HUD preset.

### Fixed

- Fixed older tracers dimming newly spawned smoke by sharing one averaged alpha
  value.
- Fixed mission selection wrapping and page-specific mission counts.
- Fixed the public packager omitting the basketball palm assets even though the
  runtime referenced them.

