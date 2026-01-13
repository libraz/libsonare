# インストールガイド

## 必要条件

### システム要件

- **C++ コンパイラ**: C++17 対応
  - GCC 8 以降
  - Clang 8 以降
  - MSVC 2019 以降
- **CMake**: 3.16 以降
- **Eigen3**: 3.3 以降 (システムパッケージまたは FetchContent)

### WebAssembly ビルド用

- **Emscripten SDK (emsdk)**: 最新版推奨

## ソースからビルド

### リポジトリのクローン

```bash
git clone https://github.com/libraz/libsonare.git
cd libsonare
```

### ネイティブビルド (Linux/macOS)

#### デバッグビルド

```bash
make build
```

#### リリースビルド

```bash
make release
```

#### テスト実行

```bash
make test
```

317 のテストがすべてパスするはずです。

### 手動 CMake ビルド

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### WebAssembly ビルド

#### Emscripten のセットアップ

```bash
# emsdk のインストール (未インストールの場合)
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

#### WASM ビルド

```bash
cd /path/to/libsonare

# emsdk 環境を有効化
source /path/to/emsdk/emsdk_env.sh

# 設定とビルド
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --parallel
```

出力ファイル:
- `dist/sonare.js` (~34KB)
- `dist/sonare.wasm` (~228KB)

## CMake オプション

| オプション | デフォルト | 説明 |
|-----------|-----------|------|
| `BUILD_TESTING` | ON | テストスイートをビルド |
| `BUILD_WASM` | OFF | WebAssembly 用にビルド |
| `BUILD_SHARED` | OFF | 共有ライブラリをビルド |

## npm パッケージとしてインストール

```bash
npm install @libraz/sonare
# または
yarn add @libraz/sonare
```

## 依存関係のインストール

### Eigen3

**Ubuntu/Debian:**
```bash
sudo apt-get install libeigen3-dev
```

**macOS (Homebrew):**
```bash
brew install eigen
```

**ソースから:**
```bash
git clone https://gitlab.com/libeigen/eigen.git
cd eigen
mkdir build && cd build
cmake ..
sudo make install
```

注意: システムに Eigen3 が見つからない場合、CMake は FetchContent を使用して自動的に取得します。

## インストールの確認

### ネイティブ

```bash
# テスト実行
./build/tests/sonare_tests

# バージョン確認
./build/sonare --version  # (CLI がビルドされている場合)
```

### WebAssembly

```javascript
import createSonare from './dist/sonare.js';

const sonare = await createSonare();
console.log('Version:', sonare.version());
```

## トラブルシューティング

### Eigen3 が見つからない

CMake が Eigen3 を見つけられない場合、パスを手動で指定します:

```bash
cmake -B build -DEigen3_DIR=/path/to/eigen3/cmake
```

### Emscripten エラー

emsdk 環境が有効化されていることを確認してください:

```bash
source /path/to/emsdk/emsdk_env.sh
```

### WASM でのメモリ問題

大きな音声ファイルの場合、初期メモリを増やします:

```bash
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_EXE_LINKER_FLAGS="-s INITIAL_MEMORY=128MB"
```
