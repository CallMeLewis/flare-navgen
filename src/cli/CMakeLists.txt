add_executable(dznav-cli main.cpp)
set_target_properties(dznav-cli PROPERTIES OUTPUT_NAME dznav)
target_link_libraries(dznav-cli PRIVATE dznav)
