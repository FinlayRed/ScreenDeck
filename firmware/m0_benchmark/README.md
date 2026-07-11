# M0 board benchmark

This diagnostic firmware validates the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 before product firmware is built.

It intentionally uses the vendor BSP, runs with the display rotated to landscape, and emits machine-searchable serial markers:

- `M0_DISPLAY` — logical display dimensions and the 8×4 manual touch grid.
- `M0_TOUCH` — a tile coordinate whenever the grid is pressed.
- `M0_PSRAM` — 4 MiB PSRAM-to-PSRAM copy throughput.
- `M0_JPEG` — ESP32-P4 hardware JPEG decode throughput for an embedded 720p JPEG.
- `M0_SD` — 4 MiB microSD write/read throughput over the board's 4-bit SDMMC bus.
- `M0_HEAP` — PSRAM and internal-memory free/largest-block telemetry.

## Build and flash

Open an ESP-IDF 5.4 PowerShell environment, then run:

```powershell
Set-Location E:\Downloads\screendeck p4\firmware\m0_benchmark
idf.py set-target esp32p4
idf.py build
idf.py -p COM8 flash monitor
```

The first build downloads the version-pinned Waveshare board-support component and its declared dependencies. Do not format the microSD card if mounting fails; the benchmark is deliberately non-destructive and logs the failure.

## Manual check

The physical display should show a landscape 8×4 grid. Press each corner tile and confirm that the serial log reports the matching one-based logical column and row. The selected default is `ESP_LV_ADAPTER_ROTATE_90`; if the cable/right-side orientation is reversed on the actual mount, change only the rotation and touch transform after recording the M0 result.
