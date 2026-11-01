# SPDX-License-Identifier: MIT
#
# CMake toolchain for the WebAssembly replay runners (docs/trace-replay.md,
# "Replay in the browser"). It cross-builds host/CMakeLists.txt for
# wasm32-wasip1 with the clang and wasi-libc that devenv pins through
# devenv.lock: ZPT_WASI_CC names a clang wrapper that targets wasm32-wasip1
# with wasi-libc and wasm-ld (devenv.nix sets it).

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

if(NOT DEFINED ENV{ZPT_WASI_CC})
  message(FATAL_ERROR "ZPT_WASI_CC is not set; run this build in the devenv shell")
endif()

set(CMAKE_C_COMPILER "$ENV{ZPT_WASI_CC}")
set(CMAKE_EXECUTABLE_SUFFIX_C ".wasm")
# The runners are WASI commands: _start reads stdin, writes stdout, and
# exits. Size matters more than speed for a page download; at -Os a browser
# still replays the whole benchmark tune set in tens of milliseconds.
set(CMAKE_C_FLAGS_INIT "-Os")
# --stack-first puts the 64 KiB stack below the data, so an overflow traps
# at address 0 instead of silently overwriting static state.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--stack-first -Wl,--strip-debug")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
