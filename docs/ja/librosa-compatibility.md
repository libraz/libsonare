# librosa 互換性ガイド

このドキュメントでは、libsonare の関数が Python の librosa ライブラリとどのように対応するかを説明します。パラメータマッピング、アルゴリズムの違い、検証方法論を含みます。

## 概要

libsonare は [librosa](https://librosa.org/) と同様の機能を提供することを目指しており、C++ と WebAssembly 環境向けに最適化されています。ほとんどのコア機能は librosa と同じアルゴリズムを使用し、互換性のあるデフォルトパラメータを採用しています。

## 機能比較

### サポートされている機能

| librosa | libsonare | 備考 |
|---------|-----------|------|
| `librosa.load()` | `Audio::from_file()` | WAV、MP3 サポート |
| `librosa.resample()` | `resample()` | r8brain 使用 |
| `librosa.stft()` | `Spectrogram::compute()` | 完全互換 |
| `librosa.istft()` | `Spectrogram::to_audio()` | OLA 再構成 |
| `librosa.griffinlim()` | `griffin_lim()` | モメンタムベース |
| `librosa.feature.melspectrogram()` | `MelSpectrogram::compute()` | Slaney 正規化 |
| `librosa.feature.mfcc()` | `MelSpectrogram::mfcc()` | DCT-II、リフタリング |
| `librosa.feature.chroma_stft()` | `Chroma::compute()` | STFT ベース |
| `librosa.onset.onset_strength()` | `compute_onset_strength()` | スペクトルフラックス |
| `librosa.beat.beat_track()` | `BeatAnalyzer` | DP ベース |
| `librosa.beat.tempo()` | `BpmAnalyzer` | テンポグラム |
| `librosa.effects.hpss()` | `hpss()` | メディアンフィルタリング |
| `librosa.effects.time_stretch()` | `time_stretch()` | 位相ボコーダー |
| `librosa.effects.pitch_shift()` | `pitch_shift()` | WSOLA 類似 |

### librosa にない機能

| libsonare | 説明 |
|-----------|------|
| `KeyAnalyzer` | 音楽キー検出 (Krumhansl-Schmuckler) |
| `ChordAnalyzer` | コード認識 (テンプレートマッチング) |
| `SectionAnalyzer` | 楽曲構造解析 |
| `TimbreAnalyzer` | 音色特性 |
| `DynamicsAnalyzer` | ラウドネスとダイナミクス |
| `RhythmAnalyzer` | 拍子、グルーブ |
| `MelodyAnalyzer` | ピッチ/メロディ追跡 |

---

## 詳細な関数マッピング

### STFT (短時間フーリエ変換)

#### librosa

```python
import librosa
import numpy as np

y, sr = librosa.load('audio.wav', sr=22050)

# STFT
S = librosa.stft(
    y,
    n_fft=2048,
    hop_length=512,
    win_length=None,  # デフォルトは n_fft
    window='hann',
    center=True,
    pad_mode='constant'
)

# 結果の形状: (1 + n_fft/2, n_frames)
# 複素値
```

#### libsonare

```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("audio.wav");
// 必要に応じて 22050 にリサンプル
audio = sonare::resample(audio, 22050);

sonare::StftConfig config;
config.n_fft = 2048;
config.hop_length = 512;
config.win_length = 0;  // デフォルトは n_fft
config.window = sonare::WindowType::Hann;
config.center = true;

auto spec = sonare::Spectrogram::compute(audio, config);

// 結果: n_bins() x n_frames()
// complex_view() で複素値にアクセス
```

**互換性に関する注意:**
- デフォルトパラメータは同一
- パディングモードは常に 'constant' (ゼロパディング)
- 窓関数: Hann、Hamming、Blackman をサポート
- 複素出力形式は互換

---

### メルスペクトログラム

#### librosa

```python
import librosa

# メルスペクトログラム
mel = librosa.feature.melspectrogram(
    y=y,
    sr=sr,
    n_fft=2048,
    hop_length=512,
    n_mels=128,
    fmin=0.0,
    fmax=None,  # sr/2
    htk=False,  # Slaney 式
    norm='slaney'
)

# dB に変換
mel_db = librosa.power_to_db(mel, ref=np.max)
```

#### libsonare

```cpp
sonare::MelConfig config;
config.n_mels = 128;
config.fmin = 0.0f;
config.fmax = 0.0f;  // sr/2
config.stft.n_fft = 2048;
config.stft.hop_length = 512;

auto mel = sonare::MelSpectrogram::compute(audio, config);

// dB に変換
auto mel_db = mel.to_db(1.0f, 1e-10f);
```

**互換性に関する注意:**
- Slaney メルスケールがデフォルト (librosa と同じ)
- HTK メルスケールは `hz_to_mel_htk()` で利用可能
- Slaney 正規化がデフォルトで適用
- dB 変換は同じ式を使用: `10 * log10(max(power, amin) / ref)`

---

### MFCC

#### librosa

```python
mfcc = librosa.feature.mfcc(
    y=y,
    sr=sr,
    n_mfcc=13,
    n_fft=2048,
    hop_length=512,
    n_mels=128,
    fmin=0.0,
    fmax=None,
    lifter=0
)

# デルタ特徴量
mfcc_delta = librosa.feature.delta(mfcc)
```

#### libsonare

```cpp
auto mel = sonare::MelSpectrogram::compute(audio, config);

// MFCC (DCT-II、直交正規化)
auto mfcc = mel.mfcc(13, 0.0f);  // n_mfcc=13, lifter=0

// デルタ特徴量
auto mfcc_delta = sonare::MelSpectrogram::delta(mfcc, 13, mel.n_frames());
```

**互換性に関する注意:**
- 直交正規化付き DCT-II (librosa と同じ)
- リフタリング係数をサポート
- デルタ計算は同じウィンドウベースの方法を使用

---

### クロマ

#### librosa

```python
chroma = librosa.feature.chroma_stft(
    y=y,
    sr=sr,
    n_fft=2048,
    hop_length=512,
    n_chroma=12,
    tuning=0.0
)
```

#### libsonare

```cpp
sonare::ChromaConfig config;
config.n_chroma = 12;
config.tuning = 0.0f;
config.stft.n_fft = 2048;
config.stft.hop_length = 512;

auto chroma = sonare::Chroma::compute(audio, config);
```

**互換性に関する注意:**
- STFT ベースのクロマ (CQT ではない)
- セント単位のチューニングオフセット
- フレームごとに出力を正規化

---

### HPSS

#### librosa

```python
y_harm, y_perc = librosa.effects.hpss(
    y,
    kernel_size=31,
    power=2.0,
    margin=1.0
)
```

#### libsonare

```cpp
sonare::HpssConfig config;
config.kernel_size_harmonic = 31;
config.kernel_size_percussive = 31;
config.power = 2.0f;
config.margin_harmonic = 1.0f;
config.margin_percussive = 1.0f;

auto result = sonare::hpss(audio, config);
// result.harmonic
// result.percussive
```

**互換性に関する注意:**
- メディアンフィルタリングアルゴリズムは同一
- パワーパラメータ付きソフトマスキング
- H と P で別々のカーネルサイズ (librosa はデフォルトで両方に同じ値を使用)

---

### ビートトラッキング

#### librosa

```python
tempo, beats = librosa.beat.beat_track(
    y=y,
    sr=sr,
    hop_length=512,
    start_bpm=120.0,
    tightness=100
)

beat_times = librosa.frames_to_time(beats, sr=sr, hop_length=512)
```

#### libsonare

```cpp
sonare::BeatConfig config;
config.start_bpm = 120.0f;
config.tightness = 100.0f;

sonare::BeatAnalyzer analyzer(audio, config);

float bpm = analyzer.bpm();
auto beat_times = analyzer.beat_times();  // 既に秒単位
```

**互換性に関する注意:**
- 動的計画法ビートトラッカー
- 開始 BPM 事前分布
- タイトネスパラメータでテンポの一貫性を制御
- 時刻を直接返す (フレームから時間への変換不要)

---

### テンポ検出

#### librosa

```python
tempo, _ = librosa.beat.beat_track(y=y, sr=sr)
# または
tempo = librosa.feature.tempo(
    y=y,
    sr=sr,
    hop_length=512,
    start_bpm=120
)
```

#### libsonare

```cpp
sonare::BpmConfig config;
config.bpm_min = 60.0f;
config.bpm_max = 200.0f;
config.start_bpm = 120.0f;

sonare::BpmAnalyzer analyzer(audio, config);

float bpm = analyzer.bpm();
float confidence = analyzer.confidence();
auto candidates = analyzer.bpm_candidates();
```

**互換性に関する注意:**
- テンポグラムベースのアルゴリズム
- 開始 BPM が候補選択に影響
- 信頼度スコアを返す
- 複数の BPM 候補を利用可能

---

## デフォルトパラメータ

### librosa デフォルト (参照)

| パラメータ | librosa デフォルト | libsonare デフォルト |
|-----------|-------------------|---------------------|
| `sr` | 22050 | ユーザー指定 |
| `n_fft` | 2048 | 2048 |
| `hop_length` | 512 | 512 |
| `win_length` | n_fft | n_fft (0) |
| `window` | 'hann' | Hann |
| `center` | True | true |
| `n_mels` | 128 | 128 |
| `fmin` | 0.0 | 0.0 |
| `fmax` | sr/2 | sr/2 (0.0) |
| `n_mfcc` | 20 | 13 |
| `n_chroma` | 12 | 12 |

---

## メルスケール公式

### Slaney (librosa デフォルト、libsonare デフォルト)

```
f < 1000 Hz の場合 (線形領域):
    mel = 3 * f / 200

f >= 1000 Hz の場合 (対数領域):
    mel = 15 + 27 * log10(f / 1000) / log10(6.4)
```

### HTK

```
mel = 2595 * log10(1 + f / 700)
```

libsonare は両方を提供:
```cpp
float mel_slaney = sonare::hz_to_mel(hz);
float mel_htk = sonare::hz_to_mel_htk(hz);
```

---

## 検証方法論

### テストデータ生成

参照値は librosa から生成:

```python
# scripts/generate_reference.py
import librosa
import numpy as np
import json

def generate_reference(audio_path):
    y, sr = librosa.load(audio_path, sr=22050)

    results = {
        'mel_spectrogram': {
            'mean': librosa.feature.melspectrogram(y=y, sr=sr).mean(axis=1).tolist(),
            'shape': librosa.feature.melspectrogram(y=y, sr=sr).shape
        },
        'mfcc': {
            'mean': librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13).mean(axis=1).tolist()
        },
        'chroma': {
            'mean': librosa.feature.chroma_stft(y=y, sr=sr).mean(axis=1).tolist()
        },
        'tempo': float(librosa.beat.beat_track(y=y, sr=sr)[0]),
        'onset_strength': {
            'mean': float(librosa.onset.onset_strength(y=y, sr=sr).mean()),
            'max': float(librosa.onset.onset_strength(y=y, sr=sr).max())
        }
    }

    return results

# テストファイル用に生成
for audio_file in glob.glob('tests/testdata/*.wav'):
    ref = generate_reference(audio_file)
    with open(f'{audio_file}.reference.json', 'w') as f:
        json.dump(ref, f, indent=2)
```

### C++ 比較テスト

```cpp
TEST_CASE("メルスペクトログラムが librosa と一致", "[librosa]") {
  auto audio = sonare::Audio::from_file("tests/testdata/sine_440hz.wav");
  audio = sonare::resample(audio, 22050);

  sonare::MelConfig config;
  auto mel = sonare::MelSpectrogram::compute(audio, config);

  // 参照値を読み込み
  auto reference = load_json("tests/testdata/sine_440hz.wav.reference.json");

  // メルバンドごとの平均値を比較 (1% の許容誤差)
  auto mel_mean = compute_mean_per_band(mel);
  for (int i = 0; i < mel.n_mels(); ++i) {
    REQUIRE_THAT(mel_mean[i],
                 Catch::Matchers::WithinRel(reference["mel_spectrogram"]["mean"][i], 0.01f));
  }
}
```

### 許容誤差ガイドライン

| 機能 | 許容誤差 | 備考 |
|------|---------|------|
| STFT マグニチュード | < 1e-6 | 浮動小数点精度 |
| メルスペクトログラム | < 1% 相対 | フィルタバンク実装の違い |
| MFCC | < 2% 相対 | DCT 正規化の違い |
| クロマ | < 5% 相対 | ピッチマッピングの違い |
| BPM | ±2 BPM | アルゴリズムの違いは許容 |
| ビート時刻 | ±50ms | 位相のずれ |

---

## 既知の違い

### 1. リサンプリングアルゴリズム

- **librosa**: `resampy` 使用 (Kaiser best)
- **libsonare**: `r8brain-free` 使用 (24ビット品質)

影響: リサンプリング後の高周波成分にわずかな違い。ほとんどの MIR タスクでは無視できる程度。

### 2. CQT (定Q変換)

- **librosa**: 完全な CQT 実装
- **libsonare**: STFT ベースのクロマのみ

CQT ベースのクロマが必要な場合は、前処理に librosa を使用するか、CQT を別途実装してください。

### 3. メルフィルタバンクエッジ

- **librosa**: ビンエッジに床関数を使用
- **libsonare**: 最近傍丸めを使用

影響: 一部のエッジケースで 1-2 ビンの違い。下流の特徴量への影響は最小限。

### 4. 窓関数正規化

- **librosa**: COLA 用に窓を正規化
- **libsonare**: 生の窓値を使用

影響: iSTFT 再構成でわずかな振幅の違いが生じる可能性。修正には `normalize()` を使用。

---

## 移行ガイド

### Python + librosa から C++ へ

**変更前 (Python):**
```python
import librosa

y, sr = librosa.load('audio.mp3', sr=22050)
tempo, beats = librosa.beat.beat_track(y=y, sr=sr)
print(f"BPM: {tempo}")
```

**変更後 (C++):**
```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("audio.mp3");
audio = sonare::resample(audio, 22050);

sonare::BpmAnalyzer analyzer(audio);
std::cout << "BPM: " << analyzer.bpm() << "\n";
```

### Python から JavaScript へ

**変更前 (Python):**
```python
import librosa

y, sr = librosa.load('audio.mp3')
chroma = librosa.feature.chroma_stft(y=y, sr=sr)
key = detect_key_from_chroma(chroma)  # カスタム関数
```

**変更後 (JavaScript):**
```typescript
import { init, detectKey } from '@libraz/sonare';

await init();

// AudioContext からサンプルを取得
const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);
const samples = audioBuffer.getChannelData(0);

const key = detectKey(samples, audioBuffer.sampleRate);
console.log(`キー: ${key.name}`);  // 組み込みキー検出！
```

---

## 参照データの場所

互換性テスト用の参照データ:

```
tests/
├── librosa/
│   ├── reference/
│   │   ├── mel_reference.json
│   │   ├── mfcc_reference.json
│   │   ├── chroma_reference.json
│   │   ├── tempo_reference.json
│   │   └── NOTICE.md
│   └── compare_librosa_test.cpp
└── testdata/
    ├── sine_440hz.wav
    ├── c_major_chord.wav
    └── 120bpm_drums.wav
```

---

## パフォーマンス比較

| 操作 | librosa (Python) | libsonare (C++) | 高速化 |
|------|------------------|-----------------|--------|
| STFT (3分音声) | ~500ms | ~50ms | ~10倍 |
| メルスペクトログラム | ~600ms | ~60ms | ~10倍 |
| MFCC | ~700ms | ~70ms | ~10倍 |
| ビートトラッキング | ~2秒 | ~200ms | ~10倍 |
| フル解析 | ~5秒 | ~500ms | ~10倍 |

*Intel Core i7、22050 Hz の 3 分音声でベンチマーク*

WebAssembly のパフォーマンスはネイティブ C++ より約 2-3 倍遅いですが、それでも Python より高速です。
