# Local splitscreen co-op in one window. Builds the DLL + host if needed, then runs N players:
# seat 0 = keyboard + mouse, seats 1-3 = XInput controllers. Close the window to stop everything.
#
#   ./play.ps1 -Players 4
#   ./play.ps1 -Players 2 -Iwad freedoom2.wad -GamePath C:\path\to\zandronum.exe
# Look-invert toggles (flip any axis that feels wrong for you, defaults are up=up / right=right):
#   ./play.ps1 -Players 4 -PadInvertY        # controller look up/down
#   ./play.ps1 -Players 4 -MouseInvertY -PadInvertX
param(
    [int]$Players = 2,
    [string]$GamePath = '',   # override the engine exe; blank = splitdronum's OWN private engine copy
    [string]$EngineSrc = '',  # dev override: a local engine build dir (folder containing zandronum.exe + .pdb) to copy instead of downloading
    [string]$EngineZip = '',  # offline override: a local engine .zip to extract instead of downloading
    [string]$EngineUrl = 'https://github.com/rc4l/Splitdronum/releases/latest/download/engine.zip',  # default first-run source: the prebuilt-engine GitHub Release
    [string]$Iwad = 'freedoom2.wad',
    [switch]$MouseInvertY,   # flip seat-0 mouse look up/down
    [switch]$PadInvertY,     # flip controller look up/down
    [switch]$PadInvertX,     # flip controller turn left/right
    [double]$RenderScale = 1.0,  # client render res = monitor * this. <1 = lower base res upscaled
                                 # (faster, softer HUD); >1 supersamples (crisper, heavier). 0.25-2.0.
    [int]$Fps = -1,              # client render fps cap: -1 = monitor refresh rate (default),
                                 # 0 = uncapped, >0 = that fixed cap. (Readback always tracks refresh.)
    [bool]$SmartScale = $true    # auto-lower each seat's render res by live player count (1->1.0,
)                                # 2->0.75, 3-4->0.5) on top of -RenderScale. -SmartScale:$false = fixed.
$root = $PSScriptRoot
if ($MouseInvertY) { $env:SS_MOUSE_INVY = '1' } else { Remove-Item Env:\SS_MOUSE_INVY -ErrorAction SilentlyContinue }
if ($PadInvertY)   { $env:SS_PAD_INVY   = '1' } else { Remove-Item Env:\SS_PAD_INVY   -ErrorAction SilentlyContinue }
if ($PadInvertX)   { $env:SS_PAD_INVX   = '1' } else { Remove-Item Env:\SS_PAD_INVX   -ErrorAction SilentlyContinue }
$env:SS_RENDER_SCALE = "$RenderScale"
if ($Fps -ge 0) { $env:SS_FPS = "$Fps" } else { $env:SS_FPS = 'auto' }
if ($SmartScale) { $env:SS_SMARTSCALE = '1' } else { $env:SS_SMARTSCALE = '0' }
# Private engine: splitdronum runs its OWN copy of the engine + gamedir, so other sessions sharing the
# upstream build can't change our config / data / instances and we can't change theirs. Copied once on
# first run; delete splitdronum\engine to refresh it from -EngineSrc.
if (-not $GamePath) {
    $engineDir = Join-Path $root 'engine'
    $GamePath  = Join-Path $engineDir 'zandronum.exe'
    if (-not (Test-Path $GamePath)) {
        # First run: acquire a PRIVATE engine copy under engine/ (so other tools rebuilding the upstream
        # engine can't corrupt ours). A local build dir wins (dev); else a local zip; else download the
        # prebuilt-engine GitHub Release. Delete engine/ to re-acquire.
        New-Item -ItemType Directory -Force $engineDir | Out-Null
        if ($EngineSrc -and (Test-Path (Join-Path $EngineSrc 'zandronum.exe'))) {
            Write-Host "first run: copying a private engine from '$EngineSrc'..."
            Copy-Item -Path (Join-Path $EngineSrc '*') -Destination $engineDir -Recurse -Force
        } else {
            $zip = $EngineZip
            if (-not $zip) {
                $zip = Join-Path ([IO.Path]::GetTempPath()) 'splitdronum-engine.zip'
                Write-Host "first run: downloading the prebuilt engine from $EngineUrl ..."
                try { $ProgressPreference = 'SilentlyContinue'; Invoke-WebRequest -Uri $EngineUrl -OutFile $zip -UseBasicParsing }
                catch { Write-Host "ERROR: couldn't download the engine ($($_.Exception.Message)). Build it yourself and pass -EngineSrc <dir>, supply -EngineZip <file>, or see the README."; exit 1 }
            }
            if (-not (Test-Path $zip)) { Write-Host "ERROR: engine zip not found at '$zip'."; exit 1 }
            Write-Host "extracting the engine into '$engineDir'..."
            Expand-Archive -Path $zip -DestinationPath $engineDir -Force
            if (-not $EngineZip) { Remove-Item $zip -EA SilentlyContinue }
        }
        if (-not (Test-Path $GamePath)) { Write-Host "ERROR: no zandronum.exe in the engine source -- the zip/folder is missing the engine."; exit 1 }
        $engineFresh = $true   # force a DLL rebuild so its baked-in engine offsets match THIS copy
    }
}
if (-not (Test-Path $GamePath)) { Write-Host "ERROR: zandronum.exe not found at '$GamePath'."; exit 1 }
$GamePath = (Resolve-Path $GamePath).Path
$gameDir  = Split-Path $GamePath
$dll = Join-Path $root 'build\ss_hook.dll'
$exe = Join-Path $root 'build\host.exe'
$globalSeatsCfg = Join-Path $root 'global_seats.cfg'   # global config applied to every seat (edit to taste)

# Rebuild the DLL when the engine was just copied (offsets must match it) or when it's missing. The DLL
# bakes in engine symbol RVAs; if the engine differs from what the DLL was built against, its hooks land
# on wrong addresses and input dies silently -- so a fresh private engine always forces a DLL rebuild.
if ($engineFresh -or -not (Test-Path $dll)) { & (Join-Path $root 'build-dll.ps1')  -GamePath $GamePath }
if (-not (Test-Path $exe)) { & (Join-Path $root 'build-host.ps1') }
if (-not (Test-Path $dll) -or -not (Test-Path $exe)) { Write-Host 'ERROR: build failed.'; exit 1 }

Write-Host "launching $Players-player splitscreen (seat 0 = kbd/mouse, seats 1+ = controllers)..."
# Launch the compositor detached in its own hidden console, so the only thing you see is the game
# window -- no terminal lingering in the background. host.exe owns that new console, so it self-hides
# as a backstop too. Close the game window to stop everything.
$argline = "$Players `"$dll`" `"$GamePath`" $Iwad `"$gameDir`" `"$globalSeatsCfg`""
Start-Process -FilePath $exe -ArgumentList $argline -WindowStyle Hidden
