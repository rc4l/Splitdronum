# splitdronum test runner. Compiles core/ + tests/ with the VS2022 C++ toolchain + doctest,
# runs them, and gates line coverage of core/ at 100% via OpenCppCoverage.
# Fully portable: no hard-coded paths -- discovers the toolchain and works from any clone
# location (including paths with spaces).
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
$exe  = Join-Path $out  'tests.exe'
New-Item -ItemType Directory -Force $out | Out-Null

# --- locate the VS2022 C++ toolchain (vcvars64.bat) via vswhere ---
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Host 'ERROR: Visual Studio not found. Install VS 2022 with the "Desktop development with C++" workload.'
    exit 2
}
$vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsroot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Write-Host 'ERROR: the VC++ toolset is not installed in Visual Studio.'; exit 2 }

# --- compile (auto-discover sources; quote every path so spaces are safe) ---
$srcs  = @()
$srcs += Get-ChildItem (Join-Path $root 'core')  -Filter *.cpp -EA SilentlyContinue | ForEach-Object FullName
$srcs += Get-ChildItem (Join-Path $root 'tests') -Filter *.cpp -EA SilentlyContinue | ForEach-Object FullName
$quoted = ($srcs | ForEach-Object { '"' + $_ + '"' }) -join ' '
# cd into the build dir so .obj files land there (avoids a /Fo path that ends in '\').
cmd /c "`"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /EHsc /Zi /Od /Fe:`"$exe`" $quoted"
if (-not (Test-Path $exe)) { Write-Host 'ERROR: compile failed'; exit 1 }

# --- run tests ---
& $exe
$testExit = $LASTEXITCODE
Write-Host "tests exit code: $testExit"
if ($testExit -ne 0) { exit $testExit }

# --- coverage gate: 100% of core/ ---
$occ = (Get-Command OpenCppCoverage.exe -EA SilentlyContinue).Source
if (-not $occ) {
    $loc = Get-ItemProperty `
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', `
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*', `
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' -EA SilentlyContinue |
        Where-Object { $_.DisplayName -like '*OpenCppCoverage*' } |
        Select-Object -First 1 -ExpandProperty InstallLocation -EA SilentlyContinue
    if ($loc) { $cand = Join-Path $loc 'OpenCppCoverage.exe'; if (Test-Path $cand) { $occ = $cand } }
}
if (-not $occ) { $cand = Join-Path $env:ProgramFiles 'OpenCppCoverage\OpenCppCoverage.exe'; if (Test-Path $cand) { $occ = $cand } }
if (-not $occ) {
    Write-Host 'NOTE: OpenCppCoverage not found; ran tests only.'
    Write-Host '      For the coverage gate: winget install OpenCppCoverage.OpenCppCoverage'
    exit 0
}
& $occ --sources "$root\core" --export_type "cobertura:$out\cov.xml" --quiet -- $exe | Out-Null
[xml]$cov = Get-Content (Join-Path $out 'cov.xml')
$rate = [double]$cov.coverage.'line-rate'
Write-Host ("core line coverage: {0:P1}" -f $rate)
if ($rate -lt 1.0) { Write-Host 'COVERAGE GATE FAILED (core < 100%)'; exit 1 }
Write-Host 'COVERAGE GATE PASSED (core = 100%)'
exit 0
