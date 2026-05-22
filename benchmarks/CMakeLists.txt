add_executable(sonare_bench bench_cpp.cpp)

target_link_libraries(sonare_bench PRIVATE sonare_core)
target_compile_options(sonare_bench PRIVATE ${SONARE_WARNING_FLAGS})

target_include_directories(sonare_bench PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

if(BUILD_MASTERING)
  add_executable(sonare_mastering_isp_bench mastering_isp_bench.cpp)
  target_link_libraries(sonare_mastering_isp_bench PRIVATE sonare_mastering)
  target_compile_options(sonare_mastering_isp_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_mastering_isp_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/src
  )

  add_executable(sonare_mastering_support_bench mastering_support_bench.cpp)
  target_link_libraries(sonare_mastering_support_bench PRIVATE sonare_mastering)
  target_compile_options(sonare_mastering_support_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_mastering_support_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/src
  )
endif()
