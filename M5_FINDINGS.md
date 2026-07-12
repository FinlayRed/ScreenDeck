# M5 completion record

## Implemented

- Clean M5UI schema v2 decoder; legacy v1 bundles are intentionally rejected.
- Static and animated asset records with strict range, FPS, and MJPEG frame-count validation.
- Visible-page MJPEG icon scheduler at 15 FPS with a touch-feedback quiet window.
- Eight-slot macro scheduler supporting keyboard down/up, delays, and consumer pulses.
- Composite keyboard, consumer-control, and mouse HID descriptors retained alongside WinUSB sync.
- Central HID release on USB detach and same-macro restart.
- Existing hardware-JPEG 30 FPS screensaver, bounded preload/SD fallback, deadline dropping, and wake-touch consumption retained.

## Verification

- ESP-IDF 5.5.4 clean build: passed (`screendeck_m5_animated_media.bin`, 0xfdce0 bytes; 32% app partition free).
- Rust editor/compiler tests: 5 passed, 2 hardware/FFmpeg tests ignored by design.
- Vitest: 2 passed.
- Svelte check: 0 errors, 0 warnings.
- Physical flash: bootloader, partition table, and 0xfdce0-byte application all hash-verified on ESP32-P4 revision 1.3.
- Post-boot USB enumeration: keyboard, consumer control, mouse, composite device, and WinUSB sync channel all present and healthy.
- Physical WinUSB schema-v2 bundle round trip: passed; committed generation was observed again after the firmware rebooted.

## Manual visual/performance verification still required

The automated hardware path is complete. Visual animation smoothness, touch
feel, macro effects in a foreground application, and long-running screensaver
FPS/drop and wake-latency observations still require a person looking at and
interacting with the display.
