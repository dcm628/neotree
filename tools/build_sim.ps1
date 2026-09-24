<#
.SYNOPSIS
Builds the host simulator (neotree_sim, neotree_view) and the engine unit
tests, then runs the tests. See sim/README.md.

.PARAMETER Clean
Delete sim/build first and configure from scratch.

.PARAMETER NoViewer
Skip the raylib viewer (faster first build, no download).

.PARAMETER NoTests
Build only; don't run the unit tests.
#>
param(
    [switch]$Clean,
    [switch]$NoViewer,
    [switch]$NoTests
)
# Not 'Stop': Windows PowerShell turns a native tool's stderr (CMake
# deprecation notices, compiler warnings) into terminating errors when output
# is redirected. Failures are caught by the explicit exit-code checks instead.
$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo 'sim'
$build = Join-Path $source 'build'

# Tools are found on PATH first, then where their installers put them - a
# fresh shell outside VSCode may not have any of them on PATH.
function Find-Tool([string]$name, [string[]]$globs) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($g in $globs) {
        $hit = Get-ChildItem $g -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "$name not found (looked on PATH and in: $($globs -join ', '))"
}

$winlibs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.*\mingw64\bin"
$gxx = Find-Tool 'g++' @("$winlibs\g++.exe")
$gcc = Join-Path (Split-Path $gxx) 'gcc.exe'
$cmake = Find-Tool 'cmake' @("$env:USERPROFILE\.pico-sdk\cmake\*\bin\cmake.exe", "$winlibs\cmake.exe")
$ninja = Find-Tool 'ninja' @("$env:USERPROFILE\.pico-sdk\ninja\*\ninja.exe", "$winlibs\ninja.exe")
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'

# The compiler's own directory has to be on PATH for its helper programs.
$env:Path = "$(Split-Path $gxx);$env:Path"

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    $viewer = if ($NoViewer) { 'OFF' } else { 'ON' }
    & $cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$gxx" `
        "-DCMAKE_C_COMPILER=$gcc" "-DNEOTREE_BUILD_VIEWER=$viewer"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}

& $cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if (-not $NoTests) {
    & $ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

Write-Host ""
Write-Host "Built:"
Get-ChildItem $build -Filter 'neotree_*.exe' | ForEach-Object { Write-Host "  $($_.FullName)" }
