# Build the injected DLL (ss_hook.dll) + the injector (inject.exe). Portable: discovers the
# VS2022 x64 toolchain via vswhere; no machine paths. Engine symbol RVAs are extracted from the
# *built* zandronum.exe at build time (gen_offsets -> dll\ss_offsets.h) so the DLL resolves them
# as (module base + RVA) at runtime with NO fragile runtime DbgHelp. Pass -GamePath if your
# Zandronum build is not in the sibling repo's build\Release.
param(
    [string]$GamePath = (Join-Path $PSScriptRoot '..\zandronum-windows\build\Release\zandronum.exe')
)
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Write-Host 'ERROR: Visual Studio not found.'; exit 2 }
$vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsroot 'VC\Auxiliary\Build\vcvars64.bat'

$inc   = Join-Path $root 'third_party\minhook\include'
$mh    = Join-Path $root 'third_party\minhook\src'
$mhSrc = @("$mh\buffer.c", "$mh\hook.c", "$mh\trampoline.c", "$mh\hde\hde32.c", "$mh\hde\hde64.c")
$dll   = Join-Path $root 'dll\ss_hook.cpp'
$inj   = Join-Path $root 'host\inject.cpp'
$gen   = Join-Path $root 'tools\gen_offsets.cpp'
$offh  = Join-Path $root 'dll\ss_offsets.h'

# --- 1. extract engine symbol RVAs from the built exe -> dll\ss_offsets.h ---
if (-not (Test-Path $GamePath)) {
    Write-Host "ERROR: zandronum.exe not found at '$GamePath' -- build the engine first, or pass -GamePath."; exit 3
}
$GamePath = (Resolve-Path $GamePath).Path
cmd /c "`"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /O2 /Fe:gen_offsets.exe `"$gen`" dbghelp.lib"
if (-not (Test-Path (Join-Path $out 'gen_offsets.exe'))) { Write-Host 'ERROR: gen_offsets build failed.'; exit 3 }
# Engine symbols the DLL needs (single source of truth; the DLL references SS_RVA_<name>):
& (Join-Path $out 'gen_offsets.exe') $GamePath $offh 'D_PostEvent' 'menuactive' 'ConsoleState' 'AppActive' 'I_GetAxes' 'GUICapture' 'g_ulChatMode' 'AddCommandString'
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: engine symbol extraction failed (see above) -- aborting.'; exit 3 }
Write-Host ("ss_offsets.h: " + (Test-Path $offh))

# --- 2. DLL (ss_hook.cpp + MinHook), x64, links opengl32 ---
$srcs = (@($dll) + $mhSrc | ForEach-Object { '"' + $_ + '"' }) -join ' '
cmd /c "`"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /LD /O2 /I `"$inc`" /Fe:ss_hook.dll $srcs opengl32.lib user32.lib winmm.lib"
Write-Host ("ss_hook.dll: " + (Test-Path (Join-Path $out 'ss_hook.dll')))

# --- 3. injector ---
cmd /c "`"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /O2 /Fe:inject.exe `"$inj`""
Write-Host ("inject.exe: " + (Test-Path (Join-Path $out 'inject.exe')))
