# libsonare examples

These examples are small, runnable starting points rather than API snippets. Build the runtime named by each example before running it.

| Example | Runtime | Run |
| --- | --- | --- |
| `python/analyze.py` | Python package | `PYTHONPATH=bindings/python/src python examples/python/analyze.py song.wav` |
| `python/master.py` | Python package | `PYTHONPATH=bindings/python/src python examples/python/master.py song.wav mastered.wav` |
| `python/midi_to_wav.py` | Python package | `PYTHONPATH=bindings/python/src python examples/python/midi_to_wav.py arrangement.wav` (output path) |
| `browser/index.html` | WASM build | `python3 -m http.server` then open `http://localhost:8000/examples/browser/` |
| `browser/master.html` | WASM build | Open `http://localhost:8000/examples/browser/master.html` and download mastered WAV |
| `browser/midi-to-wav.html` | WASM build | Open `http://localhost:8000/examples/browser/midi-to-wav.html` and download a NativeSynth WAV |
| `node-native/analyze.mjs` | local Node native binding | `cd examples/node-native && yarn install && node --preserve-symlinks analyze.mjs song.wav` |
| `node-native/master.mjs` | local Node native binding | `cd examples/node-native && node --preserve-symlinks master.mjs song.wav mastered.wav` |
| `node-native/midi-to-wav.mjs` | local Node native binding | `cd examples/node-native && node --preserve-symlinks midi-to-wav.mjs phrase.wav` |
| `cpp/analyze.cpp` | C++ source build | `cmake -S examples/cpp -B build/examples && cmake --build build/examples && build/examples/analyze song.wav` |

Build the Python shared library before using the Python examples from a checkout: `make build-shared`. For Node, build the local binding first: `cd bindings/node && yarn install && yarn build`. The Node example uses Yarn's `portal:` protocol and the node-modules linker, so it links that build without packing the repository.

The browser examples import `bindings/wasm/dist/`, so install its dependencies and build it first: `cd bindings/wasm && yarn install && yarn build`. This requires an activated [Emscripten SDK](../CONTRIBUTING.md#development-setup). The analysis page decodes on the main thread, then sends analysis to `OfflineWorkerClient`; the source buffer transfers by default. The mastering and MIDI pages write PCM16 WAVs directly to a browser download. The basic Python server above does not enable COOP/COEP, so the analysis page explicitly disables prompt cancellation. Serve with cross-origin isolation headers when prompt cancellation is required. Yarn's `portal:` dependency also requires the `--preserve-symlinks` flag shown above.

Browser and Node-native examples are intentionally not run in CI; their required browser and local dependency setup is larger than the smoke coverage they would add. The Python and C++ examples are exercised in develop CI.
