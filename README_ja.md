# libsonare

[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/master/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

音楽情報検索のための C++17 オーディオ解析ライブラリ。WebAssembly 対応、Eigen3 ベースの信号処理。

## なぜ libsonare なのか？

**libsonare** は包括的な音楽解析機能を提供します：

- **BPM 検出**: テンポグラムと自己相関による正確なテンポ推定
- **キー検出**: 強化されたキープロファイルによる Krumhansl-Schmuckler アルゴリズム
- **ビートトラッキング**: 動的計画法ベースのビート検出
- **コード認識**: 84種類のコードタイプに対応したテンプレートマッチング
- **セクション検出**: ノベルティカーブを使用した構造セグメンテーション
- **オーディオエフェクト**: HPSS、タイムストレッチ、ピッチシフト

すべての機能がネイティブ C++ と WebAssembly 環境の両方で動作します。libsonare は主にオフライン解析向けに設計されていますが、コア DSP モジュールは WebAssembly での低レイテンシオーディオ処理にも使用できます。

## 特徴

| カテゴリ | 機能 |
|---------|------|
| **コア** | STFT/iSTFT、Griffin-Lim、リサンプリング (r8brain) |
| **特徴量** | メルスペクトログラム、MFCC、クロマ、スペクトル特徴量 |
| **エフェクト** | HPSS、タイムストレッチ、ピッチシフト、ノーマライズ |
| **解析** | BPM、キー、ビート、コード、セクション、音色、ダイナミクス |
| **プラットフォーム** | Linux、macOS、WebAssembly |
| **音声 I/O** | WAV、MP3 (dr_libs、minimp3) |

## クイックスタート

### ネイティブ (C++)

```cpp
#include <sonare/sonare.h>

// 音声ファイルの読み込み
auto audio = sonare::Audio::from_file("music.mp3");

// シンプルな解析
float bpm = sonare::quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
auto key = sonare::quick::detect_key(audio.data(), audio.size(), audio.sample_rate());

std::cout << "BPM: " << bpm << std::endl;
std::cout << "Key: " << key.to_string() << std::endl;  // 例: "C major"

// フル解析
sonare::MusicAnalyzer analyzer(audio);
auto result = analyzer.analyze();

std::cout << "Beats: " << result.beats.size() << std::endl;
std::cout << "Sections: " << result.sections.size() << std::endl;
```

### WebAssembly (JavaScript/TypeScript)

```typescript
import { Sonare } from '@libraz/sonare';

const sonare = await Sonare.create();

// Web Audio API で音声をデコード
const audioCtx = new AudioContext();
const response = await fetch('music.mp3');
const audioBuffer = await audioCtx.decodeAudioData(await response.arrayBuffer());
const samples = audioBuffer.getChannelData(0);

// 解析
const bpm = sonare.detectBpm(samples, audioBuffer.sampleRate);
const key = sonare.detectKey(samples, audioBuffer.sampleRate);

console.log(`BPM: ${bpm}`);
console.log(`Key: ${key.root} ${key.mode}`);  // 例: "C Major"
```

## インストール

### 必要条件

**システム:**
- C++17 対応コンパイラ (GCC 8+、Clang 8+、MSVC 2019+)
- CMake 3.16+
- Eigen3 (システムパッケージまたは FetchContent)

**WebAssembly 用:**
- Emscripten SDK (emsdk)

### ソースからビルド

```bash
git clone https://github.com/libraz/libsonare.git
cd libsonare

# ネイティブビルド
make build
make test   # テスト実行 (317 テスト)

# WebAssembly ビルド
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --parallel

# 出力: dist/sonare.js (34KB)、dist/sonare.wasm (228KB)
```

### npm パッケージ

```bash
npm install @libraz/sonare
# または
yarn add @libraz/sonare
```

## API リファレンス

### Quick API (シンプルな関数)

```cpp
namespace sonare::quick {
  // BPM 検出
  float detect_bpm(const float* samples, size_t length, int sample_rate);

  // キー検出
  Key detect_key(const float* samples, size_t length, int sample_rate);

  // ビート検出
  std::vector<float> detect_beats(const float* samples, size_t length, int sample_rate);

  // オンセット検出
  std::vector<float> detect_onsets(const float* samples, size_t length, int sample_rate);

  // フル解析
  AnalysisResult analyze(const float* samples, size_t length, int sample_rate);
}
```

### MusicAnalyzer (Facade)

```cpp
class MusicAnalyzer {
public:
  explicit MusicAnalyzer(const Audio& audio);

  // 個別アナライザー (遅延初期化)
  BpmAnalyzer& bpm_analyzer();
  KeyAnalyzer& key_analyzer();
  BeatAnalyzer& beat_analyzer();
  ChordAnalyzer& chord_analyzer();
  SectionAnalyzer& section_analyzer();
  // ...

  // クイックアクセス
  float bpm() const;
  Key key() const;
  std::vector<Beat> beats() const;
  std::vector<Chord> chords() const;
  std::vector<Section> sections() const;

  // フル解析
  AnalysisResult analyze() const;
};
```

### C API

```c
#include <sonare_c.h>

SonareAudio* audio = NULL;
sonare_audio_from_file("music.mp3", &audio);

float bpm;
sonare_detect_bpm(audio, &bpm, NULL);

sonare_audio_free(audio);
```

## アーキテクチャ

```
src/
├── util/           # 基本型、例外、数学ユーティリティ
├── core/           # FFT、STFT、窓関数、Audio I/O、リサンプル
├── filters/        # Mel、Chroma、DCT、IIR フィルタバンク
├── feature/        # メルスペクトログラム、MFCC、クロマ、スペクトル特徴量
├── effects/        # HPSS、タイムストレッチ、ピッチシフト
├── analysis/       # BPM、キー、ビート、コード、セクション解析
├── quick.h/cpp     # シンプルな関数 API
├── sonare.h        # 統合ヘッダ
├── sonare_c.h/cpp  # C API
└── wasm/           # Embind バインディング
```

### 依存関係レベル

| レベル | モジュール | 依存先 |
|--------|-----------|--------|
| 0 | util/ | なし |
| 1 | core/convert、core/window | util/ |
| 2 | core/fft | util/、KissFFT |
| 3 | core/spectrum | core/fft、core/window |
| 4 | filters/、feature/ | core/ |
| 5 | effects/ | core/、feature/ |
| 6 | analysis/ | feature/、effects/ |

## ドキュメント

### ガイド
- [インストールガイド](docs/ja/installation.md) - ソースからビルド
- [使用例](docs/ja/examples.md) - 使用例
- [WASM ガイド](docs/ja/wasm.md) - WebAssembly の使い方

### API リファレンス
- [C++ API リファレンス](docs/ja/cpp-api.md) - 完全な C++ API ドキュメント
- [JavaScript/TypeScript API リファレンス](docs/ja/js-api.md) - 完全な JS/TS API ドキュメント
- [CLI リファレンス](docs/ja/cli-reference.md) - コマンドラインインターフェースのドキュメント

### 技術資料
- [アーキテクチャ](docs/ja/architecture.md) - 内部アーキテクチャと設計
- [librosa 互換性](docs/ja/librosa-compatibility.md) - librosa との比較と移行ガイド

## librosa との比較

libsonare は Python の librosa と同様の機能を、C++ と WebAssembly 向けに最適化して提供します：

| 機能 | librosa | libsonare |
|------|---------|-----------|
| STFT/iSTFT | Yes | Yes |
| メルスペクトログラム | Yes | Yes |
| MFCC | Yes | Yes |
| クロマ | Yes | Yes |
| HPSS | Yes | Yes |
| ビートトラッキング | Yes | Yes |
| テンポ検出 | Yes | Yes |
| キー検出 | No (madmom 使用) | Yes |
| コード検出 | No | Yes |
| セクション検出 | No | Yes |
| WebAssembly | No | Yes |

## サードパーティライブラリ

| ライブラリ | 用途 |
|-----------|------|
| [KissFFT](https://github.com/mborgerding/kissfft) | FFT 演算 |
| [Eigen3](https://eigen.tuxfamily.org/) | 行列演算 |
| [dr_libs](https://github.com/mackron/dr_libs) | WAV デコード |
| [minimp3](https://github.com/lieff/minimp3) | MP3 デコード |
| [r8brain](https://github.com/avaneev/r8brain-free-src) | リサンプリング |
| [Catch2](https://github.com/catchorg/Catch2) | テスト |

## ライセンス

[Apache License 2.0](LICENSE)

## コントリビューション

コントリビューションを歓迎します！ガイドラインは [CONTRIBUTING.md](CONTRIBUTING.md) を参照してください。

## 作者

- libraz <libraz@libraz.net>
