# M5 — animated icons and MJPEG screensaver

M5 keeps the M3 SDC3 WinUSB/keyboard transport and microSD ownership, then
adds a device-local media runtime:

- The active `SDB3` project bundle is loaded from microSD and validated before
  use. Its schema-v2 `M5UI` payload supplies profiles, pages, macros, button
  actions, accent colours, static posters, and bounded MJPEG icon streams.
- Synced icon assets are decoded from PSRAM and rendered with the configured
  contain/cover fit. Page-next, page-previous, and profile-next actions rebuild
  the visible page without rebooting.
- Up to 32 visible-page tiles animate at a bounded 15 FPS scheduler cadence;
  animation work yields briefly after touch activity to preserve feedback.
- Button macros execute on a separate task with keyboard and consumer-control
  HID reports. Disconnect and restart paths centrally release HID state.
- `/sdcard/screendeck/screensaver.mjpg` is indexed as individual JPEG frames.
- 1280×720 RGB565 frames use the ESP32-P4 hardware JPEG decoder.
- Short media is preloaded into PSRAM when the runtime budget allows it;
  otherwise frames are read through a bounded SD buffer.
- The active UI, saver playback, and wake path are explicit states. Any touch
  wakes the saver and reconstructs the LVGL page before input is accepted.
- A successful project or screensaver commit restarts after its USB response is
  acknowledged, so the newly committed content is indexed automatically.

Useful serial markers:

```text
M5_START
M5_MEDIA source=none|psram|sd frames=N fps=60
M5_STATE from=active to=playing|from=playing to=active
M5_FRAME index=N dropped=0|1
M5_COMPLETE animation_fps=15 saver_ready=1
```

The screensaver file is intentionally a raw MJPEG stream (concatenated SOI/EOI
JPEG frames), so the host can produce it deterministically without a container.
The desktop editor retains the source, creates a bounded poster, and converts
GIF, animated WebP, and video icons into deterministic native-size 149x149
15 FPS MJPEG with 12 px rounded corners baked against the tile background.
This keeps animated images on the accelerated zero-clip-radius render path.
The runtime displays up to eight animations at 15 FPS, nine to
sixteen at 10 FPS, and larger animated pages at 7 FPS while skipping source
frames to preserve playback speed.

## Deterministic conversion

Use `tools/m5_convert_media.ps1`. It selects the matching x64/ARM64 sidecar
from `desktop/m4_editor/src-tauri/resources/ffmpeg/<arch>/ffmpeg.exe`, falling
back to an installed `ffmpeg` during development:

```powershell
.\tools\m5_convert_media.ps1 -Kind Screensaver -InputPath .\clip.mp4 `
  -OutputPath .\screensaver.mjpg
.\tools\m5_convert_media.ps1 -Kind Icon -InputPath .\icon.gif `
  -OutputPath '.\icon-frames\frame-%04d.jpg'
```

In the desktop editor, connect the device, then select the monitor-with-arrow
button in the top toolbar and choose the resulting `.mjpg` file. The transfer
is checksum-verified before it replaces the screensaver on the device.
