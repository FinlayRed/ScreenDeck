# Screendeck M6 editor

Tauri 2 + Svelte 5 Windows editor for Screendeck projects. The Rust core owns
validation, portable archives, deterministic SDB3 bundle compilation and the
SDC3 v1 WinUSB transport; the Svelte layer is only the editor UI.

## Development

```powershell
npm install
npm run check
npm run test
cargo test --manifest-path src-tauri/Cargo.toml
npm run tauri dev
```

The browser-only Vite preview remains useful for layout work, but file dialogs
and device synchronization require the Tauri window.

The toolbar supports both directions: **Sync to device** uploads the current Screendeck project, while **From device** downloads the active device bundle and reconstructs the editable project hierarchy: profiles, pages, buttons, icons, and macros. Device bundles do not contain source-only names or original pre-conversion artwork, so imported items receive generated names and retain the device-ready artwork.

## Release builds

```powershell
# Native x64 executable and NSIS installer
npm run tauri build

# Windows ARM64 compile/build (install the Rust target once)
rustup target add aarch64-pc-windows-msvc
cargo check --manifest-path src-tauri/Cargo.toml --target aarch64-pc-windows-msvc
npm run tauri build -- --target aarch64-pc-windows-msvc --no-bundle
```

Artifacts are written below `src-tauri/target/<target>/release`. The x64 NSIS
installer is under `src-tauri/target/release/bundle/nsis`.

## Formats and transport

- `.sdeck` is a ZIP archive containing editable `project.json` plus original
  media under `assets/<asset-id>/original/` and a separately generated
  device-ready PNG under `assets/<asset-id>/device/`.
- `.sdb` begins with the 16-byte SDB3 v1 header from M3, followed by the
  validated `M5UI` v3 project payload. Schema-2 editor projects are migrated
  with safe M6 defaults when opened.
- Sync uses interface GUID `{F38C253C-7E95-4F15-A9FD-7BBC31E4F0C4}` and the
  SDC3 v1 HELLO/STATUS/BEGIN/CHUNK/COMMIT flow. Uploads resume from the durable
  device offset, and a lost commit acknowledgement is verified by generation.

## M6UI v3 payload

All offsets are little-endian byte offsets from the start of the M5UI payload.
Project source schema 3 is required. The 72-byte compiled header contains magic
`M5UI`, version 3, profile/page/asset/macro/radial counts, integrated device
settings, and offsets for all fixed records and asset blobs.

- Each button has a parallel `u16` macro index (`0xFFFF` for no macro).
- Each macro descriptor contains `u16 first_step` and `u16 step_count`.
- Each eight-byte step contains kind, HID usage page, `u16` usage and `u32`
  duration. Kinds are key-down, key-up, delay and consumer-control pulse.
- A button's first `u32` is its radial descriptor index. Descriptors contain
  4/6/8-item ranges; each item contains icon and macro indices.
- The settings word carries brightness, 180° orientation and screensaver
  enable state; the existing flags word remains the idle timeout.
- Keyboard and consumer usages are validated separately before compilation.
- The outer SDB3 payload CRC and deterministic SHA-256 fingerprint cover the
  entire compiled structure, including macro references and bytecode.

The status bar compares the current local fingerprint with a successful sync
from the current editor session. If the device protocol cannot prove remote
content identity, the UI says `Pending/unknown` rather than claiming a match.

The ignored Rust test `physical_winusb_round_trip_and_sync` is the physical
acceptance probe. Run it only with a Screendeck attached to the USB-OTG port:

```powershell
cargo test --manifest-path src-tauri/Cargo.toml physical_winusb_round_trip_and_sync -- --ignored --nocapture
```
