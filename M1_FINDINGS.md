# M1 findings — touch-to-HID vertical slice

Status: **composite firmware implemented and build-verified; refreshed physical acceptance pending attached board** (2026-07-12)

## Scope

M1 proves the direct device path from the already-verified GT911 touch panel to
Windows' built-in USB HID stack and a driverless WinUSB probe interface. It is
intentionally not the macro engine or desktop synchronisation protocol.

| Area | M1 decision |
| --- | --- |
| USB interfaces | One HID collection with keyboard, consumer and mouse report IDs, plus a WinUSB vendor interface; no custom Windows driver |
| Keys | F13–F24 (`0x68`–`0x73`), with a 25 ms press/release pulse |
| Touch trigger | LVGL `CLICKED` (release) event |
| Grid | 8×4, 32 square 149 px diagnostic tiles centred in 1280×720 |
| Letterboxing | 50 px black bar at the top and bottom on the verified panel |
| Diagnostic labels | Present only in M1 to identify the emitted F-key; product UI stays icon-only |
| USB port | Dedicated board USB OTG port, distinct from the CH343 USB-to-UART flash/log port |

## Handoff from M0

- Landscape display orientation is physically confirmed: USB ports on the right.
- GT911 mapping is physically confirmed by taps at all four landscape corners.
- COM8 is the connected CH343 debug UART path used for flashing and serial logs.
- A Windows PnP scan before M1 found no ESP32-P4 OTG HID device, so the dedicated
  OTG data port still needs to be attached to the laptop/PC for enumeration.

## Build and board evidence

| Check | Result |
| --- | --- |
| Build toolchain | ESP-IDF 5.5.4; `espressif/esp_tinyusb` 2.2.1 |
| Firmware image | `screendeck_m1_hid_touch.bin`, 701,712 bytes; 33% factory-partition free |
| Flash target | ESP32-P4 revision 1.3 on COM8 (CH343); write and image hash verified |
| USB stack | TinyUSB installed on P4 port 1; default Espressif descriptor VID `303A`, PID `4004` |
| Screen geometry | 1280×720 logical landscape; 8×4; 149 px square tiles; 50 px black bar above and below |
| Touch driver | GT911 found at `0x5D`; touch input registered |
| Firmware readiness | `M1_COMPLETE grid_ready=1 usb_waiting_for_otg_host=1` emitted at 2.285 s boot |

## Touch orientation correction

M1's first flash used the vendor's portrait-default GT911 flags with a
`ROTATE_90` landscape LVGL display. This leaves touch in 720×1280 portrait
coordinates while the UI is 1280×720, causing obvious wrong/unreachable tile
hits. The final USB-right landscape configuration is
`swap_xy=true`, `mirror_x=true`, which maps native `(x, y)` to landscape
`(1279 - y, x)`. The intermediate `mirror_y` variant had the correct axes but
was rotated 180°, as physical bottom-left and bottom-right landed at top-right
and top-left respectively.

The intermediate landscape firmware was built and hash-verified on COM8. Four
physical corner taps showed the expected coordinate extent, then revealed the
180° rotation described above. The final `mirror_x` correction was
verified on hardware:

| Logical coordinate | Expected tile | Observed tile | HID pulse |
| --- | --- | --- | --- |
| 1184, 574 | one physical corner | column 7, row 3 | F20 press/release, 25 ms |
| 90, 590 | one physical corner | column 0, row 3 | F13 press/release, 25 ms |
| 1178, 120 | one physical corner | column 7, row 0 | F20 press/release, 25 ms |
| 120, 126 | one physical corner | column 0, row 0 | F13 press/release, 25 ms |

Final physical verification passed: the user confirmed that mappings look good
across the grid. The serial trace independently recorded lower-left at
`107,628` → column 0, row 3; lower-right at `1217,632` → column 7, row 3; and
upper-right at `1187,110` → column 7, row 0. The final transform is accepted.

The two pre-existing vendor BSP warnings remain visible: LEDC GPIO 26 is marked
as conflicting, and the generic GT911 I²C pull-up warning is emitted. M0's
physical touch validation still applies; neither warning blocked M1 startup.

## Remaining acceptance evidence

- [x] Refreshed composite firmware builds with ESP-IDF 5.5.4 and pinned
      `esp_tinyusb` 2.2.1. Image: 706,256 bytes (`0xAC6D0`), 33% app-partition
      headroom.
- [x] Historical keyboard-only firmware flashed and reached `M1_COMPLETE` on COM8.
- [x] Historical test connected dedicated USB OTG and debug UART simultaneously.
- [x] Historical PID `4004` image enumerated as a standard
      HID keyboard.
- [x] Historical physical test logged `M1_TOUCH`, `M1_HID action=press`, and
      `M1_HID action=release`.
- [x] Refreshed VID `303A`, PID `4010` image flashed through the P4 ROM USB-OTG
      downloader on COM9; bootloader, partition table and application writes
      were hash-verified. COM9 disappearing during the post-flash hard reset was
      expected because the application re-enumerates as a composite device.
- [x] Windows x64 enumerates the refreshed composite parent plus keyboard,
      consumer-control and mouse HID collections. Interface 1 appears healthy as
      `M1 WinUSB probe` with service `WINUSB`; all six reported nodes are `OK`.
- [ ] F13–F24 are observed by [the AutoHotkey v2 probe](tools/m1_f13_f24_probe.ahk).
      The probe now writes a persistent CSV and reports 12/12 coverage. This PC
      currently has AutoHotkey 1.1 only, so v2 remains required before running it.
- [ ] Capture `touch_to_submit_us` for representative and rapid taps. This is
      device-side release-callback to TinyUSB report submission and must remain
      at or below 10,000 us with zero queue drops. It is not host-arrival latency.
- [ ] Measure press-to-visible feedback at the panel against the 20 ms target
      with an external high-speed capture or instrumentation that includes the
      GT911 interrupt and display scanout; LVGL callback timestamps alone cannot
      establish photons-on-screen latency.
- [ ] Repeat enumeration, F13–F24 capture, and timing acceptance on native
      Windows ARM64. No ARM64 Windows host is available in this workspace.

## 2026-07-12 implementation correction

- Added Microsoft OS 2.0 BOS descriptors for driverless WinUSB binding and a
  stable interface GUID, alongside keyboard, consumer and mouse HID reports.
- Added monotonically numbered touch events, release timestamps, queue-drop
  accounting, and `touch_to_submit_us` measurements. Configured report cadence
  is no longer presented as measured latency.
- Updated the AHK v2 probe to use valid v2 loop syntax, persist every observation
  to `tools/m1_f13_f24_results.csv`, and display missing-key coverage.
- Clean ESP-IDF 5.5.4 build completed in `.m1_verify`. The image was subsequently
  flashed and its complete x64 composite USB/WinUSB enumeration was verified.
  Touch-to-key and latency gates still require taps plus an AHK v2 capture.
