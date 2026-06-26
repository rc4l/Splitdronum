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
    [string]$GamePath = (Join-Path $PSScriptRoot '..\zandronum-vscode-windows\build\Release\zandronum.exe'),
    [string]$Iwad = 'freedoom2.wad',
    [switch]$MouseInvertY,   # flip seat-0 mouse look up/down
    [switch]$PadInvertY,     # flip controller look up/down
    [switch]$PadInvertX,     # flip controller turn left/right
    [double]$RenderScale = 1.0,  # client render res = monitor * this. <1 = lower base res upscaled
                                 # (faster, softer HUD); >1 supersamples (crisper, heavier). 0.25-2.0.
    [int]$Fps = -1               # client render fps cap: -1 = monitor refresh rate (default),
)                                # 0 = uncapped, >0 = that fixed cap. (Readback always tracks refresh.)
$root = $PSScriptRoot
if ($MouseInvertY) { $env:SS_MOUSE_INVY = '1' } else { Remove-Item Env:\SS_MOUSE_INVY -ErrorAction SilentlyContinue }
if ($PadInvertY)   { $env:SS_PAD_INVY   = '1' } else { Remove-Item Env:\SS_PAD_INVY   -ErrorAction SilentlyContinue }
if ($PadInvertX)   { $env:SS_PAD_INVX   = '1' } else { Remove-Item Env:\SS_PAD_INVX   -ErrorAction SilentlyContinue }
$env:SS_RENDER_SCALE = "$RenderScale"
if ($Fps -ge 0) { $env:SS_FPS = "$Fps" } else { $env:SS_FPS = 'auto' }
if (-not (Test-Path $GamePath)) { Write-Host "ERROR: zandronum.exe not found at '$GamePath' -- build the engine, or pass -GamePath."; exit 1 }
$GamePath = (Resolve-Path $GamePath).Path
$gameDir  = Split-Path $GamePath
$dll = Join-Path $root 'build\ss_hook.dll'
$exe = Join-Path $root 'build\host.exe'
$globalSeatsCfg = Join-Path $root 'global_seats.cfg'   # global config applied to every seat (edit to taste)

if (-not (Test-Path $dll)) { & (Join-Path $root 'build-dll.ps1')  -GamePath $GamePath }
if (-not (Test-Path $exe)) { & (Join-Path $root 'build-host.ps1') }
if (-not (Test-Path $dll) -or -not (Test-Path $exe)) { Write-Host 'ERROR: build failed.'; exit 1 }

Write-Host "launching $Players-player splitscreen (seat 0 = kbd/mouse, seats 1+ = controllers)..."
# Launch the compositor detached in its own hidden console, so the only thing you see is the game
# window -- no terminal lingering in the background. host.exe owns that new console, so it self-hides
# as a backstop too. Close the game window to stop everything.
$argline = "$Players `"$dll`" `"$GamePath`" $Iwad `"$gameDir`" `"$globalSeatsCfg`""
Start-Process -FilePath $exe -ArgumentList $argline -WindowStyle Hidden
