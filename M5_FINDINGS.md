# M5 completion record

## Implemented

- Clean M5UI schema v2 decoder; legacy v1 bundles are intentionally rejected.
- Static and animated asset records with strict range, FPS, and MJPEG frame-count validation.
- Visible-page MJPEG icon scheduler at 15 FPS with a touch-feedback quiet window.
- Eight-slot macro scheduler supporting keyboard down/up, delays, and consumer pulses.
- Composite keyboard, consumer-control, and mouse HID descriptors retained alongside WinUSB sync.
- Central HID release on USB detach and same-macro restart.
- Hardware-JPEG 60 FPS screensaver with bounded preload/SD fallback, deadline dropping, cold-start-safe scheduling, and wake-touch consumption.

## Verification

- ESP-IDF 5.5.4 clean build: passed (`screendeck_m5_animated_media.bin`, 0xfdce0 bytes; 32% app partition free).
- Rust editor/compiler tests: 5 passed, 2 hardware/FFmpeg tests ignored by design.
- Vitest: 2 passed.
- Svelte check: 0 errors, 0 warnings.
- Physical flash: bootloader, partition table, and 0xfdce0-byte application all hash-verified on ESP32-P4 revision 1.3.
- Post-boot USB enumeration: keyboard, consumer control, mouse, composite device, and WinUSB sync channel all present and healthy.
- Physical WinUSB schema-v2 bundle round trip: passed; committed generation was observed again after the firmware rebooted.
- Physical 60 FPS screensaver run: sustained 60.17-60.19 FPS with zero failed frames and zero deadline drops. Decode plus asynchronous panel submission averaged about 9.11 ms, with a 9.37 ms steady maximum and about 7.30 ms (44%) worst-case steady frame-budget headroom.
- The first-frame scheduler now starts its presentation timeline after the decoder's one-time initialization. Its measured cold-start work was 32.97 ms, but it no longer skips that frame or shifts subsequent deadlines.

## Screensaver refresh decision

The editor now exports MJPEG at 60 FPS with a 1,800-frame/30-second bound and
firmware schedules/decode-submits it at 60 FPS. Physical scanout remains on
Waveshare's validated 58 MHz/700 Mbps HX8394 profile (approximately 55 Hz).
Two true-60-Hz scanout configurations were rejected after visual testing:
reduced blanking porches produced a compressed, wobbling image, while a
63.264 MHz APLL clock with the original porch totals produced a blank panel.
Both were rolled back. A true 60 Hz panel mode therefore remains unsupported
without validated HX8394 timing and controller-register data from the panel
vendor; the 60 FPS media path is retained for future timing validation.

## Manual visual/performance verification still required

The automated hardware path and a sustained UART screensaver performance run
are complete. Visual animation smoothness, touch feel, macro effects in a
foreground application, and wake-latency observations still require a person
looking at and interacting with the display.
