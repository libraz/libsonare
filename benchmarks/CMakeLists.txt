add_executable(sonare_bench bench_cpp.cpp)

target_link_libraries(sonare_bench PRIVATE sonare_core)
target_compile_options(sonare_bench PRIVATE ${SONARE_WARNING_FLAGS})

target_include_directories(sonare_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

add_executable(sonare_filterbank_bench filterbank_bench.cpp)
target_link_libraries(sonare_filterbank_bench PRIVATE sonare_core)
target_compile_options(sonare_filterbank_bench PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_filterbank_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

add_executable(sonare_spectrum_power_bench spectrum_power_bench.cpp)
target_link_libraries(sonare_spectrum_power_bench PRIVATE sonare_core)
target_compile_options(sonare_spectrum_power_bench PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_spectrum_power_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

add_executable(sonare_transcendental_bench transcendental_bench.cpp)
target_link_libraries(sonare_transcendental_bench PRIVATE sonare_core)
target_compile_options(sonare_transcendental_bench PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_transcendental_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

add_executable(sonare_streaming_mel_chroma_bench streaming_mel_chroma_bench.cpp)
target_link_libraries(sonare_streaming_mel_chroma_bench PRIVATE sonare_core)
target_compile_options(sonare_streaming_mel_chroma_bench PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_streaming_mel_chroma_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

add_executable(sonare_log10_bench log10_bench.cpp)
target_link_libraries(sonare_log10_bench PRIVATE sonare_core)
target_compile_options(sonare_log10_bench PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_log10_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

if(BUILD_MASTERING)
  add_executable(sonare_mastering_isp_bench mastering_isp_bench.cpp)
  target_link_libraries(sonare_mastering_isp_bench PRIVATE sonare_mastering)
  target_compile_options(sonare_mastering_isp_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_mastering_isp_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/src
  )

  add_executable(sonare_mastering_support_bench mastering_support_bench.cpp)
  target_link_libraries(sonare_mastering_support_bench PRIVATE sonare_mastering)
  target_compile_options(sonare_mastering_support_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_mastering_support_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/src
  )

  add_executable(sonare_mastering_stereo_bench mastering_stereo_bench.cpp)
  target_link_libraries(sonare_mastering_stereo_bench PRIVATE sonare_mastering)
  target_compile_options(sonare_mastering_stereo_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_mastering_stereo_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/src
  )

  add_executable(sonare_eq_bench eq_bench.cpp)
  target_link_libraries(sonare_eq_bench PRIVATE sonare_mastering)
  target_compile_options(sonare_eq_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_eq_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/src
  )
endif()

if(BUILD_MIXING)
  add_executable(sonare_mixing_bench mixing_bench.cpp)
  target_link_libraries(sonare_mixing_bench PRIVATE sonare_mixing)
  target_compile_options(sonare_mixing_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_mixing_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/src
  )
endif()
