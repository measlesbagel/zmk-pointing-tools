# SPDX-License-Identifier: MIT
#
# CMake toolchain for the WebAssembly replay runners (docs/trace-replay.md,
# "Replay in the browser"). It cross-builds host/CMakeLists.txt for
# wasm32-wasip1 with the clang and wasi-libc that devenv pins through
# devenv.lock: ZPT_WASI_CC names the compiler and ZPT_WASI_LD_DIR the
# directory holding its wasm-ld (devenv.nix sets both).

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

foreach(variable ZPT_WASI_CC ZPT_WASI_LD_DIR)
  if(NOT DEFINED ENV{${variable}})
    message(FATAL_ERROR "${variable} is not set; run this build in the devenv shell")
  endif()
endforeach()

set(CMAKE_C_COMPILER "$ENV{ZPT_WASI_CC}")
set(CMAKE_EXECUTABLE_SUFFIX_C ".wasm")
# The runners are WASI commands: _start reads stdin, writes stdout, and
# exits. Size matters more than speed for a page download; at -Os a browser
# still replays the whole benchmark tune set in tens of milliseconds.
set(CMAKE_C_FLAGS_INIT "-Os")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-B$ENV{ZPT_WASI_LD_DIR} -Wl,--strip-debug")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
