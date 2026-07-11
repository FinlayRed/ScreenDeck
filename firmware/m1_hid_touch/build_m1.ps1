$ErrorActionPreference = 'Stop'
$env:IDF_PYTHON_ENV_PATH = 'C:\Users\Finlay\.espressif\python_env\idf5.5_py3.11_env'
. 'C:\Users\Finlay\esp\v5.5.4\export.ps1'
Set-Location $PSScriptRoot
idf.py build *>&1 | Tee-Object -FilePath 'build\m1_build.log'
exit $LASTEXITCODE
