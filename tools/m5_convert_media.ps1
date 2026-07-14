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
$sidecar = Join-Path $root "desktop\m4_editor\src-tauri\resources\ffmpeg\$Architecture\ffmpeg.exe"
$ffmpeg = if (Test-Path -LiteralPath $sidecar) { $sidecar } else { (Get-Command ffmpeg -ErrorAction Stop).Source }
$parent = Split-Path $OutputPath -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null

# Fixed thread count, timestamps, pixel format, dimensions and metadata make
# repeated conversions byte-for-byte reproducible with the same FFmpeg build.
$common = @('-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-threads', '1', '-i', $InputPath,
            '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:v', '+bitexact', '-an', '-sn', '-dn')
if ($Kind -eq 'Screensaver') {
    & $ffmpeg @common '-vf' 'fps=30,scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black' `
        '-c:v' 'mjpeg' '-q:v' '5' '-pix_fmt' 'yuvj420p' '-f' 'mjpeg' $OutputPath
} else {
    # Device-ready animation frames: native 149x149 baseline JPEGs at 15 FPS.
    # OutputPath is a printf pattern, for example icon-%04d.jpg.
    & $ffmpeg @common '-vf' "fps=15,scale=149:149:force_original_aspect_ratio=decrease,pad=149:149:(ow-iw)/2:(oh-ih)/2:black,format=rgb24,geq=r='if(gte(min(X,W-1-X),12)+gte(min(Y,H-1-Y),12)+lte(pow(12-min(X,W-1-X),2)+pow(12-min(Y,H-1-Y),2),144),r(X,Y),32)':g='if(gte(min(X,W-1-X),12)+gte(min(Y,H-1-Y),12)+lte(pow(12-min(X,W-1-X),2)+pow(12-min(Y,H-1-Y),2),144),g(X,Y),33)':b='if(gte(min(X,W-1-X),12)+gte(min(Y,H-1-Y),12)+lte(pow(12-min(X,W-1-X),2)+pow(12-min(Y,H-1-Y),2),144),b(X,Y),38)',format=yuvj420p" `
        '-c:v' 'mjpeg' '-q:v' '5' '-pix_fmt' 'yuvj420p' '-start_number' '0' $OutputPath
}
if ($LASTEXITCODE -ne 0) { throw "FFmpeg conversion failed with exit code $LASTEXITCODE" }
Write-Output $OutputPath
