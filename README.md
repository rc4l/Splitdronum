<p align="center">
  <img src="assets/logo.png" alt="splitdronum logo" width="200">
</p>

# splitdronum

Local-splitscreen wrapper for Zandronum

## Requirements

- **Windows 10/11** with Visual Studio 2022 Build Tools (the scripts auto-discover the toolchain via `vswhere` — no hardcoded paths). Used to compile splitdronum's small host + injected DLL on first run.
- **No engine build needed.** A prebuilt Zandronum engine is downloaded automatically on first run.

## Run

```powershell
git clone https://github.com/rc4l/Splitdronum.git
cd Splitdronum
./play.ps1 -Players 2      # seat 0 = keyboard/mouse, seats 1+ = controllers
```

The first run downloads a prebuilt engine into a **private** `engine/` folder (isolated so other tools can't corrupt it) and compiles the host + DLL. Later runs are instant. Design notes: [ARCHITECTURE.md](ARCHITECTURE.md).

**Using your own engine instead of the download:**

- `./play.ps1 -EngineZip path\to\engine.zip` — install from a local engine zip (offline).
- `./play.ps1 -EngineSrc path\to\build` — copy from a local Zandronum build folder (must contain `zandronum.exe` **and** `zandronum.pdb`).

## Maintainer: publishing the engine

splitdronum ships the engine as a GitHub **Release asset** (`engine.zip`) so users never build it. The injected DLL resolves engine functions from the engine's `.pdb`, so the asset must include symbols. To create / refresh it:

1. Build the engine — clone and build [`zandronum-windows`](https://github.com/rc4l/zandronum-windows) (its `build.ps1` produces `build\Release\zandronum.exe` + `.pdb`).
2. `./package-engine.ps1 -From ..\zandronum-windows\build\Release` → produces `engine.zip` (exe + pdb + pk3s + FMOD dlls + IWAD only; runtime cruft excluded).
3. Upload `engine.zip` to a Release on this repo. `play.ps1` fetches it from `releases/latest/download/engine.zip`.
