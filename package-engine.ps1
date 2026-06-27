# package-engine.ps1 -- bundle a *built* Zandronum engine into engine.zip, the asset you upload to a
# GitHub Release on Splitdronum. play.ps1 downloads that zip on a user's first run, so end users never
# have to build the engine themselves. Run this on a machine that already has a built engine (the
# zandronum-windows build\Release output, or any Zandronum build that includes a .pdb).
#
#   ./package-engine.ps1 -From ..\zandronum-windows\build\Release
#   -> creates engine.zip; upload it to a Release on github.com/rc4l/Splitdronum named 'engine.zip'.
param(
    [string]$From = '',                                       # build dir containing zandronum.exe + .pdb (required)
    [string]$Out  = (Join-Path $PSScriptRoot 'engine.zip')
)
if (-not $From) { Write-Host "usage: ./package-engine.ps1 -From <build dir with zandronum.exe + .pdb> [-Out engine.zip]"; exit 1 }
if (-not (Test-Path (Join-Path $From 'zandronum.exe'))) { Write-Host "ERROR: no zandronum.exe in '$From'."; exit 1 }
if (-not (Test-Path (Join-Path $From 'zandronum.pdb'))) {
    Write-Host "WARNING: no zandronum.pdb in '$From'. splitdronum's DLL build resolves engine symbols from the"
    Write-Host "         .pdb, so users would need a prebuilt ss_hook.dll without it. Build the engine with symbols."
}
# The runtime splitdronum needs: the exe, its debug symbols, the resource pk3s, FMOD dlls, and an IWAD.
# Skip logs / per-seat .ini+.cfg / other runtime cruft so the asset stays clean and reproducible.
$include = @('*.exe', '*.pdb', '*.pk3', '*.dll', '*.wad')
$files = Get-ChildItem -Path $From -File | Where-Object { $n = $_.Name; ($include | Where-Object { $n -like $_ }).Count -gt 0 }
if (-not $files) { Write-Host "ERROR: nothing to package in '$From'."; exit 1 }
if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path $files.FullName -DestinationPath $Out -CompressionLevel Optimal
Write-Host ""
Write-Host ("packaged {0} files -> {1} ({2} MB)" -f $files.Count, $Out, [math]::Round((Get-Item $Out).Length / 1MB, 1))
$files | Sort-Object Name | ForEach-Object { Write-Host ("  + " + $_.Name) }
Write-Host ""
Write-Host "Next: create a Release on github.com/rc4l/Splitdronum and upload this file as 'engine.zip'."
Write-Host "play.ps1 fetches it from <repo>/releases/latest/download/engine.zip on a user's first run."
