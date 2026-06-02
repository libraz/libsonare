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
if(BUILD_MIXING)
  target_link_libraries(sonare PRIVATE sonare_mixing)
  target_compile_definitions(sonare PRIVATE SONARE_WITH_MIXING=1)
endif()
if(BUILD_PITCH_EDITOR)
  target_link_libraries(sonare PRIVATE sonare_pitch_editor)
  target_compile_definitions(sonare PRIVATE SONARE_WITH_PITCH_EDITOR=1)
endif()
if(BUILD_VOICE_CHANGER)
  target_link_libraries(sonare PRIVATE sonare_voice_changer)
  target_compile_definitions(sonare PRIVATE SONARE_WITH_VOICE_CHANGER=1)
endif()
if(BUILD_ACOUSTIC_SIM AND BUILD_FX)
  # RIR synthesis + room estimation (acoustic) and the offline morph (fx).
  target_link_libraries(sonare PRIVATE sonare_acoustic sonare_fx)
  target_compile_definitions(sonare PRIVATE SONARE_WITH_ACOUSTIC_SIM=1)
endif()
target_compile_options(sonare PRIVATE ${SONARE_WARNING_FLAGS})

target_include_directories(sonare PRIVATE
  ${PROJECT_SOURCE_DIR}/src
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
