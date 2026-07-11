$ErrorActionPreference = 'Stop'
$env:IDF_PYTHON_ENV_PATH = 'C:\Users\Finlay\.espressif\python_env\idf5.5_py3.11_env'
. 'C:\Users\Finlay\esp\v5.5.4\export.ps1'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build | Out-Null
idf.py build *>&1 | Tee-Object -FilePath 'build\m5_build.log'
$idfExitCode = $LASTEXITCODE
if ($idfExitCode -ne 0) {
    throw "ESP-IDF build failed with exit code $idfExitCode. See build\m5_build.log."
}
