# Host-shell wrapper — architecture & migration

## Goal (the decision)

Be a **wrapper, not a fork** — RuneLite/Dalamud style. Instead of editing Zandronum's
source (which we drifted into: ~6 files patched, a recurring upstream-merge cost), we
**inject a DLL into the stock `zandronum.exe` and hook the functions we need at runtime.**
The shipped binary stays pristine → zero merge cost, full interop with vanilla/modded
servers, and we can eventually use the *official* prebuilt binary with no custom build.

Everything that is real *logic* lives in pure modules with **100% test coverage**. The
unavoidable glue (Win32/GL hooks, DLL injection) stays thin and is excluded from the gate.

## Layout

```
wrapper/
  core/         pure logic, NO Win32/GL/engine headers  -> 100% covered
  dll/          the injected DLL: runtime hooks (glue, excluded from coverage)
  host/         launcher (injects the DLL into stock clients) + compositor
  tests/        doctest unit tests
  third_party/  doctest.h  (+ MinHook later)
  build-tests.ps1   compile core+tests with cl.exe, run, gate 100% via OpenCppCoverage
```

## Test gate

`build-tests.ps1` compiles `core` + `tests` with the VS2022 toolchain and doctest, runs them,
then measures line coverage of `core` with OpenCppCoverage. **The gate fails the build if
`core` is below 100%.** Run it after every `core` change.

**What belongs in `core` (and thus the gate):** genuine logic only -- algorithms (layout /
aspect math) and bug-prone conventions (look sign / invert / deadzone / sensitivity).
Maintainability wins over the coverage number: do NOT pad `core` with trivial wrappers (a
constant->constant map, a one-line boolean) just to cover them -- that breeds duplication.
That kind of glue lives with the platform/engine code using the real headers, and stays out
of the gate.

## What the DLL hooks replace (current engine edits -> runtime hooks)

| Current source edit | Runtime hook in the DLL |
|---|---|
| `hs_fbshare` publish framebuffer | hook `wglSwapBuffers` -> glReadPixels -> shared mem |
| `hs_input` apply buttons/look + key events | hook input poll / `D_PostEvent` (EV_KeyDown + EV_GUI) |
| `vid_renderwhileinactive` (CanUpdate) | hook the active-check so it renders while background |
| window-hide gates (`i_main`,`win32gliface`) | hook `ShowWindow` (or hide the window externally) |
| `hs_setres` arbitrary resolution | drive the existing resize path via the hook |
| menu input gate | **host decides** (core logic); DLL just exposes `menuactive` |

## Phases

- **Phase 0 — DONE.** Skeleton + doctest + OpenCppCoverage gate; first pure module
  `ss_layout` (ComputeLayout / Letterbox / TargetSize) at 100%.
- **Phase 1.** Move all remaining logic into `core/` with 100% tests:
  input mapping (VK->scancode, VK->GK GUI codes, button bitmask, stick->look),
  mouse-delta processing (recenter/clamp/skip, X/Y sensitivity), the shared-memory
  protocol structs, and the "is this game input or menu/console input" decision.
- **Phase 2 — DONE.** Vendored MinHook; `dll/ss_hook.cpp` hooks `wglSwapBuffers` on a stock
  `zandronum.exe` (injected by `host/inject.cpp`) and publishes the GL back buffer to
  `ZanDLLFB_<pid>`. Verified: advancing frames, real pixels, the actual game image, no engine
  source edits. (glReadPixels is bottom-up + BGR; the DLL will flip to top-down before
  publishing in Phase 3 so the host gets the format the old path used.)
- **Phase 3 — input DONE (build-time symbols).** Engine symbols resolve as
  (exe module base + RVA); the RVAs are extracted from the built `zandronum.exe` at DLL-build
  time by `tools/gen_offsets.cpp` -> `dll/ss_offsets.h`. This replaced **runtime DbgHelp**,
  which raced the engine's own DbgHelp use and resolved only intermittently (~1 in 4 launches);
  build-time offsets are deterministic + instant, and the build fails loud if a symbol is gone.
  The per-frame hook reads `ZanIN_<pid>` and posts engine events via `D_PostEvent`: `EV_Mouse`
  (look — engine applies the player's own sensitivity/invert), `EV_KeyDown/Up` (gameplay binds:
  move/weapons/fire), and `EV_GUI_*` (menu/console). Keyboard is routed by the resolved
  `menuactive` global (UI keys vs gameplay binds) — which **natively fixes the old escape-close
  bug** and stops gameplay input bleeding into menus. Verified on a stock binary: move, look,
  fire, menu open+close.
- **Phase 4 — host (the 4-player milestone).** A C++ compositor launches a loopback server + N
  stock clients with the DLL injected, composites each `ZanDLLFB` via `core/`, and routes
  captured keyboard/mouse/pad input into each `ZanIN`. Needs the DLL self-sufficiency hooks
  first: render-while-inactive, window-hide (`ShowWindow`), and per-pane resolution.
- **Phase 5.** Revert every engine source edit -> pristine Zandronum; verify the DLL
  reproduces all behavior. From here the engine tree is untouched upstream.

## Status

Proven end to end on **one** stock client: framebuffer out + full input (move / look / fire /
menu open+close) in, engine source untouched, symbol resolution build-time and reliable. The
test gate is green (`core` 100% = layout math). Next: the DLL self-sufficiency hooks
(render-while-inactive, window-hide, per-pane resolution), then the C++ host (launch + inject N
clients, composite via `core/`, route input, join) — the **4-player milestone** — then revert
the engine edits to pristine. The 6 engine-edited files in the Zandronum tree stay in place and
working until the DLL fully reproduces them; they get reverted last, not before.
