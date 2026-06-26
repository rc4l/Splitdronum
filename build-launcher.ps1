# Build the splitdronum GUI launcher (Dear ImGui + GLFW). The launcher is cross-platform via CMake;
# this is the Windows wrapper. Discovers VS2022 (vswhere) and CMake (PATH, else the one bundled with
# VS's C++ CMake tools). Output: build\launcher.exe (next to host.exe). Run it, or use play.ps1 directly.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Write-Host 'ERROR: Visual Studio not found.'; exit 2 }
$vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $bundled = Join-Path $vsroot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $bundled) { $cmake = $bundled }
}
if (-not $cmake -or -not (Test-Path $cmake)) {
    Write-Host 'ERROR: CMake not found. Install CMake, or add the "C++ CMake tools for Windows" VS component.'; exit 2
}

$src = Join-Path $root 'launcher'
$bld = Join-Path $root 'build\launcher-cmake'   # under build\ (gitignored)
& $cmake -S $src -B $bld -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: CMake configure failed.'; exit 3 }
& $cmake --build $bld --config Release
Write-Host ("launcher.exe: " + (Test-Path (Join-Path $root 'build\launcher.exe')))
