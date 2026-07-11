# M1 findings — touch-to-HID vertical slice

Status: **hardware bring-up complete; Windows HID acceptance pending OTG cable** (2026-07-11)

## Scope

M1 proves the direct device path from the already-verified GT911 touch panel to
Windows' built-in USB keyboard driver. It is intentionally not the macro engine
or desktop synchronisation protocol.

| Area | M1 decision |
| --- | --- |
| USB interface | One standard USB HID boot keyboard; no custom Windows driver |
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

- [x] Firmware builds with ESP-IDF 5.5.4 and the pinned `esp_tinyusb` component.
- [x] Firmware flashes and reaches `M1_COMPLETE` on COM8.
- [x] Dedicated USB OTG and debug UART are connected simultaneously.
- [x] Windows enumerates `USB\\VID_303A&PID_4004\\M1-TOUCH-HID` as a standard
      HID keyboard.
- [x] Tapping tiles logs `M1_TOUCH`, `M1_HID action=press`, and
      `M1_HID action=release`.
- [ ] F13–F24 are observed by [the AutoHotkey v2 probe](tools/m1_f13_f24_probe.ahk).
