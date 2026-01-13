# Installation Guide

## Requirements

### System Requirements

- **C++ Compiler**: C++17 compatible
  - GCC 8 or later
  - Clang 8 or later
  - MSVC 2019 or later
- **CMake**: 3.16 or later
- **Eigen3**: 3.3 or later (system package or FetchContent)

### For WebAssembly Build

- **Emscripten SDK (emsdk)**: Latest version recommended

## Build from Source

### Clone Repository

```bash
git clone https://github.com/libraz/libsonare.git
cd libsonare
```

### Native Build (Linux/macOS)

#### Debug Build

```bash
make build
```

#### Release Build

```bash
make release
```

#### Run Tests

```bash
make test
```

All 317 tests should pass.

### Manual CMake Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### WebAssembly Build

#### Setup Emscripten

```bash
# Install emsdk (if not already installed)
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

#### Build WASM

```bash
cd /path/to/libsonare

# Activate emsdk environment
source /path/to/emsdk/emsdk_env.sh

# Configure and build
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --parallel
```

Output files:
- `dist/sonare.js` (~34KB)
- `dist/sonare.wasm` (~228KB)

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTING` | ON | Build test suite |
| `BUILD_WASM` | OFF | Build for WebAssembly |
| `BUILD_SHARED` | OFF | Build shared library |

## Install as npm Package

```bash
npm install @libraz/sonare
# or
yarn add @libraz/sonare
```

## Dependency Installation

### Eigen3

**Ubuntu/Debian:**
```bash
sudo apt-get install libeigen3-dev
```

**macOS (Homebrew):**
```bash
brew install eigen
```

**From Source:**
```bash
git clone https://gitlab.com/libeigen/eigen.git
cd eigen
mkdir build && cd build
cmake ..
sudo make install
```

Note: If Eigen3 is not found on the system, CMake will automatically fetch it using FetchContent.

## Verify Installation

### Native

```bash
# Run tests
./build/tests/sonare_tests

# Check version
./build/sonare --version  # (if CLI is built)
```

### WebAssembly

```javascript
import createSonare from './dist/sonare.js';

const sonare = await createSonare();
console.log('Version:', sonare.version());
```

## Troubleshooting

### Eigen3 Not Found

If CMake cannot find Eigen3, specify the path manually:

```bash
cmake -B build -DEigen3_DIR=/path/to/eigen3/cmake
```

### Emscripten Errors

Ensure emsdk environment is activated:

```bash
source /path/to/emsdk/emsdk_env.sh
```

### Memory Issues in WASM

For large audio files, increase the initial memory:

```bash
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_EXE_LINKER_FLAGS="-s INITIAL_MEMORY=128MB"
```
