add_executable(sonare_bench bench_cpp.cpp)

target_link_libraries(sonare_bench PRIVATE sonare_core)
target_compile_options(sonare_bench PRIVATE ${SONARE_WARNING_FLAGS})

target_include_directories(sonare_bench PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)
