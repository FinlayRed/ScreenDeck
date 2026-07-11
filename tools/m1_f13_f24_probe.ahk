#Requires AutoHotkey v2.0
#SingleInstance Force

; M1 acceptance harness.  Run this script, then tap a Screendeck M1 tile.
; It only observes the F13--F24 range and does not suppress any key.
for keyNumber in 13..24 {
    Hotkey("~F" keyNumber, RecordKey.Bind(keyNumber))
}

RecordKey(keyNumber, *) {
    ToolTip("Screendeck M1 detected F" keyNumber " at " A_Hour ":" A_Min ":" A_Sec)
    SetTimer(() => ToolTip(), -1200)
}

Esc::ExitApp
