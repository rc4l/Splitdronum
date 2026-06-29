#!/usr/bin/env bash
# Build the macOS capture dylib (ss_hook.dylib) -- the cross-platform counterpart of build-dll.ps1.
# Extracts engine symbol vmaddrs from the Intel zandronum binary (gen_offsets_mac -> dll/ss_offsets.h),
# then builds the injected dylib. SDL/GL symbols are left UNDEFINED (-undefined dynamic_lookup): they
# resolve at runtime from the engine process's own SDL2/OpenGL, which is exactly what the interposes hook.
#
#   ./build-dll.sh <path-to-zandronum-binary> [sdl2-include-dir]
#
# Needs: macOS, clang, nm, and SDL2 headers (brew install sdl2). Runtime also needs the engine + a Mac.
set -e
root="$(cd "$(dirname "$0")" && pwd)"
out="$root/build"; mkdir -p "$out"

engine="${1:?usage: build-dll.sh <path-to-zandronum-binary> [sdl2-include-dir]}"
sdlinc="${2:-$(brew --prefix sdl2 2>/dev/null)/include/SDL2}"
if [ ! -f "$sdlinc/SDL.h" ]; then
  echo "ERROR: SDL.h not found under '$sdlinc'. Install SDL2 (brew install sdl2) or pass its include dir."; exit 2
fi

# 1. build the offset tool and extract the engine symbols the hook resolves -> dll/ss_offsets.h
clang++ -std=c++17 -O2 -o "$out/gen_offsets_mac" "$root/tools/gen_offsets_mac.cpp"
"$out/gen_offsets_mac" "$engine" "$root/dll/ss_offsets.h" \
    D_PostEvent menuactive ConsoleState AppActive I_GetAxes GUICapture g_ulChatMode AddCommandString

# 2. the dylib. -arch x86_64 to match the Intel engine; flat dynamic lookup so SDL_* resolve from it.
clang++ -std=c++17 -O2 -arch x86_64 -dynamiclib -fPIC \
    -I"$sdlinc" -I"$root/dll" -I"$root/host" \
    -framework OpenGL -undefined dynamic_lookup \
    -o "$out/ss_hook.dylib" "$root/dll/ss_hook_mac.cpp"
echo "ss_hook.dylib: $(test -f "$out/ss_hook.dylib" && echo OK || echo MISSING)"
