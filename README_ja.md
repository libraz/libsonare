# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

**librosa互換のオーディオ解析をC++、Python、ブラウザで。** 高速・依存なし・どこでも動作。

librosa/Pythonより数十倍高速。

## インストール

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM)
pip install libsonare            # Python
```

## クイックスタート

### JavaScript / TypeScript

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);
```

### Python

```python
import libsonare

bpm = libsonare.detect_bpm(samples, sample_rate=22050)
key = libsonare.detect_key(samples, sample_rate=22050)
result = libsonare.analyze(samples, sample_rate=22050)

# Audioクラスも使えます
audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")
```

### Python CLI

```bash
pip install libsonare

sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json
# {"bpm": 161.0}
```

### C++

```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm << ", Key: " << result.key.to_string() << std::endl;
```

## 機能

| 解析 | DSP | エフェクト |
|------|-----|-----------|
| BPM / テンポ | STFT / iSTFT | HPSS |
| キー検出 | メルスペクトログラム | タイムストレッチ |
| ビートトラッキング | MFCC | ピッチシフト |
| コード認識 | クロマ | ノーマライズ / トリム |
| セクション検出 | CQT / VQT | |
| 音色 / ダイナミクス | スペクトル特徴量 | |
| ピッチ検出 (YIN/pYIN) | オンセット検出 | |
| リアルタイムストリーミング | リサンプル | |

## パフォーマンス

Pythonベースの代替ライブラリと比較して圧倒的に高速。CPU自動検出による並列解析、マルチスレッドHPSSメディアンフィルタで最適化。

詳細は[ベンチマーク](https://libsonare.libraz.net/ja/docs/benchmarks)を参照。

## librosa互換性

デフォルトパラメータはlibrosaと一致:
- サンプルレート: 22050 Hz
- n_fft: 2048, hop_length: 512, n_mels: 128
- fmin: 0.0, fmax: sr/2

## ソースからビルド

```bash
# ネイティブ（C++ライブラリ + CLI）
make build && make test

# WebAssembly
make wasm

# リリースビルド（最適化）
make release
```

## ドキュメント

- [はじめに](https://libsonare.libraz.net/ja/docs/getting-started)
- [JavaScript API](https://libsonare.libraz.net/ja/docs/js-api)
- [Python API](https://libsonare.libraz.net/ja/docs/python-api)
- [C++ API](https://libsonare.libraz.net/ja/docs/cpp-api)
- [WebAssemblyガイド](https://libsonare.libraz.net/ja/docs/wasm)

## ライセンス

[Apache-2.0](LICENSE)
