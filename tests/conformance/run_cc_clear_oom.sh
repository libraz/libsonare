#!/usr/bin/env bash
# Build and run the CC-map allocation refusal in a Linux scratch checkout.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
export SONARE_TEST_CC_CLEAR_OOM=1
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
case "${1:-}" in
  node)
    cd "$root/bindings/node"
    if [[ -f build/CMakeCache.txt ]]; then cmake -S . -B build; fi
    yarn build:native
    yarn build:ts
    yarn vitest run tests/surface/engine.test.ts -t 'reports OutOfMemory when clearing bindings fails'
    ;;
  wasm)
    cd "$root/bindings/wasm"
    yarn build:wasm
    yarn build:js
    yarn vitest run tests/surface/engine.test.ts -t 'reports OutOfMemory when clearing bindings fails'
    ;;
  python)
    cd "$root"
    build="$root/build-cc-clear-oom"
    cmake -S . -B "$build" -DBUILD_SHARED=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --target sonare_shared
    SONARE_LIB_PATH="$build/lib/libsonare.so" uv run --project bindings/python pytest -q \
      bindings/python/tests/test_engine_midi.py::test_engine_midi_cc_clear_out_of_memory_preserves_bindings
    ;;
  *)
    printf 'usage: bash %s {node|wasm|python}\n' "$0" >&2
    exit 2
    ;;
esac
