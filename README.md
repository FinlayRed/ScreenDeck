# ScreenDeck

ScreenDeck turns a Waveshare ESP32-P4-WIFI6-Touch-LCD-5 into a 32-key USB macro pad. The Windows editor configures pages, icons, macros, radial menus, and screensavers; the device stores the result on microSD and runs on its own.

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

## License

ScreenDeck is licensed under the [GNU General Public License, version 3 or later](LICENSE). Third-party dependencies keep their own licenses. The Waveshare code under `vendor/` remains Apache-2.0 licensed.
