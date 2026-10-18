# Vendored Recast & Detour (see UPSTREAM.txt). DayZ-specific changes are marked "DAYZ:".
file(GLOB RECAST_SOURCES Recast/Source/*.cpp)
add_library(Recast STATIC ${RECAST_SOURCES})
target_include_directories(Recast PUBLIC Recast/Include)

file(GLOB DETOUR_SOURCES Detour/Source/*.cpp)
add_library(Detour STATIC ${DETOUR_SOURCES})
target_include_directories(Detour PUBLIC Detour/Include)
# DayZ uses 64-bit poly refs (16-bit salt, 28-bit tile, 20-bit poly).
target_compile_definitions(Detour PUBLIC DT_POLYREF64)

if(MSVC)
  target_compile_options(Recast PRIVATE /W3)
  target_compile_options(Detour PRIVATE /W3)
endif()
