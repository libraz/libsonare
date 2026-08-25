cmake_minimum_required(VERSION 3.16)
project(libsonare VERSION 1.8.0 LANGUAGES C CXX)

if(WIN32)
  message(FATAL_ERROR
    "Windows is not a supported platform. Supported: Linux, macOS, WebAssembly. "
    "On Windows, build under WSL2.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Needed before add_subdirectory(src): the exported targets' install-interface
# include paths are written against CMAKE_INSTALL_INCLUDEDIR as the targets are
# created.
include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

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
option(SONARE_WASM_ANALYSIS_ONLY
       "Build only analysis and feature APIs into the WebAssembly entry" OFF)
set(SONARE_WASM_OUTPUT_NAME "sonare" CACHE STRING
    "Output basename for the WebAssembly JavaScript and binary files")
option(BUILD_SHARED "Build shared library" OFF)
option(BUILD_CLI "Build CLI tool" ON)
option(BUILD_BENCH "Build C++ benchmark binary" OFF)
option(BUILD_MASTERING "Build the mastering library" ON)
option(BUILD_MIXING "Build the mixing library" ON)
# Offline mixing assistant. Analyses tracks and returns a suggested scene; it
# never processes audio and nothing in the mixing runtime depends on it, so it
# can be dropped without touching the rest of the library.
option(BUILD_MIXING_ASSISTANT "Build the offline mixing assistant" ON)
option(BUILD_GRAPH "Build the routing graph library" ON)

option(BUILD_FX "Build creative realtime FX processors" ON)
option(BUILD_ACOUSTIC_SIM "Build geometric room-acoustics simulation (5th reverb engine)" ON)
option(BUILD_PITCH_EDITOR "Build monophonic pitch editor primitives" ON)
option(BUILD_VOICE_CHANGER "Build realtime voice changer primitives" ON)
option(BUILD_ARRANGEMENT "Build headless arrangement / DAW project subsystem" ON)
option(BUILD_ASSIST "Build composition-assist injection seam (control/offline only)" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)

# Install/export rules describe a standalone build of this project. When
# libsonare is pulled into someone else's build with add_subdirectory() -- the
# Node addon, the C++ example, a downstream application -- the parent decides
# what its `install` step contains, so the default follows whether this is the
# top-level project. The WebAssembly configuration produces an embind module,
# not a distributable C++ library, so it never generates install rules.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT BUILD_WASM)
  set(SONARE_INSTALL_DEFAULT ON)
else()
  set(SONARE_INSTALL_DEFAULT OFF)
endif()
option(SONARE_INSTALL "Generate install and find_package() export rules" ${SONARE_INSTALL_DEFAULT})
option(SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS
       "Build optional external fixture tests that require local datasets" OFF)

# FFT backend. PFFFT is a SIMD FFT and is 2-4x faster than the scalar KissFFT
# for the power-of-two sizes the STFT uses; KissFFT stays linked and serves every
# transform length PFFFT cannot factor.
#
# The WebAssembly configuration is off by default, and it stays off because the
# trade was measured rather than because it is untried. Three things decide it.
#
# The flag first: `-msimd128` is applied target by target to the sonare_*
# libraries below and the vendored `pffft` target is not among them. PFFFT
# selects its kernels from the compiler's own target macros, so without that
# flag `__wasm_simd128__` is undefined, `pffft_simd_size()` reports 1 instead of
# 4, and what gets compiled is the scalar path -- a second FFT that is not the
# one worth carrying, and one that also shifts which lengths PFFFT serves, since
# fft.cpp derives its granularity from that same value. Enabling the option here
# means giving the `pffft` target the flag in the same change.
#
# Size, built with the flag and compared against bindings/wasm/wasm-size-budget.json:
# the full module takes +0.47% raw / +0.49% gzip and fits inside the 2% the gate
# allows, while the analysis-only module takes +2.55% raw / +2.70% gzip and does
# not. Turning it on therefore also means re-recording that budget.
#
# Speed does not carry over from the native measurement and has to be taken on
# WebAssembly. The phase-vocoder streaming benchmark (n_fft 2048, hop 512,
# 48 kHz) moves from 11.70 to 10.73 ms per second of audio, -8.3%, well short of
# the native STFT figure because that pipeline spends most of its time on phase
# accumulation and overlap-add rather than in the transform. A transform-bound
# workload should gain more; none is measured here, so that is a reason to
# re-measure rather than a number to rely on.
#
# -8.3% on one pipeline does not pay for a budget rebase on the entry whose
# whole point is being small, so the WebAssembly build keeps KissFFT alone.
if(BUILD_WASM)
  set(SONARE_PFFFT_DEFAULT OFF)
else()
  set(SONARE_PFFFT_DEFAULT ON)
endif()
option(SONARE_USE_PFFFT "Use the SIMD PFFFT backend where the transform length allows"
       ${SONARE_PFFFT_DEFAULT})

# Development-only: expose the synth voices' `SONARE_TUNABLE` calibration
# constants as runtime-overridable knobs (see src/util/tunable.h), so a fitter
# can sweep one without a rebuild per candidate value. The macro changes what a
# declaration means, so the definition must reach every target uniformly or two
# translation units would disagree on the same constant's storage.
option(BUILD_TUNING
       "Read voice calibration constants from SONARE_TUNING_OVERRIDES (development only)" OFF)
if(BUILD_TUNING)
  message(STATUS "libsonare: BUILD_TUNING on — calibration constants are runtime-overridable; "
                 "do not ship this configuration")
  add_compile_definitions(SONARE_TUNING=1)
endif()

# macOS host backends — concrete implementations of the sonare::host seams that
# drive a real device/plugin. They include OS/SDK headers, so they are kept out
# of core (invariant 6), default OFF, and excluded from the default distribution
# and every language binding. macOS-only; ignored on other platforms.
option(BUILD_COREAUDIO "macOS CoreAudio host backend (host::AudioDevice)" OFF)
option(BUILD_COREMIDI  "macOS CoreMIDI host backend (host::MidiInputSource/Sink)" OFF)
option(BUILD_AU_HOST   "macOS Audio Unit plugin host (host::InstrumentProvider)" OFF)

# The analysis-only WebAssembly entry rejects BUILD_MASTERING / BUILD_MIXING, and
# BUILD_MIXING_ASSISTANT pulls both of them back on below. Drop the dependent
# rather than letting the raise fight the rejection: an option that defaults ON
# must not make every caller of an unrelated configuration learn about it.
if(SONARE_WASM_ANALYSIS_ONLY AND BUILD_MIXING_ASSISTANT)
  message(STATUS "libsonare: SONARE_WASM_ANALYSIS_ONLY excludes the mixing assistant; "
                 "disabling BUILD_MIXING_ASSISTANT")
  set(BUILD_MIXING_ASSISTANT OFF CACHE BOOL "Build the offline mixing assistant" FORCE)
endif()

if(BUILD_MIXING_ASSISTANT AND NOT BUILD_MIXING)
  message(STATUS "libsonare: BUILD_MIXING_ASSISTANT requires BUILD_MIXING; enabling BUILD_MIXING")
  set(BUILD_MIXING ON CACHE BOOL "Build the mixing library" FORCE)
endif()

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
# The capability descriptor reports which subsystems this build contains, so its
# flags have to mean the same thing in every translation unit that can compile
# it -- sonare_c_memory.cpp alone is pulled into the core objects, the CLI and
# the WASM entry, each of which used to carry its own copy of this table. A copy
# that omits a flag does not fail to build; it answers false, and the descriptor
# then advertises a subsystem the build actually has as missing. Deriving them
# once from the resolved options, after every dependency rule above has run, is
# what keeps the per-target copies from disagreeing.
add_compile_definitions(
  SONARE_BUILD_MASTERING=$<IF:$<BOOL:${BUILD_MASTERING}>,1,0>
  SONARE_BUILD_MIXING=$<IF:$<BOOL:${BUILD_MIXING}>,1,0>
  SONARE_BUILD_MIXING_ASSISTANT=$<IF:$<BOOL:${BUILD_MIXING_ASSISTANT}>,1,0>
  SONARE_BUILD_FX=$<IF:$<BOOL:${BUILD_FX}>,1,0>
  SONARE_BUILD_ARRANGEMENT=$<IF:$<BOOL:${BUILD_ARRANGEMENT}>,1,0>
)

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

# Link-line de-duplication.
#
# CMake's default REORDER_MINIMALLY strategy emits a target's direct link
# entries first, in their declared order, and then appends whatever the
# transitive closure still needs. An archive that is both a direct dependency
# and an indirect one therefore lands on the link line twice; with this tree's
# feature-scoped archives that is nine repeats on the CLI and the shared
# library, which Apple's linker reports as `ignoring duplicate libraries`.
# REORDER_FREELY lets CMake merge the two passes and de-duplicate. Nothing here
# intermixes raw link flags with libraries or relies on ordering between
# independent entries, which is the control that strategy gives up.
#
# Older CMake ignores the variable and keeps the duplicated -- still correct --
# link line.
if(NOT CMAKE_VERSION VERSION_LESS 3.31)
  set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

  # What survives de-duplication is the sonare_core / sonare_pitch_editor pair:
  # those two archives depend on each other (see src/CMakeLists.txt), and CMake
  # answers a declared cycle by repeating both -- which GNU ld needs to resolve
  # it and Apple's linker does not. Silence the report for that one deliberate
  # case. Kept inside this branch so a CMake too old for REORDER_FREELY still
  # reports the accidental duplicates, which are the ones worth seeing.
  if(APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries"
                      SONARE_LINKER_NO_WARN_DUPLICATE_LIBRARIES)
    if(SONARE_LINKER_NO_WARN_DUPLICATE_LIBRARIES)
      add_link_options("LINKER:-no_warn_duplicate_libraries")
    endif()
  endif()
endif()

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
  # a system Eigen (macOS CI, WASM). SYSTEM marks the headers so
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
if(BUILD_COREAUDIO OR BUILD_COREMIDI OR BUILD_AU_HOST)
  if(APPLE)
    add_subdirectory(src/host/backends)
  else()
    message(STATUS "libsonare: host backends (CoreAudio/CoreMIDI/AU) are macOS-only; ignoring on this platform")
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

# Runs last: it installs the targets every subdirectory above contributed.
if(SONARE_INSTALL)
  include(SonareInstall)
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
