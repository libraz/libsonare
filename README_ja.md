# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

**オーディオ解析と業務クオリティのマスタリングDSPを、C++・Python・ブラウザで。**
Apache-2.0、依存ライブラリなし、WebAssemblyで動くのでブラウザ上でも完結します。

- **解析（Analysis）** — BPM・キー・コード・ビート・セクション・音色・ダイナミクス・ピッチ
  （librosa互換のデフォルト値。librosa/Python比で数十倍高速）。
- **マスタリング（Mastering）** — EQ、ダイナミクス、マルチバンド、ステレオ、サチュレーション、
  リペア、マキシマイザー、リファレンスマッチング。公開規格・査読論文ベースの90以上のDSPモジュール
  （ITU-R BS.1770-4 ラウドネス／トゥルーピーク、Vicanekバイクァッド、ADAA非線形、
  Lemireスライディング最大、ポリフェーズFIRオーバーサンプリング 等）。
- **単一のパーミッシブライセンス** — スタック全体がApache-2.0。LGPL/GPLに触れず、
  プロプライエタリ算法・SaaS依存も含みません。

## インストール

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM、Float32Arrayを受け取り)
pip install libsonare            # Python（WAV/MP3対応。M4A/AACは「対応音声フォーマット」参照）
```

Node.jsでネイティブにファイルをデコードしたい場合は、
[`@libraz/libsonare-native`](bindings/node/) をソースからビルドします:

```bash
cd bindings/node
yarn install
yarn build  # pkg-configでFFmpegを自動検出（無ければWAV/MP3、有ればM4A/AAC/FLAC/OGGも対応）
```

明示的に切り替えたい場合:

```bash
SONARE_FFMPEG=0 yarn build  # FFmpegを使わない
SONARE_FFMPEG=1 yarn build  # FFmpegを必須（dev libs不在ならビルド失敗）
```

## クイックスタート

### JavaScript / TypeScript (WASM)

`@libraz/libsonare` はデコード済みの `Float32Array` を受け取ります（Web Audio APIや
JSデコーダで取得してください）。マスタリングDSPは標準のWASMビルドに同梱されています。

**解析**

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);
```

**マスタリング**

```typescript
import {
  init,
  masteringChain,
  masteringChainStereo,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringProcess,
} from '@libraz/libsonare';

await init();

const mastered = masteringChain(samples, sampleRate, {
  eq: { tiltDb: 1.0 },
  dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
  saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

const stereo = masteringChainStereo(left, right, sampleRate, {
  stereo: { imager: { width: 1.1 }, monoMaker: { amount: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

// 単一の名前付きプロセッサを適用
const compressed = masteringProcess('dynamics.compressor', samples, sampleRate, {
  thresholdDb: -24,
  ratio: 1.5,
});

// リファレンス参照のマスタリング
const matched = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
  mix: 0.25,
});
const loudnessJson = masteringPairAnalyze(
  'match.referenceLoudness', source, reference, sampleRate,
);
```

### Python

`pip install libsonare` で提供されるホイールは **WAV/MP3のみ対応** です
（librosa / pydub / soundfile と同じ慣習）。M4A/AAC/FLAC/OGG を扱う場合は、
事前に外部 `ffmpeg` で変換するか、FFmpegをリンクしたソースビルドを行ってください:

```bash
SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare
# システム側のFFmpeg dev libsが必要:
# brew install ffmpeg / apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

# マスタリング — 返り値は MasteringResult(samples, sample_rate,
# input_lufs, output_lufs, applied_gain_db, latency_samples)
result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS "
      f"(gain {result.applied_gain_db:+.2f} dB)")

# 単一プロセッサ / リファレンス参照解析
compressed = libsonare.mastering_process(
    "dynamics.compressor", samples, sample_rate=44100,
    params={"thresholdDb": -24, "ratio": 1.5},
)
loudness_json = libsonare.mastering_pair_analyze(
    "match.referenceLoudness", source, reference, sample_rate=44100,
)
```

### Python CLI

```bash
pip install libsonare

# 解析
sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json

# マスタリング
sonare mastering song.wav -o mastered.wav --target-lufs -14
sonare mastering-processor song.wav --processor dynamics.compressor \
    --params thresholdDb=-24,ratio=1.5
sonare mastering-pair-analyze source.wav --reference reference.wav \
    --analysis match.referenceLoudness
```

### C++

```cpp
#include <sonare/sonare.h>           // 解析 + 特徴量 + エフェクト
#include <sonare/mastering/master.h> // マスタリングチェイン & プロセッサ

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

## 機能

### 解析（librosa互換）

| 音楽               | スペクトル / ピッチ  | ストリーミング       |
|--------------------|----------------------|----------------------|
| BPM / テンポ       | STFT / iSTFT         | リアルタイム解析     |
| キー検出           | メルスペクトログラム | 逐次BPM              |
| ビートトラッキング | MFCC                 | 逐次キー             |
| コード認識         | クロマ               | オンセットイベント   |
| セクション検出     | CQT / VQT            |                      |
| 音色 / ダイナミクス| スペクトル特徴量     |                      |
| ピッチ (YIN/pYIN)  | オンセット検出       |                      |

### マスタリング（90以上のDSPモジュール）

| ダイナミクス                | EQ                          | マルチバンド / ステレオ                |
|-----------------------------|-----------------------------|----------------------------------------|
| コンプレッサー              | パラメトリック / グラフィック | マルチバンド comp / EQ / limiter       |
| リミッター / ブリックウォール| リニアフェーズ / ミニマムフェーズ | ステレオイメージャー / M-S           |
| エキスパンダー / ゲート     | ダイナミックEQ              | Haas / フェーズアライン                |
| ディエッサー                | Pultec / API スタイル       | モノメーカー / モノ互換チェック        |
| トランジェントシェイパー    | チルト / シェルビング       |                                        |

| サチュレーション / リペア             | マキシマイザー / マッチ                  | ビルディングブロック        |
|--------------------------------------|-----------------------------------------|-----------------------------|
| テープ / チューブ / トランス         | トゥルーピークリミッター (ITU-R BS.1770-4) | ポリフェーズFIRオーバーサンプラ |
| エキサイター / ビットクラッシャー   | ラウドネスオプティマイザ (LUFSターゲット) | ADAA非線形                  |
| デクリック / デクリップ / デクラックル | アダプティブリリース                    | Vicanekバイクァッド設計      |
| デノイズ / ディリバーブ / デハム     | リファレンスEQ / loudness / spectrum     | パーティションドコンボルバー |

マスタリングはデフォルトでビルドされます（`BUILD_MASTERING=ON`）。
`cmake -DBUILD_MASTERING=OFF` で解析専用ビルド（バイナリを小さく）にもできます。

## パフォーマンス

Pythonベースの代替ライブラリと比較して圧倒的に高速。CPU自動検出による並列解析、
マルチスレッドHPSSメディアンフィルタで最適化。マスタリングはITU規格準拠の
ポリフェーズオーバーサンプリング、ADAA（積分による反エイリアス）、
ホットパスでのSIMD対応Eigen GEMMを採用しています。

詳細は[ベンチマーク](https://libsonare.libraz.net/ja/docs/benchmarks)を参照。

## librosa互換性

デフォルトパラメータはlibrosaに揃えています:
- サンプルレート: 22050 Hz
- n_fft: 2048, hop_length: 512, n_mels: 128
- fmin: 0.0, fmax: sr/2

## 対応音声フォーマット

| フォーマット | デフォルト¹ | FFmpegあり² | WASM (`@libraz/libsonare`) |
|--------------|-------------|-------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | n/a (sampleを直接渡す) |
| MP3 | ✅ | ✅ | n/a |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | ❌（明示エラー） | ✅ | n/a (Web Audio APIを使用) |

¹ **デフォルト**: PyPIホイール（`pip install libsonare`）と、FFmpeg dev libsが
無い環境でのソースビルド。PyPIホイールはこのモードに固定されており、
ユーザの `libavformat` の有無に左右されません。

² **FFmpegあり**: FFmpegをリンクしたソースビルド。CMakeはpkg-config経由で
自動検出します（`-DSONARE_WITH_FFMPEG=AUTO` がデフォルト）。
`-DSONARE_WITH_FFMPEG=ON`/`OFF` で強制できます。Python:
`SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare`。
Nodeネイティブ: `SONARE_FFMPEG=1 yarn build`。

WASMは設計上ファイルデコーダを同梱しません。
Web Audio APIなどでデコードした `Float32Array` を渡してください。

## ソースからビルド

```bash
# ネイティブ（FFmpeg自動検出。マスタリングはデフォルトでON）
make build && make test

# 解析専用（バイナリを小さくしたいとき）
cmake -B build -DBUILD_MASTERING=OFF && cmake --build build

# WebAssembly（マスタリング同梱）
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
