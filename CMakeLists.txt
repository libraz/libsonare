cmake_minimum_required(VERSION 3.16)
project(libsonare VERSION 1.0.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_WASM "Build for WebAssembly" OFF)
option(BUILD_SHARED "Build shared library" OFF)
option(BUILD_CLI "Build CLI tool" ON)
option(BUILD_BENCH "Build C++ benchmark binary" OFF)
option(BUILD_MASTERING "Build the mastering library" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
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
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
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
  FetchContent_MakeAvailable(Eigen3)
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
