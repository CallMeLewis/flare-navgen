add_library(dznav STATIC
  Capture.cpp
  Compare.cpp
  DataServerClient.cpp
  Generator.cpp
  JumpLinks.cpp
  NavMeshAssembly.cpp
  NavMeshFormat.cpp
  Preview.cpp
  Raster.cpp
  ReplayServer.cpp
  TileBuilder.cpp
  WorldInput.cpp
)
if(WIN32)
  target_link_libraries(dznav PUBLIC ws2_32)
endif()
target_include_directories(dznav PUBLIC include)
target_link_libraries(dznav PUBLIC Recast Detour)
