# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding()]
param(
    [ValidateSet('all', 'editor', 'firmware', 'test')]
    [string] $Target = 'all',

    [switch] $NoRestore,
    [switch] $Flash,
    [string] $Port
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$editor = Join-Path $root 'editor'
$firmware = Join-Path $root 'firmware'
$waveshare = Join-Path $root 'vendor\waveshare-reference\examples\esp-idf\10_mp4_player\components\esp32_p4_wifi6_touch_lcd_5\CMakeLists.txt'

function Require-Command([string] $Name, [string] $InstallHint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing '$Name'. $InstallHint"
    }
}

function Invoke-Checked([string] $Executable, [string[]] $CommandArguments, [string] $WorkingDirectory) {
    Push-Location $WorkingDirectory
    try {
        Write-Host "> $Executable $($CommandArguments -join ' ')" -ForegroundColor Cyan
        & $Executable $CommandArguments
        if ($LASTEXITCODE -ne 0) {
            throw "'$Executable' failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }
}

function Restore-Editor {
    Require-Command 'node' 'Install Node.js 20 or newer.'
    Require-Command 'npm.cmd' 'Install Node.js 20 or newer.'
    Require-Command 'cargo' 'Install the stable Rust toolchain.'
    $nodeMajor = [int] ((& node --version).TrimStart('v').Split('.')[0])
    if ($nodeMajor -lt 20) { throw 'ScreenDeck requires Node.js 20 or newer.' }
    if (-not $NoRestore) {
        Invoke-Checked 'npm.cmd' @('ci', '--no-audit') $editor
    }
    elseif (-not (Test-Path -LiteralPath (Join-Path $editor 'node_modules'))) {
        throw 'Editor dependencies are missing. Run again without -NoRestore.'
    }
}

function Test-Editor {
    Restore-Editor
    Invoke-Checked 'npm.cmd' @('run', 'check') $editor
    Invoke-Checked 'npm.cmd' @('test') $editor
    Invoke-Checked 'npm.cmd' @('run', 'build') $editor
    Invoke-Checked 'cargo' @('test', '--manifest-path', 'src-tauri/Cargo.toml') $editor
    & (Join-Path $root 'tests\gesture-matrix.ps1')
}

function Build-Editor {
    Restore-Editor
    Invoke-Checked 'npm.cmd' @('run', 'tauri', 'build') $editor
    Write-Host "Editor packages: $editor\src-tauri\target\release\bundle\nsis" -ForegroundColor Green
}

function Initialize-FirmwareDependencies {
    if (Test-Path -LiteralPath $waveshare) { return }

    Require-Command 'git' 'Install Git and clone the repository with submodules.'
    Invoke-Checked 'git' @('submodule', 'update', '--init', '--recursive') $root
    if (-not (Test-Path -LiteralPath $waveshare)) {
        throw 'The Waveshare BSP submodule is incomplete.'
    }
}

function Build-Firmware {
    Initialize-FirmwareDependencies
    Require-Command 'idf.py' 'Install and activate ESP-IDF 5.5.x in this shell.'
    if (-not $env:IDF_PATH) {
        throw 'IDF_PATH is not set. Run the ESP-IDF export script first.'
    }
    $idfVersion = (& idf.py --version) -join ' '
    if ($idfVersion -notmatch 'v5\.5\.') {
        throw "ScreenDeck requires ESP-IDF 5.5.x; found '$idfVersion'."
    }

    $arguments = @('-C', $firmware, 'build')
    if ($Flash) {
        if (-not $Port) { throw 'Pass the board UART port with -Port when using -Flash.' }
        $arguments = @('-C', $firmware, '-p', $Port, 'flash')
    }
    Invoke-Checked 'idf.py' $arguments $root
    Write-Host "Firmware image: $firmware\build\screendeck.bin" -ForegroundColor Green
}

if ($Flash -and $Target -notin @('all', 'firmware')) {
    throw '-Flash only applies to the firmware and all targets.'
}

switch ($Target) {
    'test' { Test-Editor }
    'editor' { Build-Editor }
    'firmware' { Build-Firmware }
    'all' {
        Build-Editor
        Build-Firmware
    }
}
