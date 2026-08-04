# ScreenDeck firmware

The firmware targets the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 with ESP-IDF 5.5.x. It renders the 8×4 key grid, runs HID macros, handles radial gestures and animated media, stores bundles on microSD, and syncs over WinUSB.

Build or flash from an activated ESP-IDF PowerShell:

```powershell
./build.ps1 -Target firmware
./build.ps1 -Target firmware -Flash -Port COM8
```

The build script initializes the pinned Waveshare BSP submodule and lets ESP-IDF resolve TinyUSB and the remaining managed components. Local `sdkconfig`, `managed_components`, and build output stay untracked; `sdkconfig.defaults` holds the project configuration.

The board has two USB ports: use USB-to-UART for flashing and logs, and USB-OTG for HID and WinUSB sync.
