# Screendeck M4 editor

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
  media under `assets/<asset-id>/`.
- `.sdb` begins with the 16-byte SDB3 v1 header from M3, followed by the
  validated project payload.
- Sync uses interface GUID `{F38C253C-7E95-4F15-A9FD-7BBC31E4F0C4}` and the
  SDC3 v1 HELLO/STATUS/BEGIN/CHUNK/COMMIT flow. Uploads resume from the durable
  device offset, and a lost commit acknowledgement is verified by generation.

The ignored Rust test `physical_winusb_round_trip_and_sync` is the physical
acceptance probe. Run it only with a Screendeck attached to the USB-OTG port:

```powershell
cargo test --manifest-path src-tauri/Cargo.toml physical_winusb_round_trip_and_sync -- --ignored --nocapture
```
