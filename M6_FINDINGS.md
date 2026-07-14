# M6 completion record

## Implemented

- M6UI schema 3 with validated radial descriptors/items and integrated device settings.
- Per-button 4/6/8-way icon-plus-macro radial definitions in the editor.
- Live press/drag/release simulator using the same clockwise-up sectors,
  thresholds, edge clamp, centre cancel, commit radius and hysteresis as firmware.
- Firmware radial rendering and execution with an allocation-free, trig-free
  gesture hot path based on squared distances and Q10 maximum dot products.
- Brightness (0–100%), landscape/180° orientation, idle timeout and
  screensaver enable settings applied from the synced bundle.
- Existing M5 animation scheduling, macro execution, hardware-JPEG saver and
  wake-touch consumption integrated into the M6 runtime.

## Performance choices

- No `atan2`, square root, floating-point work, heap work, or division occurs
  during touch movement.
- The radial overlay is bounded to one object tree per gesture and pointer
  updates restyle only the old and new selection.
- Sticky outer selection prevents boundary chatter without delaying fast flicks.
- Existing adaptive animation throttling and touch quiet-window remain active.

## Verification

- Vitest geometry/model tests: passed.
- Svelte check: 0 errors and 0 warnings.
- Rust compiler/archive tests: passed, including schema-3 radial/settings round trip.
- PowerShell gesture matrix: covers corners, edges, jitter, fast flick,
  hysteresis and source-verified wake suppression.
- ESP-IDF 5.5.4 full build: passed (`screendeck_m6_radial_ux.bin`,
  `0xff7e0` bytes; 32% of the app partition remains free).
  SHA-256: `88C344B39E69FD1DDE78527DF0B08EE7A1A9C94F73253DB46F8A331E9B147C35`.

## Physical follow-up

The image was flashed to the attached ESP32-P4 revision 1.3 on COM8. Esptool
hash-verified the bootloader, partition table, and 1,046,496-byte application;
the board hard-reset and its boot log identified project
`screendeck_m6_radial_ux`, initialized 32 MiB PSRAM, mounted generation 38 from
microSD, enumerated USB, and registered the display and GT911 touch device.
Final tactile threshold tuning, brightness perception and 180° touch alignment
still benefit from a person using the display.
