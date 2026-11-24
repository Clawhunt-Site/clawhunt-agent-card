# Build the card's firmware on Windows with a local ESP-IDF 5.5.3. The application image lands in dist\,
# named after the firmware's version.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -IdfPath C:\esp\esp-idf-v5.5.3 -IdfToolsPath C:\esp\tools
#
# -IdfPath and -IdfToolsPath default to the IDF_PATH and IDF_TOOLS_PATH of the shell. It can be started from
# Git Bash: the MSYSTEM variable that makes ESP-IDF refuse to run is cleared here.
# With Docker, or on Linux and macOS: build.sh.
#
# The version is CARD_FW_VERSION in firmware\main\card_model.h. It is also written into the image, so
# `python tools\flash.py check` can say which firmware a card runs.
# The image is the application only: it is written to 0x10000 by tools\flash.py, never from 0x0.
param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfToolsPath = $env:IDF_TOOLS_PATH
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath 'export.ps1'))) {
    throw 'ESP-IDF 5.5.3 was not found: pass -IdfPath <its folder> or set IDF_PATH (Espressif''s Get Started guide says how to install it).'
}

$model = Join-Path $root 'firmware\main\card_model.h'
$line = Select-String -LiteralPath $model -Pattern '^#define CARD_FW_VERSION "([0-9]+[.][0-9]+[.][0-9]+)"' | Select-Object -First 1
if (-not $line) {
    throw "no version like 1.2.3 found as CARD_FW_VERSION in $model"
}
$version = $line.Matches[0].Groups[1].Value

Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$env:IDF_PATH = $IdfPath
if ($IdfToolsPath) { $env:IDF_TOOLS_PATH = $IdfToolsPath }

# From here on python, cmake and ninja run. What they say on stderr is not an error; Windows PowerShell 5.1 makes
# it one when its own output goes to a pipe or a file, and 'Stop' would then end the build at the first warning.
# Their exit code is what counts.
$ErrorActionPreference = 'Continue'
. (Join-Path $IdfPath 'export.ps1') | Out-Null
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "export.ps1 of $IdfPath did not set ESP-IDF up: idf.py is not on the PATH"
}
$idfVersion = (& idf.py --version 2>&1 | Out-String).Trim()
if ($idfVersion -notmatch '5\.5\.3') {
    throw "this firmware is built with ESP-IDF 5.5.3; the one at $IdfPath says: $idfVersion"
}

Write-Host "building firmware $version with $idfVersion at $IdfPath"
Push-Location -LiteralPath (Join-Path $root 'firmware')
try {
    & idf.py "-DPROJECT_VER=$version" build
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
$ErrorActionPreference = 'Stop'
if ($code -ne 0) {
    throw "idf.py build failed (exit $code)"
}

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$name = "clawhunt-agent-card-$version-app.bin"
$out = Join-Path $dist $name
Copy-Item -LiteralPath (Join-Path $root 'firmware\build\FoloToy-AI-Passport.bin') -Destination $out -Force
$hash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLower()
# the line build.sh writes, with its line ending
[System.IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS.txt'), "$hash  $name`n", (New-Object System.Text.ASCIIEncoding))
Write-Host "$hash  $name"
Write-Host "built dist\$name"
Write-Host "next: python tools\flash.py flash dist\$name"
