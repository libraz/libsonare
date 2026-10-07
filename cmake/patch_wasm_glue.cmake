# Rewrites the Node-branch `await import("node:module")` in an Emscripten ES6
# glue file so browser bundlers (webpack, esbuild, vite) do not try to resolve
# the literal specifier. Usage: cmake -DGLUE_FILE=<path> -P patch_wasm_glue.cmake
#
# The specifier becomes a non-literal and process.getBuiltinModule is preferred
# where it exists. Fails unless the file holds exactly one original or exactly
# one already-patched occurrence, so an emitter change cannot slip through.

if(NOT GLUE_FILE OR NOT EXISTS "${GLUE_FILE}")
  message(FATAL_ERROR "patch_wasm_glue: GLUE_FILE is missing or not a file: '${GLUE_FILE}'")
endif()

set(_old [[const{createRequire}=await import("node:module")]])
set(_new [[const _m="node:module";const{createRequire}=process.getBuiltinModule?.("module")??await import(/* webpackIgnore: true */ /* @vite-ignore */ _m)]])

file(READ "${GLUE_FILE}" _content)
string(FIND "${_content}" "${_old}" _old_pos)
string(FIND "${_content}" "${_new}" _new_pos)

if(_old_pos EQUAL -1 AND NOT _new_pos EQUAL -1)
  string(FIND "${_content}" "${_new}" _new_last REVERSE)
  if(NOT _new_pos EQUAL _new_last)
    message(FATAL_ERROR "patch_wasm_glue: patched form occurs more than once in ${GLUE_FILE}")
  endif()
  return()
endif()

if(_old_pos EQUAL -1 OR NOT _new_pos EQUAL -1)
  message(FATAL_ERROR "patch_wasm_glue: expected exactly one `${_old}` in ${GLUE_FILE}; "
                      "the Emscripten glue shape changed")
endif()
string(FIND "${_content}" "${_old}" _old_last REVERSE)
if(NOT _old_pos EQUAL _old_last)
  message(FATAL_ERROR "patch_wasm_glue: `${_old}` occurs more than once in ${GLUE_FILE}")
endif()

string(REPLACE "${_old}" "${_new}" _content "${_content}")
file(WRITE "${GLUE_FILE}" "${_content}")
