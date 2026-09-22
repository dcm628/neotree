<#
.SYNOPSIS
    One-shot build + deploy: builds neo_tree.uf2 on this desktop, ships it to
    the Pi, and flashes + verifies the Pico over the Pi's USB connection.
    No physical access to either machine required.

.DESCRIPTION
    Runs ninja in firmware/build, scp's the resulting .uf2 to the Pi, then
    runs tools/pi_flash.py on the Pi (from the mapping venv, which already
    has pyserial) to trigger a BOOTSEL reset, flash it, and confirm the new
    firmware actually responds over serial before declaring success.

    Requires: the Pi's clone of this repo is up to date (git pull) so
    tools/pi_flash.py is present there, and the Pico's USB stays plugged
    into the Pi throughout.

    ninja is only on PATH inside VSCode's own integrated terminal (the Pico
    extension injects it there). A plain PowerShell/SSH session - which is
    how this script is normally invoked - does not have it, and repeatedly
    failed with "ninja is not recognized" until this script started locating
    it itself instead of assuming PATH. See Find-Ninja below.

.PARAMETER PiHost
    SSH host alias for the Pi. Defaults to "treepi" (~/.ssh/config).

.EXAMPLE
    .\tools\deploy.ps1
#>
param(
    [string]$PiHost = "treepi"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "firmware\build"
$Uf2Path = Join-Path $BuildDir "neo_tree.uf2"

function Fail($msg) {
    Write-Host "DEPLOY FAILED: $msg" -ForegroundColor Red
    exit 1
}

function Find-Ninja {
    # Prefer PATH if it's actually set up (e.g. running inside VSCode's own
    # integrated terminal, where the Pico extension injects it) - but don't
    # depend on it, since a plain PowerShell session spawned any other way
    # (a new terminal, an SSH session, this script run standalone) does not
    # have it on PATH and this has repeatedly failed with "ninja is not
    # recognized" as a result. Fall back to searching where the Pico
    # extension actually installs it, picking the newest version present
    # rather than hardcoding one, since the extension can update it.
    $onPath = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $sdkNinjaRoot = Join-Path $env:USERPROFILE ".pico-sdk\ninja"
    if (Test-Path $sdkNinjaRoot) {
        $found = Get-ChildItem -Path $sdkNinjaRoot -Filter "ninja.exe" -Recurse -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { return $found.FullName }
    }

    Fail "could not find ninja.exe on PATH or under $sdkNinjaRoot - is the Pico VSCode extension's toolchain installed?"
}

$ninja = Find-Ninja
Write-Host "[build] running $ninja in $BuildDir" -ForegroundColor Cyan
Push-Location $BuildDir
try {
    & $ninja
    if ($LASTEXITCODE -ne 0) { Fail "ninja build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

if (-not (Test-Path $Uf2Path)) { Fail "build succeeded but $Uf2Path not found" }
$size = (Get-Item $Uf2Path).Length
Write-Host "[build] OK: $Uf2Path ($size bytes)" -ForegroundColor Green

Write-Host "[deploy] copying to ${PiHost}:/tmp/neo_tree.uf2" -ForegroundColor Cyan
scp $Uf2Path "${PiHost}:/tmp/neo_tree.uf2"
if ($LASTEXITCODE -ne 0) { Fail "scp failed (exit $LASTEXITCODE)" }

Write-Host "[deploy] flashing + verifying via $PiHost..." -ForegroundColor Cyan
ssh $PiHost "cd ~/workspace/neotree/mapping && source .venv/bin/activate && python3 ~/workspace/neotree/tools/pi_flash.py /tmp/neo_tree.uf2"
if ($LASTEXITCODE -ne 0) { Fail "remote flash/verify failed (exit $LASTEXITCODE) - see output above" }

Write-Host "[deploy] DONE - firmware built, flashed, and verified running." -ForegroundColor Green
