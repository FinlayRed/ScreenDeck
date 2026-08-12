# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Icon', 'Screensaver')][string]$Kind,
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [ValidateSet('Auto', 'x64', 'arm64')][string]$Architecture = 'Auto'
)

$ErrorActionPreference = 'Stop'
$InputPath = (Resolve-Path -LiteralPath $InputPath).Path
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

if ($Architecture -eq 'Auto') {
    $Architecture = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'x64' }
}
$root = Split-Path $PSScriptRoot -Parent
$sidecar = Join-Path $root "editor\src-tauri\resources\ffmpeg\$Architecture\ffmpeg.exe"
$ffmpeg = if (Test-Path -LiteralPath $sidecar) { $sidecar } else { (Get-Command ffmpeg -ErrorAction Stop).Source }
$ffprobe = Join-Path (Split-Path $ffmpeg -Parent) 'ffprobe.exe'
if (-not (Test-Path -LiteralPath $ffprobe)) { $ffprobe = (Get-Command ffprobe -ErrorAction Stop).Source }
$parent = Split-Path $OutputPath -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null

# Fixed thread count, timestamps, pixel format, dimensions and metadata make
# repeated conversions byte-for-byte reproducible with the same FFmpeg build.
$common = @('-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-threads', '1', '-i', $InputPath,
            '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:v', '+bitexact', '-an', '-sn', '-dn')

# Device media contract (mirrors the editor converter and the firmware parser
# in m5_index_mjpeg / m5_mjpeg_file_valid): 720x1280 portrait MJPEG at 60 FPS,
# at most 1800 frames (30 s), total stream <= 16 MiB, each frame <= 2 MiB, and
# every frame decodable at 720x1280.
$SCREENSAVER_FILTER = 'fps=60,scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black,transpose=clock,format=yuvj420p'
$SCREENSAVER_MAX_FRAMES = 1800
$SCREENSAVER_MAX_BYTES = 16 * 1024 * 1024
$SCREENSAVER_MAX_FRAME_BYTES = 2 * 1024 * 1024

function Test-ScreensaverStream {
    param([string]$Path, [string]$Probe)
    $size = (Get-Item -LiteralPath $Path).Length
    if ($size -gt $SCREENSAVER_MAX_BYTES) { return [pscustomobject]@{ Valid = $false; Reason = "stream is $size bytes; limit is $SCREENSAVER_MAX_BYTES" } }
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $text = $latin1.GetString([IO.File]::ReadAllBytes($Path))
    $soi = [string][char]0xFF + [string][char]0xD8
    $eoi = [string][char]0xFF + [string][char]0xD9
    $frameCount = 0
    $maxFrame = 0
    $cursor = 0
    $firstSoi = -1
    while ($true) {
        $start = $text.IndexOf($soi, $cursor)
        if ($start -lt 0) { break }
        if ($firstSoi -lt 0) { $firstSoi = $start }
        $end = $text.IndexOf($eoi, $start + 2)
        if ($end -lt 0) { return [pscustomobject]@{ Valid = $false; Reason = 'truncated frame: EOI not found' } }
        $length = $end + 2 - $start
        if ($length -gt $SCREENSAVER_MAX_FRAME_BYTES) { return [pscustomobject]@{ Valid = $false; Reason = "frame $frameCount is $length bytes; limit is $SCREENSAVER_MAX_FRAME_BYTES" } }
        if ($length -gt $maxFrame) { $maxFrame = $length }
        $frameCount++
        if ($frameCount -gt $SCREENSAVER_MAX_FRAMES) { return [pscustomobject]@{ Valid = $false; Reason = "more than $SCREENSAVER_MAX_FRAMES frames" } }
        $cursor = $end + 2
    }
    if ($frameCount -eq 0) { return [pscustomobject]@{ Valid = $false; Reason = 'no complete frames found' } }
    # Decode the first frame with ffprobe to confirm the dimensions are 720x1280.
    $firstLength = $text.IndexOf($eoi, $firstSoi + 2) + 2 - $firstSoi
    $probePath = Join-Path ([IO.Path]::GetTempPath()) ("screendeck-probe-{0}.jpg" -f ([guid]::NewGuid()))
    try {
        [IO.File]::WriteAllBytes($probePath, $latin1.GetBytes($text.Substring($firstSoi, $firstLength)))
        $dims = & $Probe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 $probePath
        if ($LASTEXITCODE -ne 0 -or $dims -ne '720x1280') {
            return [pscustomobject]@{ Valid = $false; Reason = "first frame decodes to '$dims', expected 720x1280" }
        }
    } finally {
        Remove-Item -LiteralPath $probePath -ErrorAction SilentlyContinue
    }
    return [pscustomobject]@{ Valid = $true; Reason = "frames=$frameCount largest=$maxFrame" }
}

if ($Kind -eq 'Screensaver') {
    $succeeded = $false
    foreach ($quality in @('10', '20', '31')) {
        Remove-Item -LiteralPath $OutputPath -ErrorAction SilentlyContinue
        & $ffmpeg @common '-vf' $SCREENSAVER_FILTER '-frames:v' ([string]$SCREENSAVER_MAX_FRAMES) `
            '-c:v' 'mjpeg' '-q:v' $quality '-pix_fmt' 'yuvj420p' '-f' 'mjpeg' $OutputPath
        if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $OutputPath -ErrorAction SilentlyContinue; continue }
        $result = Test-ScreensaverStream -Path $OutputPath -Probe $ffprobe
        if ($result.Valid) { $succeeded = $true; Write-Verbose "screensaver validated: $($result.Reason)"; break }
        Write-Verbose "screensaver rejected at q=$quality : $($result.Reason)"
        Remove-Item -LiteralPath $OutputPath -ErrorAction SilentlyContinue
    }
    if (-not $succeeded) {
        throw 'Converted screensaver exceeds the device contract (720x1280 at 60 FPS, <= 1800 frames, <= 16 MiB total, <= 2 MiB per frame); choose a shorter or less detailed source.'
    }
} else {
    # Device-ready animation frames: native 149x149 baseline JPEGs at 15 FPS.
    # OutputPath is a printf pattern, for example icon-%04d.jpg.
    & $ffmpeg @common '-vf' "fps=15,scale=149:149:force_original_aspect_ratio=decrease,pad=149:149:(ow-iw)/2:(oh-ih)/2:black,format=rgb24,geq=r='if(gte(min(X,W-1-X),12)+gte(min(Y,H-1-Y),12)+lte(pow(12-min(X,W-1-X),2)+pow(12-min(Y,H-1-Y),2),144),r(X,Y),32)':g='if(gte(min(X,W-1-X),12)+gte(min(Y,H-1-Y),12)+lte(pow(12-min(X,W-1-X),2)+pow(12-min(Y,H-1-Y),2),144),g(X,Y),33)':b='if(gte(min(X,W-1-X),12)+gte(min(Y,H-1-Y),12)+lte(pow(12-min(X,W-1-X),2)+pow(12-min(Y,H-1-Y),2),144),b(X,Y),38)',format=yuvj420p" `
        '-c:v' 'mjpeg' '-q:v' '5' '-pix_fmt' 'yuvj420p' '-start_number' '0' $OutputPath
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg conversion failed with exit code $LASTEXITCODE" }
}
Write-Output $OutputPath
