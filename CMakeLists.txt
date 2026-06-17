cmake_minimum_required(VERSION 3.16)
project(libsonare VERSION 1.3.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent <windows.h> (pulled in transitively by dr_libs on MSVC) from defining
# the min/max function-like macros that clobber std::min/std::max. Also silence
# MSVC's C4996 deprecation of standard C functions (e.g. strncpy in the C API),
# which are used correctly and portably.
add_compile_definitions(
  $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_WASM "Build for WebAssembly" OFF)
option(BUILD_SHARED "Build shared library" OFF)
option(BUILD_CLI "Build CLI tool" ON)
option(BUILD_BENCH "Build C++ benchmark binary" OFF)
option(BUILD_MASTERING "Build the mastering library" ON)
option(BUILD_MIXING "Build the mixing library" ON)
option(BUILD_GRAPH "Build the routing graph library" ON)

option(BUILD_FX "Build creative realtime FX processors" ON)
option(BUILD_ACOUSTIC_SIM "Build geometric room-acoustics simulation (5th reverb engine)" ON)
option(BUILD_PITCH_EDITOR "Build monophonic pitch editor primitives" ON)
option(BUILD_VOICE_CHANGER "Build realtime voice changer primitives" ON)
option(BUILD_ARRANGEMENT "Build headless arrangement / DAW project subsystem" ON)
option(BUILD_ASSIST "Build composition-assist injection seam (control/offline only)" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
option(SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS
       "Build optional external fixture tests that require local datasets" OFF)

# macOS host backends — concrete implementations of the sonare::host seams that
# drive a real device/plugin. They include OS/SDK headers, so they are kept out
# of core (invariant 6), default OFF, and excluded from the default distribution
# and every language binding. macOS-only; ignored on other platforms.
option(BUILD_COREAUDIO "macOS CoreAudio host backend (host::AudioDevice)" OFF)
option(BUILD_COREMIDI  "macOS CoreMIDI host backend (host::MidiInputSource/Sink)" OFF)
option(BUILD_AU_HOST   "macOS Audio Unit plugin host (host::InstrumentProvider)" OFF)
option(BUILD_VST3_HOST "VST3 plugin host (requires the VST3 SDK)" OFF)

if(BUILD_MIXING AND NOT BUILD_MASTERING)
  message(STATUS "libsonare: BUILD_MIXING requires BUILD_MASTERING; enabling BUILD_MASTERING")
  set(BUILD_MASTERING ON CACHE BOOL "Build the mastering library" FORCE)
endif()

if(BUILD_MIXING AND NOT BUILD_GRAPH)
  message(STATUS "libsonare: BUILD_MIXING requires BUILD_GRAPH; enabling BUILD_GRAPH")
  set(BUILD_GRAPH ON CACHE BOOL "Build the routing graph library" FORCE)
endif()

if(NOT BUILD_PITCH_EDITOR)
  message(STATUS "libsonare: C editing API requires BUILD_PITCH_EDITOR; enabling BUILD_PITCH_EDITOR")
  set(BUILD_PITCH_EDITOR ON CACHE BOOL "Build monophonic pitch editor primitives" FORCE)
endif()

if(BUILD_VOICE_CHANGER AND NOT BUILD_MASTERING)
  message(STATUS "libsonare: BUILD_VOICE_CHANGER requires BUILD_MASTERING; disabling BUILD_VOICE_CHANGER")
  set(BUILD_VOICE_CHANGER OFF CACHE BOOL "Build realtime voice changer primitives" FORCE)
endif()
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
# MSVC /W4 suppressions, applied so MSVC matches the warning bar the primary
# GCC/Clang build enforces (-Wall -Wextra, which intentionally does NOT include
# -Wconversion): /wd4244 /wd4267 /wd4305 silence double->float / size_t->int /
# literal narrowing, which this audio library performs deliberately and which
# GCC/Clang do not flag; /wd4324 is the intentional alignas() padding warning.
set(SONARE_WARNING_FLAGS
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /wd4244 /wd4267 /wd4305 /wd4324 /external:W0>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
  # GCC's -Wmaybe-uninitialized produces well-known false positives on
  # std::variant move/construction once optimisations are on (e.g. our
  # util::json::Value in -O2/Release builds). Keep the warning visible but do
  # not let it fail the build; Clang/MSVC are unaffected and still enforce it.
  $<$<CXX_COMPILER_ID:GNU>:-Wno-error=maybe-uninitialized>
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
  # Populate Eigen's headers only; do NOT run its CMake project. Eigen is
  # header-only for our use, and add_subdirectory()/MakeAvailable would register
  # Eigen's own ~880 unit tests into our CTest (they fail as "Unable to find
  # executable" because we never build them) and pull in its targets. A bare
  # populate + manual INTERFACE target avoids that on every platform that lacks
  # a system Eigen (Windows and macOS CI, WASM). SYSTEM marks the headers so
  # MSVC /W4 /WX and -Wall -Werror do not error on warnings inside Eigen.
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
endif()

# サブディレクトリ
add_subdirectory(src)

# Host backends depend on sonare_engine/midi (defined under src) and on system
# frameworks, so they are wired after src and only when explicitly enabled on a
# macOS host. They are leaf targets: core links never reverse-depend on them.
if(BUILD_COREAUDIO OR BUILD_COREMIDI OR BUILD_AU_HOST OR BUILD_VST3_HOST)
  if(APPLE)
    add_subdirectory(src/host/backends)
  else()
    message(STATUS "libsonare: host backends (CoreAudio/CoreMIDI/AU/VST3) are macOS-only; ignoring on this platform")
  endif()
endif()

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
        '${PROJECT_SOURCE_DIR}/src/*'
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
