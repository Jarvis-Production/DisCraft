# Cross-compiling DisCraft.asi for 32-bit Windows from Linux (or MSYS2) with MinGW-w64:
#   cmake -S native -B build/native -DCMAKE_TOOLCHAIN_FILE=native/cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/native
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
