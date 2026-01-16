# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/master/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

**librosa互換のオーディオ解析をC++とブラウザで。** 高速・依存なし・どこでも動作。

## ユースケース

- **librosaをネイティブ速度で使いたい？** → Eigen3ベクトル化によるC++実装
- **ブラウザで音声解析したい？** → WebAssemblyビルド (262KB)
- **音楽アプリを作りたい？** → BPM・キー・コード・ビート・セクションを1ライブラリで

## クイックスタート

### JavaScript / TypeScript

```bash
npm install @libraz/sonare  # 近日公開予定（現在β版）
```

```typescript
import { Sonare } from '@libraz/sonare';

const sonare = await Sonare.create();
const bpm = sonare.detectBpm(samples, sampleRate);
const key = sonare.detectKey(samples, sampleRate);  // { root: "C", mode: "Major" }
const beats = sonare.detectBeats(samples, sampleRate);
```

### C++

```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("music.mp3");
float bpm = sonare::quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
auto key = sonare::quick::detect_key(audio.data(), audio.size(), audio.sample_rate());
```

## 機能

| 解析 | DSP | エフェクト |
|------|-----|-----------|
| BPM / テンポ | STFT / iSTFT | HPSS |
| キー検出 | メルスペクトログラム | タイムストレッチ |
| ビートトラッキング | MFCC | ピッチシフト |
| コード認識 | クロマ | ノーマライズ |
| セクション検出 | CQT / VQT | |

## librosa互換性

libsonareはlibrosa互換のアルゴリズムを同一のデフォルトパラメータで実装。移行は簡単です。

```python
# librosa
S = librosa.feature.melspectrogram(y=y, sr=sr, n_mels=128, fmax=8000)
```

```cpp
// libsonare
auto mel = sonare::MelSpectrogram(sr, 2048, 512, 128, 0, 8000);
auto S = mel.compute(audio);
```

詳細は [librosa互換性ガイド](https://libsonare.libraz.net/ja/docs/librosa-compatibility) を参照。

## ビルド

```bash
# ネイティブ
make build && make test

# WebAssembly（要: source /path/to/emsdk/emsdk_env.sh）
make wasm
```

## ドキュメント

- [はじめに](https://libsonare.libraz.net/ja/docs/getting-started)
- [JavaScript API](https://libsonare.libraz.net/ja/docs/js-api)
- [C++ API](https://libsonare.libraz.net/ja/docs/cpp-api)
- [使用例](https://libsonare.libraz.net/ja/docs/examples)
- [WebAssemblyガイド](https://libsonare.libraz.net/ja/docs/wasm)

## ライセンス

[Apache-2.0](LICENSE)
