# CLI tool
add_executable(sonare
  sonare_cli.cpp
  cli_support.cpp
)

target_link_libraries(sonare PRIVATE sonare_core)
if(BUILD_MASTERING)
  target_link_libraries(sonare PRIVATE sonare_mastering)
  target_compile_definitions(sonare PRIVATE SONARE_WITH_MASTERING=1)
endif()
target_compile_options(sonare PRIVATE ${SONARE_WARNING_FLAGS})

target_include_directories(sonare PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

# Profiling build option
option(ENABLE_PROFILER "Enable gperftools profiler" OFF)
if(ENABLE_PROFILER)
  find_library(PROFILER_LIB profiler)
  if(PROFILER_LIB)
    target_link_libraries(sonare PRIVATE ${PROFILER_LIB})
    target_compile_definitions(sonare PRIVATE ENABLE_PROFILER)
  endif()
endif()

# Install
install(TARGETS sonare RUNTIME DESTINATION bin)
