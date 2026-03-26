cmake_minimum_required(VERSION 3.16)
project(libsonare VERSION 1.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_WASM "Build for WebAssembly" OFF)
option(BUILD_SHARED "Build shared library" OFF)
option(BUILD_CLI "Build CLI tool" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)

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
