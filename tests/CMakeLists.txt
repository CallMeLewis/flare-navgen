add_executable(dznav-tests format_tests.cpp)
target_link_libraries(dznav-tests PRIVATE dznav)
add_test(NAME format COMMAND dznav-tests)

# Relinks shipped tiles with our patched Detour and compares bytes with the file.
add_executable(relink relink.cpp)
target_link_libraries(relink PRIVATE dznav)

# Rebuilds shipped tiles from their reconstructed poly meshes with our dtCreateNavMeshData.
add_executable(rebuild rebuild.cpp)
target_link_libraries(rebuild PRIVATE dznav)

# Regenerate a captured area and compare with a reference navmesh of the same tiles:
#   DZNAV_FIXTURE_CAPTURE=<file.dzgeo> DZNAV_FIXTURE_REFERENCE=<reference.nm> [DZNAV_FIXTURE_SEA_LEVEL=0.75]
if(DEFINED ENV{DZNAV_FIXTURE_CAPTURE} AND DEFINED ENV{DZNAV_FIXTURE_REFERENCE})
  set(_sea "")
  if(DEFINED ENV{DZNAV_FIXTURE_SEA_LEVEL})
    set(_sea --sea-level $ENV{DZNAV_FIXTURE_SEA_LEVEL})
  endif()
  add_test(NAME fixture_generate
           COMMAND dznav-cli generate ${CMAKE_CURRENT_BINARY_DIR}/fixture.nm --capture $ENV{DZNAV_FIXTURE_CAPTURE} ${_sea})
  add_test(NAME fixture_verify
           COMMAND dznav-cli verify $ENV{DZNAV_FIXTURE_REFERENCE} ${CMAKE_CURRENT_BINARY_DIR}/fixture.nm)
  set_tests_properties(fixture_verify PROPERTIES DEPENDS fixture_generate)
endif()

# Round-trip shipped navmeshes when DZNAV_SAMPLE_NM points at a directory of .nm files.
if(DEFINED ENV{DZNAV_SAMPLE_NM})
  file(GLOB _samples "$ENV{DZNAV_SAMPLE_NM}/*.nm")
  foreach(_nm ${_samples})
    get_filename_component(_name ${_nm} NAME_WE)
    add_test(NAME roundtrip_${_name} COMMAND dznav-cli roundtrip ${_nm})
    add_test(NAME relink_${_name} COMMAND relink ${_nm})
  endforeach()
endif()
