# M0 board benchmark

This ESP-IDF 5.5.4 diagnostic validates the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 paths used by the product. It is bounded, non-destructive, and emits whitespace-separated `key=value` records suitable for the included log validator.

## Cases and timing boundaries

- `M0_DISPLAY_CONFIG`: MIPI-DSI/LVGL mode, rotation, and DPI framebuffer count.
- `M0_TOUCH`: logical LVGL press location. During the combined run, `M0_TOUCH_WORKLOAD` confirms callback service and gives time since workload start. It cannot measure controller-interrupt-to-feedback latency, so it explicitly reports `latency_us=unavailable`.
- `M0_PSRAM`: 24 PSRAM-to-PSRAM copies of 4 MiB.
- `M0_JPEG`: 30 hardware decodes of the embedded 1280×720 colour JPEG to RGB565. Timing includes `jpeg_decoder_process()` only; it excludes SD, LVGL, and display transfer.
- `M0_DISPLAY_CASE`: 30 full 720×1280 physical-panel RGB565 blits through the real LVGL adapter/MIPI-DSI panel path. `wait=true` includes adapter submit through the colour-transfer-done callback; it excludes decode, LVGL rendering, panel scanout, and photons.
- `M0_SD_READ`: two sequential reads of an 8 MiB temporary file using 128 KiB application and stdio buffers. Pass 1 is the playback-relevant first pass; pass 2 exposes cache effects. `M0_SD` includes durable write timing and confirms temporary-file removal.
- `M0_COMBINED`: 90 frames by default, each read sequentially from a temporary MJPEG stream, hardware decoded, installed as a 1280×720 LVGL RGB565 image, invalidated, and synchronously refreshed through the selected display mode. It reports achieved FPS, deadline misses (`dropped_frames`), failures, per-stage totals, maximum frame/lock time, touch events, and heap telemetry. A 30 FPS cadence is a target only; `achieved_fps` is the measured result.
- `M0_HEAP`: PSRAM/internal free and largest-block telemetry at each boundary.

The benchmark creates only `/sdcard/m0_sd_io.bin` and `/sdcard/m0_media_stream.mjpg`, removes both, and never formats the card.

## Build, select a mode, and run

Open the installed ESP-IDF 5.5.4 PowerShell environment:

```powershell
Set-Location 'E:\Downloads\screendeck p4\firmware\m0_benchmark'
idf.py menuconfig
idf.py -B '..\..\.m0_verify' build
idf.py -B '..\..\.m0_verify' -p COM8 flash monitor
```

Under **Screendeck M0 benchmark**, build and physically run each candidate separately:

1. `Triple partial` (production candidate/vendor default)
2. `Double direct`
3. `Double full`

Mode changes require display reinitialisation, so one firmware image intentionally measures one mode. Keep the same SD card and ambient conditions, capture from `M0_START` through `M0_COMPLETE`, then validate each log:

```powershell
python tools\validate_log.py run-triple-partial.log --json
python -m unittest discover -s tools -p 'test_*.py'
```

Do not infer tear-free behaviour from timing: visually inspect each mode for tearing while the combined image runs and tap several grid locations during `M0_COMBINED`.

## Candidate production buffer plan

This is the current allocation budget, not a claim that the new workload has run on hardware:

| Reservation | Memory | Bytes | Basis |
| --- | --- | ---: | --- |
| Three 720×1280 RGB565 DPI framebuffers | PSRAM | 5,529,600 | Static calculation; current BSP `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3` |
| One 1280×720 aligned RGB565 JPEG output | PSRAM/DMA-capable decoder memory | 1,843,200 | Static calculation and build-time dimensions |
| Compressed-frame/read ring | PSRAM | 262,144 | Proposed 2×128 KiB production ring |
| UI/assets/runtime reserve | PSRAM | 4,194,304 | Explicit product reservation |
| Fragmentation/safety reserve | PSRAM | 4,194,304 | Explicit hard reserve |
| SD DMA/stdio working buffers | Internal | 262,144 | Current benchmark worst case (2×128 KiB) |

The earlier physical run measured 28,017,592 bytes PSRAM free after display initialisation. Against that *measured baseline*, the proposed post-display additions (JPEG output + read ring + UI reserve + safety reserve = 10,493,952 bytes) leave 17,523,640 bytes (16.71 MiB) calculable PSRAM headroom. This must be recomputed from the new run's `after_combined` heap marker; it is not yet a measured combined-workload margin. The earlier 419,931-byte internal free result leaves only 157,787 bytes after the benchmark's 256 KiB SD buffers, so production should prefer one 128 KiB DMA staging buffer plus PSRAM buffering unless hardware results justify more internal allocation.

## Remaining physical gates

- Run all three display modes and compare `M0_DISPLAY_CASE` and `M0_COMBINED` output plus visible tearing.
- Confirm `completed_frames`, achieved FPS, deadline misses, stage timings, and post-workload heap on the real board/SD card.
- Tap during the combined interval and record `M0_TOUCH_WORKLOAD`; measure actual press-to-visible-feedback latency externally or add controller interrupt timestamping before treating latency as passed.
- The 30-minute soak is explicitly excluded/waived for this task and is not marked passed.
