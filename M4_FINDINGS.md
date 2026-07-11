# M4 findings — lightweight Windows macro editor

Status: **implemented; x64 physical gate passed, ARM64 runtime gate pending** (2026-07-11)

## Design delivered

| Area | M4 implementation |
| --- | --- |
| Desktop shell | Tauri 2 with a Svelte 5 UI and a Rust model/compiler independent of the window layer |
| Page editor | Centred 8×4 square-key preview with black letterboxing, profile/page tree, key selection and per-key accent |
| Icons | PNG/JPEG/WebP import, drop-to-key assignment and original media retained in the project archive |
| Macros | Ordered key-down, key-up, delay and consumer-key steps with HID key picker and strict bounds |
| Project files | Portable `.sdeck` ZIP containing pretty editable JSON plus original asset files |
| Bundle compiler | Deterministic SDB3 v1 bundle, physical-M3-compatible CRC, validation summary and standalone `.sdb` backup |
| Device | Native architecture-neutral WinUSB FFI, capability/status display, resume-aware upload, atomic commit and actionable errors |
| Packaging | Branded x64 application and per-user NSIS installer; the same source compile-checks for Windows ARM64 |

## Verification

- [x] `svelte-check`: 0 errors and 0 warnings.
- [x] Vitest: 2 editor-model tests passed.
- [x] Rust: CRC compatibility, SDB3 header and editable archive round-trip tests passed.
- [x] Native x64 release executable built.
- [x] x64 NSIS installer built as
      `desktop/m4_editor/src-tauri/target/release/bundle/nsis/Screendeck_0.4.0_x64-setup.exe`.
- [x] `cargo check --target aarch64-pc-windows-msvc` passed, including the raw WinUSB FFI.
- [x] M4's Rust WinUSB client synchronized the sample project to the physical
      P4 and verified atomic activation at generation 3.
- [ ] Run the ARM64 binary on native Windows ARM64 and repeat the same physical
      archive-open/save/sync acceptance test. No Windows ARM64 host is attached
      to this workspace, so this is the only remaining plan gate.

## M3 compatibility correction found by M4

Repeated chunk uploads exposed that M3 could silently drop a required response
when TinyUSB's IN buffer was momentarily busy. `m3_send_response` now waits up
to 250 ms and emits an explicit timeout/short-write diagnostic instead. The
corrected 807,712-byte firmware was rebuilt, flashed and hash-verified on COM8
with 23% factory-partition headroom before the successful M4 sync.

The M4 transport also retains protocol-level recovery: it can re-query `BEGIN`
for the durable offset after a lost chunk acknowledgement and verify generation
after a lost commit acknowledgement.
