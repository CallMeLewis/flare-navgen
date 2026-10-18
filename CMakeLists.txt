cmake_minimum_required(VERSION 3.24)
project(dznav VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DZNAV_BUILD_TESTS "Build tests" ON)

# Static C++ runtime, so dznav.exe runs (and ships inside the desktop app) without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Recast & Detour, patched for DayZ's tile format.
add_subdirectory(third_party/recastnavigation)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(src/dznav)
add_subdirectory(src/cli)

if(DZNAV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
