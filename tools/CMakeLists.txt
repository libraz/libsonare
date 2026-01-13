# CLI tool
add_executable(sonare sonare_cli.cpp)

target_link_libraries(sonare PRIVATE sonare_core)

target_include_directories(sonare PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

# Install
install(TARGETS sonare RUNTIME DESTINATION bin)
