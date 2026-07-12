# M4 findings — lightweight Windows macro editor

Status: **M4 implementation corrected and final x64 application built; installer repack, device runtime and native ARM64 execution deferred** (2026-07-12)

## Design delivered

| Area | M4 implementation |
| --- | --- |
| Desktop shell | Tauri 2 with a Svelte 5 UI and a Rust model/compiler independent of the window layer |
| Page editor | Centred 8×4 square-key preview with black letterboxing, profile/page tree, key selection and per-key accent |
| Icons | PNG/JPEG/WebP import, drop-to-key assignment, original media and device-ready PNG retained separately |
| Macros | Ordered key-down, key-up, delay and consumer-key steps with HID key picker and strict bounds |
| Project files | Portable `.sdeck` ZIP containing pretty editable JSON plus original asset files |
| Bundle compiler | Deterministic SDB3 envelope with M5UI v2 profiles, pages, assets, button macro references and macro bytecode |
| Device | Native architecture-neutral WinUSB FFI, capability/status display, resume-aware upload, atomic commit and actionable errors |
| Packaging | Branded x64 application and per-user NSIS installer; the same source compile-checks for Windows ARM64 |

## Verification

- [x] `svelte-check`: 0 errors and 0 warnings.
- [x] Vitest: 2 editor-model tests passed.
- [x] Rust: 5 tests passed, including byte-level macro/reference serialization
      and separate original/derivative archive round-trip coverage.
- [x] Native x64 release executable built.
- [x] Final schema-v2 native x64 executable built at
      `desktop/m4_editor/src-tauri/target/release/screendeck-editor.exe`
      (9,867,264 bytes; SHA-256 `3ED4303C7DFA2F3762E44B255236AE7A11C63C29A1A5AF7A49332B61EF4DED13`).
- [ ] Repackage the final NSIS installer. The NSIS bootstrap download is blocked
      in the restricted workspace and the approval-credit request was rejected;
      the existing setup executable predates the final schema-v2 rebuild and
      must not be treated as the deliverable.
- [x] `cargo check --target aarch64-pc-windows-msvc` passed, including the raw WinUSB FFI.
- [x] Local project/device comparison now reports a confirmed match only after
      a successful sync in the current session; otherwise it honestly reports
      remote content identity as pending/unknown.
- [ ] Update the integrated firmware decoder/runtime to M5UI v2 and repeat the
      physical project sync. Per user direction, M2/M3 runtime testing is
      deferred; the currently flashed M1 image has no SDC3 sync interface.
- [ ] Run the ARM64 binary on native Windows ARM64 and repeat the same physical
      archive-open/save/sync acceptance test. No Windows ARM64 host is attached
      to this workspace, so this is the only remaining plan gate.

## 2026-07-12 correctness remediation

- The earlier compiler validated macro definitions but silently omitted them
  from the device bundle. M5UI v2 now serializes every button macro reference,
  macro descriptor and ordered HID step.
- Unsupported or wrong-page HID usages now fail validation. The editor shows
  keyboard keys for keyboard steps and consumer keys for consumer steps.
- Imports retain the exact original file separately from the bounded PNG used
  by the device; archive tests prove the byte streams remain distinct.
- The prior “diff summary” could only show generation and bundle size. The UI
  now exposes the deterministic local fingerprint and never infers remote
  equality from generation alone.
- Backward compatibility with the incomplete M5UI v1 payload is intentionally
  not provided, by user decision.

## M3 compatibility correction found by M4

Repeated chunk uploads exposed that M3 could silently drop a required response
when TinyUSB's IN buffer was momentarily busy. `m3_send_response` now waits up
to 250 ms and emits an explicit timeout/short-write diagnostic instead. The
corrected 807,712-byte firmware was rebuilt, flashed and hash-verified on COM8
with 23% factory-partition headroom before the successful M4 sync.

The M4 transport also retains protocol-level recovery: it can re-query `BEGIN`
for the durable offset after a lost chunk acknowledgement and verify generation
after a lost commit acknowledgement.
