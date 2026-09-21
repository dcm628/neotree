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

Write-Host "[build] running ninja in $BuildDir" -ForegroundColor Cyan
Push-Location $BuildDir
try {
    ninja
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
