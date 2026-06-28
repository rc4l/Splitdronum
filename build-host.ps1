# Build the cross-platform Qt host/compositor (host.exe): host/host_qt.cpp built via the root CMake
# project (-DSS_BUILD_HOST=ON), which also fetches + builds SDL2 (static, gamepads only) and links Qt6.
# Qt Quick is the compositor: it draws the seat framebuffers AND the QML overlay in one scene on Qt's RHI
# (D3D11 here). This is the convergence default; the legacy Win32/D3D11 host is build-host-legacy.ps1.
# Output: build\host.exe + the Qt runtime staged beside it. Discovers CMake (PATH, else VS's bundled one)
# and Qt under deps\qt.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$qtBase = Join-Path $root 'deps\qt'
$qtVer  = (Get-ChildItem $qtBase -Directory -EA SilentlyContinue | Where-Object { $_.Name -match '^6\.' } | Select-Object -First 1).Name
$qt     = Join-Path $qtBase "$qtVer\msvc2022_64"
if (-not (Test-Path (Join-Path $qt 'bin\windeployqt.exe'))) { Write-Host "ERROR: Qt not found under $qtBase (run aqtinstall -- see README)."; exit 3 }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsroot  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $bundled = Join-Path $vsroot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { $cmake = $bundled }
    }
}
if (-not $cmake -or -not (Test-Path $cmake)) { Write-Host 'ERROR: CMake not found (install it, or the "C++ CMake tools for Windows" VS component).'; exit 2 }

$bld = Join-Path $out 'host-cmake'   # under build\ (gitignored)
& $cmake -S $root -B $bld -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qt" -DSS_BUILD_HOST=ON -DSS_BUILD_LAUNCHER=OFF
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: CMake configure failed.'; exit 3 }
& $cmake --build $bld --config Release --target sshost
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: build failed.'; exit 3 }

# Stage host.exe (named "host" by the target) into build\ next to ss_hook.dll, then deploy the Qt runtime
# (DLLs + QtQuick plugins). The host finds qml/Overlay.qml at ..\overlay\qml relative to build\host.exe.
$built = Join-Path $bld 'Release\host.exe'
$exe   = Join-Path $out 'host.exe'
if (Test-Path $built) {
    Copy-Item $built $exe -Force
    & (Join-Path $qt 'bin\windeployqt.exe') --release --qmldir (Join-Path $root 'overlay\qml') --no-translations --no-compiler-runtime $exe *> $null
}
Write-Host ("host.exe (Qt): " + (Test-Path $exe))
