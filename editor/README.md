# ScreenDeck editor

The Windows editor uses Svelte 5 for the interface and a Tauri 2/Rust core for project archives, bundle compilation, and WinUSB sync.

Run it from the repository root:

```powershell
./build.ps1 -Target test
cd editor
npm run tauri dev
```

Build the NSIS installer with `./build.ps1 -Target editor`. A browser-only preview is available through `npm run dev`, but device sync and file dialogs require Tauri.

Projects use `.sdeck` ZIP archives. Compiled device bundles use the versioned `.sdb` format described in [the protocol documentation](../docs/protocol.md).

The ignored Rust test `physical_winusb_round_trip_and_sync` requires a connected ScreenDeck:

```powershell
cargo test --manifest-path src-tauri/Cargo.toml physical_winusb_round_trip_and_sync -- --ignored --nocapture
```
