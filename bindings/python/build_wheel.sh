#!/bin/bash
# Build libsonare shared library and package platform-specific Python wheel.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PYTHON_PKG="$SCRIPT_DIR/src/libsonare"

echo "=== Building libsonare shared library (Release) ==="
cmake -B "$PROJECT_ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON "$PROJECT_ROOT"
cmake --build "$PROJECT_ROOT/build" --config Release -j

echo "=== Copying shared library to Python package ==="
if [[ "$(uname)" == "Darwin" ]]; then
    cp "$PROJECT_ROOT/build/lib/libsonare.dylib" "$PYTHON_PKG/"
    # Fix install name for relocatable dylib
    install_name_tool -id @loader_path/libsonare.dylib "$PYTHON_PKG/libsonare.dylib" 2>/dev/null || true
    echo "Copied libsonare.dylib"
elif [[ "$(uname)" == "Linux" ]]; then
    cp "$PROJECT_ROOT/build/lib/libsonare.so" "$PYTHON_PKG/"
    echo "Copied libsonare.so"
fi

echo "=== Building Python wheel ==="
cd "$SCRIPT_DIR"
rm -rf dist/
python3 -m pip wheel . --no-deps -w dist/

echo "=== Re-tagging wheel with platform tag ==="
if [[ "$(uname)" == "Darwin" ]]; then
    ARCH="$(uname -m)"
    if [[ "$ARCH" == "arm64" ]]; then
        PLAT_TAG="macosx_11_0_arm64"
    else
        PLAT_TAG="macosx_10_15_x86_64"
    fi
elif [[ "$(uname)" == "Linux" ]]; then
    ARCH="$(uname -m)"
    PLAT_TAG="manylinux_2_17_${ARCH}"
else
    echo "Error: Unsupported platform $(uname)" >&2
    exit 1
fi

# Use 'wheel tags' to properly update both filename and internal WHEEL metadata
python3 -m wheel tags --platform-tag "$PLAT_TAG" --remove dist/*.whl

echo "=== Done ==="
ls -lh dist/*.whl 2>/dev/null
