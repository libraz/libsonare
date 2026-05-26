cmake_minimum_required(VERSION 3.16)
project(libsonare VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent <windows.h> (pulled in transitively by dr_libs on MSVC) from defining
# the min/max function-like macros that clobber std::min/std::max.
add_compile_definitions($<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_WASM "Build for WebAssembly" OFF)
option(BUILD_SHARED "Build shared library" OFF)
option(BUILD_CLI "Build CLI tool" ON)
option(BUILD_BENCH "Build C++ benchmark binary" OFF)
option(BUILD_MASTERING "Build the mastering library" ON)
option(BUILD_MIXING "Build the mixing library" ON)
option(BUILD_GRAPH "Build the routing graph library" ON)

# The mixer embeds mastering processors as channel-strip EQ/inserts.
if(BUILD_MIXING AND NOT BUILD_MASTERING)
  message(STATUS "libsonare: BUILD_MIXING requires BUILD_MASTERING; enabling BUILD_MASTERING")
  set(BUILD_MASTERING ON CACHE BOOL "Build the mastering library" FORCE)
endif()

# The C mixer (sonare_c_mixing.cpp) routes signal through the routing graph, so
# mixing requires the graph library. Auto-enable it to keep existing configs
# that only set BUILD_MIXING working.
if(BUILD_MIXING AND NOT BUILD_GRAPH)
  message(STATUS "libsonare: BUILD_MIXING requires BUILD_GRAPH; enabling BUILD_GRAPH")
  set(BUILD_GRAPH ON CACHE BOOL "Build the routing graph library" FORCE)
endif()
option(BUILD_FX "Build creative realtime FX processors" ON)
option(BUILD_PITCH_EDITOR "Build monophonic pitch editor primitives" ON)
option(BUILD_VOICE_CHANGER "Build realtime voice changer primitives" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
option(SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS
       "Build optional external fixture tests that require local datasets" OFF)
set(SONARE_WITH_FFMPEG "AUTO" CACHE STRING
    "FFmpeg-based decoding for M4A/AAC/FLAC/OGG: ON (require), OFF (disable), AUTO (detect via pkg-config, default)")
set_property(CACHE SONARE_WITH_FFMPEG PROPERTY STRINGS ON OFF AUTO)

# FFmpeg is not WASM-compatible and would bloat the bundle. Force OFF for WASM.
if(BUILD_WASM AND NOT SONARE_WITH_FFMPEG STREQUAL "OFF")
  if(SONARE_WITH_FFMPEG STREQUAL "ON")
    message(WARNING "SONARE_WITH_FFMPEG=ON is not supported with BUILD_WASM=ON; "
                    "forcing SONARE_WITH_FFMPEG=OFF.")
  endif()
  set(SONARE_WITH_FFMPEG "OFF" CACHE STRING "" FORCE)
endif()

# 出力ディレクトリ
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 警告設定 (サードパーティに波及させないためターゲット単位で適用)
set(SONARE_WARNING_FLAGS
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /wd4324 /external:W0>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

# Coverage settings
if(ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Enabling code coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -O0 -g")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0 -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
  else()
    message(WARNING "Code coverage is only supported with GCC or Clang")
  endif()
endif()

# Eigen3
find_package(Eigen3 3.3 QUIET NO_MODULE)
if(NOT Eigen3_FOUND)
  # Fallback: fetch Eigen3 (for WASM or when system Eigen is incompatible)
  include(FetchContent)
  FetchContent_Declare(
    Eigen3
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG 3.4.0
    GIT_SHALLOW TRUE
  )
  if(BUILD_WASM)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_GetProperties(Eigen3)
    if(NOT eigen3_POPULATED)
      FetchContent_Populate(Eigen3)
    endif()
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    target_include_directories(Eigen3::Eigen SYSTEM INTERFACE ${eigen3_SOURCE_DIR})
  else()
    FetchContent_MakeAvailable(Eigen3)
    # Mark the fetched Eigen headers as system includes so MSVC /W4 /WX (and
    # -Wall -Werror) do not error on warnings originating inside Eigen. A
    # find_package'd Eigen is already exported as a system include; only this
    # fetched fallback (used on Windows CI, which has no system Eigen) needs it.
    if(TARGET eigen)
      get_target_property(_sonare_eigen_inc eigen INTERFACE_INCLUDE_DIRECTORIES)
      if(_sonare_eigen_inc)
        set_target_properties(eigen PROPERTIES
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_sonare_eigen_inc}")
      endif()
    endif()
  endif()
endif()

# サブディレクトリ
add_subdirectory(src)

if(BUILD_TESTING AND NOT BUILD_WASM)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BUILD_CLI AND NOT BUILD_WASM)
  add_subdirectory(tools)
endif()

if(BUILD_BENCH)
  add_subdirectory(benchmarks)
endif()

# Coverage target
if(ENABLE_COVERAGE)
  find_program(LCOV_PATH lcov)
  find_program(GENHTML_PATH genhtml)
  if(LCOV_PATH AND GENHTML_PATH)
    add_custom_target(coverage
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
      COMMAND ${LCOV_PATH} --directory . --zerocounters
      COMMAND ctest --output-on-failure --parallel
      COMMAND ${LCOV_PATH} --directory . --capture --output-file ${CMAKE_BINARY_DIR}/coverage/coverage.info
      COMMAND ${LCOV_PATH} --extract ${CMAKE_BINARY_DIR}/coverage/coverage.info
        '${CMAKE_SOURCE_DIR}/src/*'
        --output-file ${CMAKE_BINARY_DIR}/coverage/coverage_filtered.info
      COMMAND ${GENHTML_PATH} ${CMAKE_BINARY_DIR}/coverage/coverage_filtered.info
        --output-directory ${CMAKE_BINARY_DIR}/coverage/html
      COMMAND ${CMAKE_COMMAND} -E echo "Coverage report generated: ${CMAKE_BINARY_DIR}/coverage/html/index.html"
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Generating code coverage report"
    )
    add_custom_target(coverage-clean
      COMMAND find . -name '*.gcda' -delete
      COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/coverage
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Cleaning coverage data"
    )
  else()
    message(WARNING "lcov and genhtml not found. Coverage report target will not be available.")
    message(WARNING "Install with: sudo apt-get install lcov (Linux)")
  endif()
endif()
