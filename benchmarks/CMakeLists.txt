add_executable(sonare_bench bench_cpp.cpp)

target_link_libraries(sonare_bench PRIVATE sonare_core sonare_rt)
target_compile_options(sonare_bench PRIVATE ${SONARE_WARNING_FLAGS})

target_include_directories(sonare_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

if(EMSCRIPTEN)
  # The same benchmark, built for WebAssembly, so the browser cost of the
  # threaded paths (HPSS, pYIN) can be stated rather than described. NODERAWFS
  # lets the binary open the fixture WAV by path under Node exactly as the
  # native build does, keeping the two runs comparable.
  target_link_options(sonare_bench PRIVATE
    -sNODERAWFS=1
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=536870912
    -sEXIT_RUNTIME=1
    -sSTACK_SIZE=8388608
  )
endif()

if(BUILD_ARRANGEMENT)
  add_executable(sonare_piano_voice_bench piano_voice_bench.cpp)
  target_link_libraries(sonare_piano_voice_bench PRIVATE sonare_midi)
  target_compile_options(sonare_piano_voice_bench PRIVATE ${SONARE_WARNING_FLAGS})
  target_include_directories(sonare_piano_voice_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/src
  )
endif()

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

add_executable(sonare_phase_vocoder_streaming_bench phase_vocoder_streaming_bench.cpp)
target_link_libraries(sonare_phase_vocoder_streaming_bench PRIVATE sonare_core)
target_compile_options(sonare_phase_vocoder_streaming_bench PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_phase_vocoder_streaming_bench PRIVATE
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
