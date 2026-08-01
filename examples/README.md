# libsonare examples

These examples are small, runnable starting points rather than API snippets. Build the runtime named by each example before running it.

| Example | Runtime | Run |
| --- | --- | --- |
| `python/analyze.py` | Python package | `python examples/python/analyze.py song.wav` |
| `python/master.py` | Python package | `python examples/python/master.py song.wav mastered.wav` |
| `python/midi_to_wav.py` | Python package | `python examples/python/midi_to_wav.py arrangement.wav` |
| `browser/index.html` | WASM build | `python3 -m http.server -d examples/browser` |
| `node-native/analyze.mjs` | local Node native binding | `cd examples/node-native && yarn install && node analyze.mjs song.wav` |
| `cpp/analyze.cpp` | C++ source build | `cmake -S examples/cpp -B build/examples && cmake --build build/examples && build/examples/analyze song.wav` |

Build the Python shared library before using the Python examples from a checkout: `make build-shared`. The browser example imports `bindings/wasm/dist/`, so run `cd bindings/wasm && yarn build:wasm && yarn build:js` first. Browser and Node-native examples are intentionally not run in CI; their required browser and local dependency setup is larger than the smoke coverage they would add. The Python and C++ examples are exercised in develop CI.
