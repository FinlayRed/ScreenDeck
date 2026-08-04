# M2 findings — runtime UI and safe macro engine

Status: **in progress** (2026-07-11)

## Scope

M2 turns the verified touch-to-HID slice into an autonomous runtime. It uses a
compiled, versioned default bundle only for this milestone; M3 replaces that
source with an atomically synchronized microSD bundle.

| Area | M2 implementation |
| --- | --- |
| UI | Two icon-only 8×4 square pages, centred in landscape with black bars |
| Actions | Keyboard macros plus explicit next/previous page and next-profile buttons |
| Config | Versioned, bounds-checked bundle model with a disabled-HID fallback |
| Scheduler | Eight concurrent slots; restarting a physical button cancels only its own prior slot |
| HID safety | Ref-counted keyboard and modifier state; central report composition; six-key overflow releases all |
| Fault paths | Boot, USB detach, display startup failure and scheduler cleanup release all held controls |

## Hardware gates to complete

- [x] Firmware builds and flashes on the physical P4. `screendeck_m2_macro_runtime.bin`
      is 704,288 bytes with 33% factory-partition headroom; COM8 write hash verified.
- [x] M2 starts with the accepted bundle and landscape UI. Serial markers confirm
      bundle v1 accepted (2 pages, 2 profiles, 12 macros), USB mounted, the
      1280×720 8×4 square grid, and eight scheduler slots.
- [ ] Page/profile actions repaint correctly without leaving a key held.
- [ ] Rapid overlapping macro taps and same-button restarts produce no stuck HID state.
- [ ] USB detach while a macro is active emits the central safe-release path.
