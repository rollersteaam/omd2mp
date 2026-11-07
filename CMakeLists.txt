cmake_minimum_required(VERSION 3.16)
project(omd2mp C)

# The game is a 32-bit process, so the loader DLL must be built for Win32:
#   cmake -S . -B build/msvc -G "Visual Studio 16 2019" -A Win32
#   cmake --build build/msvc --config Release
#   ctest --test-dir build/msvc -C Release

add_library(patchcore STATIC loader/src/patchcore.c)
target_include_directories(patchcore PUBLIC loader/src)

add_executable(test_patchcore tests/c/test_patchcore.c)
target_link_libraries(test_patchcore patchcore)

enable_testing()
add_test(NAME patchcore COMMAND test_patchcore)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_library(omd2mp_loader SHARED loader/src/dllmain.c)
  target_link_libraries(omd2mp_loader patchcore)
  set_target_properties(omd2mp_loader PROPERTIES OUTPUT_NAME version
                        MSVC_RUNTIME_LIBRARY "MultiThreaded")     # static CRT: no redistributable needed
  set_property(TARGET patchcore test_patchcore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  message(STATUS "omd2mp: not a 32-bit MSVC build; only the unit tests are built")
endif()
