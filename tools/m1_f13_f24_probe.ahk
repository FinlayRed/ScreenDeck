#Requires AutoHotkey v2.0
#SingleInstance Force

; M1 acceptance harness.  Run this script, then tap a Screendeck M1 tile.
; It records every F13--F24 observation and reports complete coverage. It does
; not suppress any key. Delete the CSV before a new acceptance run if desired.
logPath := A_ScriptDir "\m1_f13_f24_results.csv"
seen := Map()
if !FileExist(logPath)
    FileAppend("host_timestamp,key,event`n", logPath, "UTF-8")

Loop 12 {
    keyNumber := A_Index + 12
    Hotkey("~F" keyNumber, RecordKey.Bind(keyNumber))
}

RecordKey(keyNumber, *) {
    global logPath, seen
    key := "F" keyNumber
    seen[key] := true
    stamp := FormatTime(, "yyyy-MM-dd HH:mm:ss.fff")
    FileAppend(stamp "," key ",down`n", logPath, "UTF-8")
    missing := []
    Loop 12 {
        number := A_Index + 12
        if !seen.Has("F" number)
            missing.Push("F" number)
    }
    status := missing.Length = 0
        ? "PASS: observed F13-F24"
        : "Observed " seen.Count "/12; missing " Join(missing, " ")
    ToolTip(key " detected at " stamp "`n" status)
    SetTimer(() => ToolTip(), -1200)
}

Join(values, separator) {
    result := ""
    for index, value in values
        result .= (index = 1 ? "" : separator) value
    return result
}

Esc::ExitApp
