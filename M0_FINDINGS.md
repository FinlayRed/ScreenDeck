# M0 — Board bring-up and performance truth

Status: **in progress**

This log records observed findings only. Targets from the project plan are not substituted for measured results.

## Environment discovered

- Board transport: Waveshare USB-to-UART bridge identified as **CH343, COM8**.
- Framework installed: **ESP-IDF v5.4.1** at `C:\Users\Finlay\esp\v5.4.1\esp-idf`.
- Official reference imported: `vendor/waveshare-reference` from Waveshare's `ESP32-P4-WIFI6-Touch-LCD-5` sample repository (shallow clone on 2026-07-11).
- Official BSP configuration found: HX8394 panel over 2-lane MIPI-DSI; GT911 capacitive touch on I²C; 4-bit SDMMC at high-speed mode; 32 MB flash/PSRAM board configuration.
- Benchmark project: `firmware/m0_benchmark`.

## Build findings

- **2026-07-11:** The initial managed dependency `waveshare/esp32_p4_wifi6_touch_lcd_5@1.0.1` could not be resolved by the public ESP Component Registry, despite the vendor sample declaring it. M0 now uses the exact board BSP and HX8394 component checked into the official sample repository under `vendor/waveshare-reference`; this avoids a transient registry dependency and keeps the pin configuration inspectable in the workspace.
- **2026-07-11:** The checked-in BSP resolves `espressif/esp_lvgl_adapter@^0.1`, whose currently available releases require ESP-IDF 5.5+. The locally installed framework is v5.4.1, so M0 must be built with a pinned 5.5.x ESP-IDF checkout. This is a dependency compatibility issue, not a board failure.
- **2026-07-11:** Pinned ESP-IDF v5.5.4 and its ESP32-P4 toolchain were installed under `C:\Users\Finlay\esp\v5.5.4`. The first compile reached the M0 source and exposed a missing explicit `esp_timer` component requirement; it was corrected before flashing.

## Pre-flash board baseline

The existing firmware was read from `COM8` before M0 flashing. It reported:

- Project: `esp32p4_mjpeg_benchmark`, built with ESP-IDF **v5.5.2**.
- Chip revision: **v1.3**; CPU frequency: **360 MHz**.
- PSRAM: **32 MB**, 200 MHz, X16 mode; boot-time PSRAM memory test passed.
- microSD: `SL64G`, **4-bit**, **40 MHz**, 60,906 MB.
- Existing asset: `/sdcard/animation.p4mj`, 2,903,036 bytes, 43 frames, 720×752; reported SD load rate **1.82 MiB/s**.
- Existing firmware successfully initialized the MIPI-DSI panel and HX8394 driver (v1.0.3).

These are baseline observations from a different test firmware, not M0 results.

## Benchmark protocol

1. Flash M0 over `COM8`.
2. Verify the screen renders an 8×4 landscape grid with the USB connectors on the right.
3. Press the four corner tiles and record `M0_TOUCH` mapping.
4. Capture the one-shot `M0_PSRAM`, `M0_JPEG`, `M0_SD`, and `M0_HEAP` markers.
5. Leave the board running for 30 minutes and capture any warnings, resets, allocation changes, or display artefacts.

## Results

| Area | Measured result | Status |
| --- | --- | --- |
| Flash/build | ESP-IDF v5.5.4 image flashed and hash-verified over COM8 | Pass |
| Landscape display | MIPI-DSI/HX8394 initialized; LVGL reports 1280×720 with 90° rotation; USB ports confirmed on the right | Pass |
| Touch mapping | GT911 found at `0x5D`; configuration version 70; user visually confirmed the four corner taps | Pass |
| PSRAM copy throughput | 39.89 MiB/s for 4 MiB PSRAM→PSRAM copy, 24 iterations | Measured |
| Hardware JPEG decode | 1920×1080 colour JPEG → RGB565: **48.89 FPS** over 30 frames; 4,177,920-byte aligned output | Measured (decode only) |
| SDMMC write/read | 4.51–6.68 MiB/s write, 1.38–1.59 MiB/s read; 4 MiB FATFS test file | Measured |
| Memory headroom | After display: 26.72 MiB PSRAM free, 26.50 MiB largest; 410 KiB internal free, 304 KiB largest | Measured |
| 30-minute stability | Pending | Not run |

## Current constraints / follow-up

- M0 is diagnostic firmware only; it contains no HID, profile, or macro implementation.
- The microSD benchmark creates and removes only `/sdcard/m0_sd_io.bin`; it never formats the card.
- The chosen landscape transform is `ESP_LV_ADAPTER_ROTATE_90`. Confirm the physical right-side USB orientation before M1, then record any required touch mirror/swap correction here.
- The first on-board M0 run confirmed the display and touch transport. It also found a vendor BSP API bug: `bsp_display_lock()` is declared as `bool` but returns `esp_err_t` directly, so successful `ESP_OK` is observed as `false`. M0 follows the vendor demos and ignores that wrapper return; product firmware will call the underlying `esp_lv_adapter_lock()` API directly for type-safe status handling.
- The initial JPEG benchmark embedded `esp720.jpg` from the ESP-IDF sample. The decoder correctly identified it as grayscale and rejected the requested RGB565 output. M0 now embeds the sample's colour `esp1080.jpg`, so the result will measure the RGB565 hardware path relevant to the screensaver.
- The colour JPEG exposed a second hardware-specific requirement: P4 decoder output buffers are dimensioned to 16-pixel blocks. For the 1920×1080 test image, that means 1920×1088×2 bytes. The allocation has been corrected before the final automatic run.

## Final automatic M0 run

- Firmware: `screendeck_m0_benchmark`, ESP-IDF **v5.5.4**; image flash hash verified by esptool.
- Display: HX8394 MIPI-DSI and GT911 touch initialized successfully. LVGL reports a **1280×720** logical surface with the configured 90° rotation.
- PSRAM copy: **39.91 MiB/s** for 24 copies of a 4 MiB PSRAM buffer.
- JPEG: **48.89 FPS** for 30 hardware decodes of the embedded 1920×1080 colour JPEG to RGB565. This is a decoder-only result; MIPI-DSI handoff is deliberately not included yet.
- SDMMC: **6.36 MiB/s write**, **1.58 MiB/s read** for the final 4 MiB FATFS test. The temporary file was removed after each run.
- Post-test heap: **28,017,592 B (26.72 MiB)** PSRAM free with a **26.50 MiB** largest block; **419,931 B (410 KiB)** internal free with a **304 KiB** largest block.

## Warnings requiring follow-up

- The vendor BSP logs that its GPIO 26 LEDC backlight configuration conflicts with another board function. The panel initializes, but M1 must replace or verify the BSP backlight path before relying on brightness control.
- The GT911 I²C driver emits a generic pull-up resistance warning even though the controller initializes and reports its ID/configuration. Treat this as a board-BSP warning until a long touch test proves otherwise.
- The M0 SDMMC read result is much lower than bus peak rate and lower than the previous benchmark's 1.82 MiB/s asset load. M1/M5 must profile buffered sequential reads and decoder/display overlap rather than extrapolating from bus clock alone.

## Physical verification

- **2026-07-11:** User confirmed that the landscape grid is correct with USB ports on the right and that all four corner taps worked as expected. This completes the manual display/orientation/touch portion of M0.

## Product-layout decision

- The production macro UI uses **square 1:1 icons**. On the 1280×720 landscape panel, the fixed 8×4 grid is vertically centred; the unused space is rendered as intentional black bars above and below the grid. The editor preview and firmware layout engine must share this same geometry.
