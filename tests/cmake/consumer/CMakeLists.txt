cmake_minimum_required(VERSION 3.16)
project(sonare_find_package_consumer LANGUAGES C CXX)

# Smoke test for the installed package: a project that knows nothing about
# libsonare's build tree, finds it with find_package, and links the exported
# targets. Driven by `make test-install`, which installs into a scratch prefix
# and points CMAKE_PREFIX_PATH here. What it proves is exactly what an in-tree
# build cannot: that the exported targets carry no source-tree paths, that the
# installed header layout resolves both include spellings, and that the link
# interface names nothing the consumer has to supply itself.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(sonare REQUIRED)

add_executable(consumer_cpp consumer.cpp)
target_link_libraries(consumer_cpp PRIVATE sonare::sonare)

# The C ABI on its own, through a narrower target than the aggregate.
add_executable(consumer_c consumer_c.c)
target_link_libraries(consumer_c PRIVATE sonare::core)

enable_testing()
add_test(NAME consumer_cpp COMMAND consumer_cpp)
add_test(NAME consumer_c COMMAND consumer_c)

# Per-archive link closure. Calling into one subsystem pulls only the objects
# that subsystem's entry points need, so a missing entry in a target's link
# interface stays invisible until some consumer happens to reach the one
# function that crosses into the undeclared archive. Forcing the whole archive
# in turns that into a link error here instead: every object of every exported
# target has to resolve against nothing but that target's declared interface.
if(APPLE)
  set(SONARE_WHOLE_ARCHIVE_BEGIN "-Wl,-force_load")
  set(SONARE_WHOLE_ARCHIVE_END "")
else()
  set(SONARE_WHOLE_ARCHIVE_BEGIN "-Wl,--whole-archive")
  set(SONARE_WHOLE_ARCHIVE_END "-Wl,--no-whole-archive")
endif()

set(SONARE_COMPONENTS
  core rt automation acoustic fx mastering mixing mixing_assistant graph
  pitch_editor voice_changer midi engine arrangement mir serialize assist
)

foreach(component IN LISTS SONARE_COMPONENTS)
  if(NOT TARGET sonare::${component})
    continue()
  endif()
  add_executable(link_closure_${component} link_closure.cpp)
  target_link_libraries(link_closure_${component} PRIVATE sonare::${component})
  target_link_options(link_closure_${component} PRIVATE
    ${SONARE_WHOLE_ARCHIVE_BEGIN}
    $<TARGET_FILE:sonare::${component}>
    ${SONARE_WHOLE_ARCHIVE_END}
  )
endforeach()
