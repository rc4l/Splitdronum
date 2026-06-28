# Build the splitdronum GUI launcher (Qt Quick -- same Qt 6 stack as the in-game overlay). Discovers
# CMake (PATH, else the one bundled with VS's C++ CMake tools), points it at the vendored Qt in deps\,
# builds, then runs windeployqt so the Qt runtime DLLs + QML plugins sit next to build\launcher.exe.
# Output: build\launcher.exe (next to host.exe). Run it, or use play.ps1 directly.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot

$qtPrefix = Join-Path $root 'deps\qt\6.8.1\msvc2022_64'
if (-not (Test-Path $qtPrefix)) { Write-Host "ERROR: Qt not found at '$qtPrefix'. Fetch it (aqtinstall) into deps\ first."; exit 2 }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsroot  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $bundled = Join-Path $vsroot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { $cmake = $bundled }
    }
}
if (-not $cmake -or -not (Test-Path $cmake)) {
    Write-Host 'ERROR: CMake not found. Install CMake, or add the "C++ CMake tools for Windows" VS component.'; exit 2
}

$src = Join-Path $root 'launcher'
$bld = Join-Path $root 'build\launcher-cmake'   # under build\ (gitignored)
& $cmake -S $src -B $bld -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtPrefix"
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: CMake configure failed.'; exit 3 }
& $cmake --build $bld --config Release
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: build failed.'; exit 3 }

# Deploy the Qt runtime (DLLs + QML plugins for Controls/Layouts/Dialogs) next to launcher.exe so it
# runs standalone. --qmldir lets windeployqt discover the QML imports from the source.
$exe = Join-Path $root 'build\launcher.exe'
$wdq = Join-Path $qtPrefix 'bin\windeployqt.exe'
if ((Test-Path $exe) -and (Test-Path $wdq)) {
    & $wdq --release --no-translations --qmldir (Join-Path $src 'qml') $exe | Out-Null
}
Write-Host ("launcher.exe: " + (Test-Path $exe))
