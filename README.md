# ScreenDeck

ScreenDeck turns a Waveshare ESP32-P4-WIFI6-Touch-LCD-5 into a 32-key USB macro pad. The Windows editor configures pages, icons, macros, radial menus, and screensavers; the device stores the result on microSD and runs on its own.

![ScreenDeck editor](https://img.reddi.ng/u/GyUM49.png)

## Build

Clone the submodule and run the root build script from PowerShell:

```powershell
git clone --recurse-submodules https://github.com/FinlayRed/screendeckp4.git
cd screendeckp4
./build.ps1 -Target test
./build.ps1 -Target editor
```

The editor requires Node.js 20+, Rust, and the [Tauri 2 Windows prerequisites](https://v2.tauri.app/start/prerequisites/). Its installer is written to `editor/src-tauri/target/release/bundle/nsis`.

Firmware builds require an activated [ESP-IDF 5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/get-started/index.html) shell:

```powershell
./build.ps1 -Target firmware
./build.ps1 -Target firmware -Flash -Port COM8
```

`./build.ps1` builds both the editor and firmware. Pass `-NoRestore` to reuse installed Node dependencies.

## Layout

- `editor/` — Tauri, Svelte, and Rust desktop app
- `firmware/` — current ESP-IDF firmware
- `scripts/` — media conversion utilities
- `tests/` — host-side protocol and gesture checks
- `vendor/` — pinned Waveshare board support submodule

The UART USB port handles flashing and logs. The separate USB-OTG port carries HID output and editor sync.

See [editor/README.md](editor/README.md) and [firmware/README.md](firmware/README.md) for details.

## FFmpeg runtime

Animated icons and screensaver conversion require FFmpeg. The installer does not currently bundle it. The editor looks for it, in order, at:

1. `ffmpeg.exe` beside the installed Screendeck executable
2. `ffmpeg` on `PATH`

If FFmpeg is missing, the editor disables animated-icon import and screensaver upload and shows these installation options. Static icons and every non-conversion feature remain available.

### Future bundled distribution

Bundling FFmpeg requires adding a pinned Windows binary to the Tauri bundle configuration and installing it beside the application executable, where the existing runtime lookup will find it. Before doing that:

- Record the exact source, version, architecture, and SHA-256 checksum.
- Include the applicable FFmpeg GPL or LGPL license files.
- Verify the checksum during packaging.
- Test the NSIS installer on a machine without system FFmpeg.

Do not add an unpinned download or silently fetch an executable at runtime.

## License

ScreenDeck is licensed under the [GNU General Public License, version 3 or later](LICENSE). Third-party dependencies keep their own licenses. The Waveshare code under `vendor/` remains Apache-2.0 licensed.
