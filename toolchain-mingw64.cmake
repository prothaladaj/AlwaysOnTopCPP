# CMake toolchain file for cross-compiling to Windows x86-64
# using the MinGW-w64 toolchain installed on Linux / WSL.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake .
#   cmake --build build

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Compiler binaries  (adjust prefix if your distro uses a different name)
set(CMAKE_C_COMPILER     x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER   x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER    x86_64-w64-mingw32-windres)
set(CMAKE_STRIP          x86_64-w64-mingw32-strip)

# Sysroot
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
