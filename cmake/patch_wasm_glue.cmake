# Rewrites the Node-branch `await import("node:module")` in an Emscripten ES6
# glue file to `process.getBuiltinModule("module")`, so the glue holds no
# dynamic import at all. A bundler resolves a literal `node:module` for the
# browser, and a dev server (vite dev) rewrites a non-literal `import()`
# through its client script, which throws inside an AudioWorkletGlobalScope.
# process.getBuiltinModule exists from Node 22.3, below the package's floor.
# Usage: cmake -DGLUE_FILE=<path> -P patch_wasm_glue.cmake
#
# Fails unless the file holds exactly one original or exactly one
# already-patched occurrence, so an emitter change cannot slip through.
# The known forms are the `_old_form_*` variables.

if(NOT GLUE_FILE OR NOT EXISTS "${GLUE_FILE}")
  message(FATAL_ERROR "patch_wasm_glue: GLUE_FILE is missing or not a file: '${GLUE_FILE}'")
endif()

# Emscripten 7 parenthesises the import; 6.x does not. The third is this
# script's previous output, still present in a glue file linked before it.
# Held as separate variables because a CMake list would split the third at its `;`.
set(_old_form_0 [[const{createRequire}=await(import("node:module"))]])
set(_old_form_1 [[const{createRequire}=await import("node:module")]])
set(_old_form_2 [[const _m="node:module";const{createRequire}=process.getBuiltinModule?.("module")??await import(/* webpackIgnore: true */ /* @vite-ignore */ _m)]])
set(_new [[const{createRequire}=process.getBuiltinModule("module")]])

file(READ "${GLUE_FILE}" _content)
set(_old "${_old_form_0}")
foreach(_form_var IN ITEMS _old_form_0 _old_form_1 _old_form_2)
  string(FIND "${_content}" "${${_form_var}}" _old_pos)
  if(NOT _old_pos EQUAL -1)
    set(_old "${${_form_var}}")
    break()
  endif()
endforeach()
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
