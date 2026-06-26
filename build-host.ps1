# Build the splitscreen compositor (host.exe): host/host.cpp + core/ss_layout.cpp, linked
# against user32/gdi32. Portable: discovers the VS2022 x64 toolchain via vswhere.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Write-Host 'ERROR: Visual Studio not found.'; exit 2 }
$vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsroot 'VC\Auxiliary\Build\vcvars64.bat'

$hostcpp = Join-Path $root 'host\host.cpp'
$layout  = Join-Path $root 'core\ss_layout.cpp'
cmd /c "`"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /O2 /EHsc /Fe:host.exe `"$hostcpp`" `"$layout`" user32.lib gdi32.lib xinput9_1_0.lib winmm.lib dwmapi.lib d3d11.lib dxgi.lib d3dcompiler.lib"
Write-Host ("host.exe: " + (Test-Path (Join-Path $out 'host.exe')))
