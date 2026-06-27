# Build the splitscreen compositor (host.exe): host/host.cpp + core modules + the Qt Quick overlay
# (host/overlay_qt.cpp), linked against user32/gdi32/d3d + Qt6. Portable: discovers the VS2022 x64
# toolchain via vswhere and Qt under deps/qt. windeployqt stages the Qt runtime next to host.exe.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Write-Host 'ERROR: Visual Studio not found.'; exit 2 }
$vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsroot 'VC\Auxiliary\Build\vcvars64.bat'

# Qt (fetched by aqtinstall to deps/qt -- see README). Resolve the version dir so a Qt bump just works.
$qtBase = Join-Path $root 'deps\qt'
$qtVer  = (Get-ChildItem $qtBase -Directory -EA SilentlyContinue | Where-Object { $_.Name -match '^6\.' } | Select-Object -First 1).Name
$qt     = Join-Path $qtBase "$qtVer\msvc2022_64"
if (-not (Test-Path (Join-Path $qt 'bin\windeployqt.exe'))) { Write-Host "ERROR: Qt not found under $qtBase (run aqtinstall -- see README)."; exit 3 }
$qtInc  = Join-Path $qt 'include'
$qtLib  = Join-Path $qt 'lib'

$hostcpp = Join-Path $root 'host\host.cpp'
$layout  = Join-Path $root 'core\ss_layout.cpp'
$join    = Join-Path $root 'core\ss_join.cpp'
$profile = Join-Path $root 'core\ss_profile.cpp'
$overlay = Join-Path $root 'host\overlay_qt.cpp'
$rc      = Join-Path $root 'host\host.rc'        # app icon resource -> host.res, linked into host.exe

$qtI = "/I `"$qtInc`" /I `"$qtInc\QtCore`" /I `"$qtInc\QtGui`" /I `"$qtInc\QtQml`" /I `"$qtInc\QtQuick`""
$qtL = "`"$qtLib\Qt6Core.lib`" `"$qtLib\Qt6Gui.lib`" `"$qtLib\Qt6Qml.lib`" `"$qtLib\Qt6Quick.lib`""

# overlay_qt.cpp needs Qt's required /permissive- (strict conformance); compile it to an .obj on its own
# so host.cpp keeps its relaxed flags, then link everything together.
cmd /c "`"$vcvars`" >nul && cd /d `"$out`" && rc /nologo /fo host.res `"$rc`" && cl /c /nologo /O2 /EHsc /std:c++17 /Zc:__cplusplus /permissive- $qtI `"$overlay`" && cl /nologo /O2 /EHsc /Fe:host.exe `"$hostcpp`" `"$layout`" `"$join`" `"$profile`" overlay_qt.obj host.res user32.lib gdi32.lib xinput9_1_0.lib winmm.lib dwmapi.lib d3d11.lib dxgi.lib d3dcompiler.lib ws2_32.lib $qtL"

if (Test-Path (Join-Path $out 'host.exe')) {
    # stage the Qt runtime (DLLs + QtQuick/QtQuick.Effects plugins) next to host.exe so it loads
    & (Join-Path $qt 'bin\windeployqt.exe') --qmldir (Join-Path $root 'overlay\qml') --no-translations --no-compiler-runtime (Join-Path $out 'host.exe') *> $null
}
Write-Host ("host.exe: " + (Test-Path (Join-Path $out 'host.exe')))
