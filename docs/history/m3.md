# M3 findings — atomic USB sync and microSD ownership

Status: **implemented and verified on the physical P4** (2026-07-11)

## Design delivered

| Area | M3 implementation |
| --- | --- |
| USB | Composite standard keyboard + vendor bulk endpoint; no mass-storage interface |
| Protocol | Versioned SDC3 frames: hello, begin, chunk, commit, abort, status and diagnostics |
| Integrity | Per-frame payload CRC32, per-chunk CRC32, full-bundle CRC32 and strict size bounds |
| Resume | Persistent upload state plus staged-file size validation; `BEGIN` returns the safe offset |
| Atomic activation | Stage → immutable numbered bundle → immutable numbered pointer; boot chooses the highest valid pointer |
| Card ownership | Firmware mounts and owns the mandatory microSD; Windows never receives block-level card access |
| Recovery | Internal-flash recovery screen; HID output remains disabled until a complete runtime bundle is integrated |

## Hardware gates to complete

- [x] M3 builds and flashes to the physical P4; the latest built image is
      807,376 bytes with 23% factory-partition headroom, and flash hashing
      passed on COM8.
- [x] Validate SD directory creation and recovery state. FATFS long filenames
      were enabled after the initial 8.3-mode diagnostic; the board now logs
      `M3_SD result=ready active_generation=0 active_valid=0` and enters the
      intended HID-disabled recovery UI.
- [x] Confirm no MSC interface is exposed. The board reports only keyboard and
      vendor-sync interfaces.
- [x] Confirm the vendor sync child binds to Microsoft's `winusb.inf`.
      Windows reports a healthy `USBDevice` “Sync channel” on interface 1,
      alongside the healthy keyboard interface. The final descriptor includes
      a BOS Microsoft OS 2.0 compatible ID and stable device-interface GUID;
      no MSC child is present.
- [x] Exercise the wire protocol from Windows with
      `tools/m3_sync_smoke.ps1`. The physical device returned capability word
      `0x0000001F`; the end-to-end deterministic 112-byte bundle test staged,
      verified and atomically committed generation 1 with payload CRC
      `0xBDDFEACA`. A separate persisted-resume test accepted 56 bytes,
      returned offset 56 from a fresh `BEGIN`, then aborted; the active
      generation remained 1.
- [x] Reset and boot recovery with the committed bundle. The physical board
      logged `M3_SD result=ready active_generation=1 active_valid=1` and
      `M3_COMPLETE sd_ready=1 active_bundle=1 hid_output=disabled msc=disabled`.
- [x] M4 sustained-upload compatibility: response sending now waits up to
      250 ms for TinyUSB's vendor IN buffer instead of silently dropping an
      acknowledgement. The corrected 807,712-byte image was flashed and
      hash-verified on COM8; M4 then completed an atomic project commit.
- [ ] Interrupt transfer before, during and after commit; verify boot selects either the old valid pointer or no bundle, never a partial bundle.

## Handoff

- `firmware/m3_sync_storage/PROTOCOL.md` defines the v1 wire contract for M4.
- `tools/m3_sync_smoke.ps1` is an architecture-neutral Windows WinUSB smoke
  test (it uses platform P/Invoke and works under x64 or ARM64 PowerShell).
- The remaining M3 test is deliberately manual: remove the OTG cable at each
  transfer phase and reboot the board, then use the smoke test to confirm the
  active generation is unchanged unless commit already completed.
