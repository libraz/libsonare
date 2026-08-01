cmake_minimum_required(VERSION 3.16)
project(libsonare_cpp_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(../.. libsonare)

add_executable(analyze analyze.cpp)
target_link_libraries(analyze PRIVATE sonare_core sonare_rt)
