cmake_minimum_required(VERSION 3.16)
project(libsonare_cpp_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prefer an installed libsonare, and fall back to building the checked-out tree
# in place. Both paths end at the same target name, so nothing below this block
# depends on which one ran.
#
#   installed:  cmake --install <libsonare build dir> --prefix ~/.local
#               cmake -B build -DCMAKE_PREFIX_PATH=~/.local
#   in tree:    cmake -B build
find_package(sonare CONFIG QUIET)
if(NOT sonare_FOUND)
  message(STATUS "libsonare not installed; building it from ../..")
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_CLI OFF CACHE BOOL "" FORCE)
  add_subdirectory(../.. libsonare)
endif()

add_executable(analyze analyze.cpp)
target_link_libraries(analyze PRIVATE sonare::sonare)
