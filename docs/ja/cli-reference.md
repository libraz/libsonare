# CLI リファレンス

`sonare` コマンドラインインターフェースの完全なリファレンス。

## 概要

sonare CLI は、コマンドラインから包括的な音楽解析、オーディオ処理、特徴量抽出機能を提供します。

```bash
sonare <コマンド> [オプション] <オーディオファイル> [-o 出力先]
```

## グローバルオプション

| オプション | 説明 |
|------------|------|
| `--json` | JSON 形式で出力 |
| `--quiet`, `-q` | 進捗表示を抑制 |
| `--help`, `-h` | ヘルプを表示 |
| `-o`, `--output` | 出力ファイルパス |
| `--n-fft <int>` | FFT サイズ (デフォルト: 2048) |
| `--hop-length <int>` | ホップ長 (デフォルト: 512) |
| `--n-mels <int>` | メルバンド数 (デフォルト: 128) |
| `--fmin <float>` | 最小周波数 (Hz) |
| `--fmax <float>` | 最大周波数 (Hz) |

---

## 解析コマンド

### analyze

BPM、キー、ビート、コード、セクション、音色、ダイナミクス、リズムを含む完全な音楽解析を実行します。

```bash
sonare analyze music.mp3
sonare analyze music.mp3 --json
```

**出力:**
```
BPM: 120.5 (confidence: 0.95)
Key: C major (confidence: 0.85)
Time Signature: 4/4
Beats: 240
Sections: Intro (0-8s), Verse (8-32s), Chorus (32-48s)
Form: IABABCO
```

**JSON 出力:**
```json
{
  "bpm": 120.5,
  "bpmConfidence": 0.95,
  "key": {
    "root": 0,
    "mode": 0,
    "confidence": 0.85,
    "name": "C major"
  },
  "timeSignature": {
    "numerator": 4,
    "denominator": 4,
    "confidence": 0.9
  },
  "beats": [...],
  "chords": [...],
  "sections": [...],
  "timbre": {...},
  "dynamics": {...},
  "rhythm": {...},
  "form": "IABABCO"
}
```

---

### bpm

テンポ (BPM) のみを検出します。

```bash
sonare bpm music.mp3
sonare bpm music.wav --json
```

**出力:**
```
BPM: 128.0
```

---

### key

音楽のキー (調) を検出します。

```bash
sonare key music.mp3
sonare key music.mp3 --json
```

**出力:**
```
Key: A minor (confidence: 0.82)
```

**JSON 出力:**
```json
{"root": 9, "mode": 1, "confidence": 0.82, "name": "A minor"}
```

---

### beats

ビート時刻を検出します。

```bash
sonare beats music.mp3
sonare beats music.mp3 --json
```

**出力:**
```
Beat times (240 beats):
0.52, 1.02, 1.52, 2.02, 2.52, 3.02, 3.52, 4.02, 4.52, 5.02,
...
```

**JSON 出力:**
```json
[0.52, 1.02, 1.52, 2.02, ...]
```

---

### onsets

オンセット時刻（音の立ち上がり）を検出します。

```bash
sonare onsets music.mp3
sonare onsets music.mp3 --json
```

---

### chords

コード進行を検出します。

```bash
sonare chords music.mp3
sonare chords music.mp3 --min-duration 0.5 --threshold 0.3
sonare chords music.mp3 --triads-only
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--min-duration` | 0.3 | 最小コード持続時間（秒） |
| `--threshold` | 0.5 | 最小信頼度閾値 |
| `--triads-only` | false | 三和音のみを検出（メジャー/マイナー/ディミニッシュ/オーグメント） |

**出力:**
```
Chord progression: C - G - Am - F
Duration: 180.5s, 48 chord changes

Time      Chord    Confidence
0.00s     C        0.85
4.02s     G        0.78
8.04s     Am       0.82
12.06s    F        0.80
```

---

### sections

楽曲構造（イントロ、ヴァース、コーラスなど）を検出します。

```bash
sonare sections music.mp3
sonare sections music.mp3 --min-duration 4.0 --threshold 0.3
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--min-duration` | 4.0 | 最小セクション持続時間（秒） |
| `--threshold` | 0.3 | 境界検出閾値 |

**出力:**
```
Form: IABABCO
Duration: 180.5s, 7 sections

Section   Type         Start    End      Energy
1         Intro        0.00s    8.52s    0.45
2         Verse        8.52s    32.10s   0.62
3         Chorus       32.10s   48.20s   0.85
```

---

### timbre

音色特性を解析します。

```bash
sonare timbre music.mp3
sonare timbre music.mp3 --json
```

**出力:**
```
Timbre Analysis:
  Brightness:  0.65 (bright)
  Warmth:      0.42 (neutral)
  Density:     0.78 (rich)
  Roughness:   0.25 (smooth)
  Complexity:  0.55 (moderate)
```

---

### dynamics

ダイナミクスとラウドネスを解析します。

```bash
sonare dynamics music.mp3
sonare dynamics music.mp3 --window-sec 0.4
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--window-sec` | 0.4 | 解析ウィンドウ（秒） |

**出力:**
```
Dynamics Analysis:
  Peak Level:       -0.5 dB
  RMS Level:        -12.3 dB
  Dynamic Range:    15.2 dB
  Crest Factor:     11.8 dB
  Loudness Range:   8.5 LU
  Compression:      No (natural)
```

---

### rhythm

リズム特徴を解析します。

```bash
sonare rhythm music.mp3
sonare rhythm music.mp3 --bpm-min 60 --bpm-max 200
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--start-bpm` | 120.0 | 初期 BPM 推定値 |
| `--bpm-min` | 60.0 | BPM 検索範囲の最小値 |
| `--bpm-max` | 200.0 | BPM 検索範囲の最大値 |

**出力:**
```
Rhythm Analysis:
  Time Signature:    4/4 (confidence: 0.92)
  BPM:               128.0
  Groove Type:       straight
  Syncopation:       0.35 (moderate)
  Pattern Regularity: 0.85 (regular)
  Tempo Stability:   0.92 (stable)
```

---

### melody

メロディとピッチ輪郭を追跡します。

```bash
sonare melody music.mp3
sonare melody music.mp3 --fmin 80 --fmax 1000 --threshold 0.1
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--fmin` | 80.0 | 最小周波数 (Hz) |
| `--fmax` | 1000.0 | 最大周波数 (Hz) |
| `--threshold` | 0.1 | 有声音閾値 |

**出力:**
```
Melody Analysis:
  Has Melody:      Yes
  Pitch Range:     1.52 octaves
  Mean Frequency:  320.5 Hz
  Pitch Stability: 0.78
  Vibrato Rate:    5.2 Hz
  Pitch Points:    4520
```

---

### boundaries

構造的境界を検出します。

```bash
sonare boundaries music.mp3
sonare boundaries music.mp3 --threshold 0.3 --min-distance 2.0
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--threshold` | 0.3 | 検出閾値 |
| `--kernel-size` | 64 | チェッカーボードカーネルサイズ |
| `--min-distance` | 2.0 | 境界間の最小距離（秒） |

**出力:**
```
Structural Boundaries (6 detected):
Time      Strength
8.52s     0.85
32.10s    0.92
48.20s    0.78
```

---

## 処理コマンド

### pitch-shift

テンポを変えずにピッチを半音単位でシフトします。

```bash
sonare pitch-shift --semitones 3 input.wav -o output.wav
sonare pitch-shift --semitones -5 input.mp3 -o lower.wav
```

**オプション:**

| オプション | 必須 | 説明 |
|------------|------|------|
| `--semitones` | はい | 半音数（正 = 上げる、負 = 下げる） |

---

### time-stretch

ピッチを変えずにテンポを変更します。

```bash
sonare time-stretch --rate 0.5 input.wav -o slower.wav   # 半分の速度
sonare time-stretch --rate 2.0 input.wav -o faster.wav   # 2倍速
sonare time-stretch --rate 1.25 input.mp3 -o output.wav  # 25%速く
```

**オプション:**

| オプション | 必須 | 説明 |
|------------|------|------|
| `--rate` | はい | ストレッチ率（0.5 = 半分の速度、2.0 = 2倍速） |

---

### hpss

調波・打撃音分離（Harmonic-Percussive Source Separation）。

```bash
sonare hpss input.wav -o separated           # separated_harmonic.wav と separated_percussive.wav を作成
sonare hpss input.wav -o output --with-residual  # output_residual.wav も作成
sonare hpss input.wav -o harmonic.wav --harmonic-only
sonare hpss input.wav -o percussive.wav --percussive-only
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--kernel-harmonic` | 31 | 水平カーネルサイズ（時間方向） |
| `--kernel-percussive` | 31 | 垂直カーネルサイズ（周波数方向） |
| `--hard-mask` | false | ソフトマスクの代わりにハードマスクを使用 |
| `--with-residual` | false | 残差成分も出力 |
| `--harmonic-only` | false | 調波成分のみを出力 |
| `--percussive-only` | false | 打撃音成分のみを出力 |

---

## 特徴量コマンド

### mel

メルスペクトログラムの統計を計算します。

```bash
sonare mel music.mp3
sonare mel music.mp3 --n-mels 80 --fmin 50 --fmax 8000
```

**出力:**
```
Mel Spectrogram:
  Shape:       128 bands x 8520 frames
  Duration:    180.52s
  Sample Rate: 22050 Hz
  Stats:       min=0.0001, max=0.8520, mean=0.0452
```

---

### chroma

クロマグラム（ピッチクラス分布）を計算します。

```bash
sonare chroma music.mp3
sonare chroma music.mp3 --json
```

**出力:**
```
Chromagram:
  Shape:    12 bins x 8520 frames
  Duration: 180.52s

Mean Energy by Pitch Class:
  C : 0.125 *************
  C#: 0.045 *****
  D : 0.082 ********
  ...
```

---

### spectral

スペクトル特徴量（セントロイド、帯域幅、ロールオフ、フラットネス、ZCR、RMS）を計算します。

```bash
sonare spectral music.mp3
sonare spectral music.mp3 --json
```

**出力:**
```
Spectral Features (8520 frames):
  Feature          Mean       Std        Min        Max
  centroid         2150.5     850.2      120.5      8500.0
  bandwidth        1850.2     520.8      50.2       4200.5
  rolloff          4520.8     1200.5     200.0      10000.0
  flatness         0.0250     0.0180     0.0010     0.1520
  zcr              0.0850     0.0420     0.0020     0.2500
  rms              0.0520     0.0280     0.0001     0.1850
```

---

### pitch

YIN または pYIN アルゴリズムでピッチを追跡します。

```bash
sonare pitch music.mp3
sonare pitch music.mp3 --algorithm yin --fmin 65 --fmax 2093
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--algorithm` | pyin | ピッチアルゴリズム: "yin" または "pyin" |
| `--fmin` | 65.0 | 最小周波数 (Hz) |
| `--fmax` | 2093.0 | 最大周波数 (Hz) |
| `--threshold` | 0.3 | 信頼度閾値 |

**出力:**
```
Pitch Tracking (pyin):
  Frames:    8520
  Voiced:    6250 (73.4%)
  Median F0: 285.5 Hz
  Mean F0:   302.8 Hz
```

---

### onset-env

オンセット強度エンベロープを計算します。

```bash
sonare onset-env music.mp3
sonare onset-env music.mp3 --json
```

**出力:**
```
Onset Strength Envelope:
  Frames:       8520
  Duration:     180.52s
  Peak Time:    45.28s
  Peak Strength: 0.952
  Mean:         0.125
```

---

### cqt

定Q変換（Constant-Q Transform）を計算します。

```bash
sonare cqt music.mp3
sonare cqt music.mp3 --fmin 32.7 --n-bins 84 --bins-per-octave 12
```

**オプション:**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--fmin` | 32.7 | 最小周波数 (Hz) - デフォルトは C1 |
| `--n-bins` | 84 | 周波数ビン数 |
| `--bins-per-octave` | 12 | オクターブあたりのビン数（12 = 半音分解能） |

**出力:**
```
Constant-Q Transform:
  Shape:          84 bins x 8520 frames
  Frequency Range: 32.7 - 4186.0 Hz (7 octaves)
  Duration:       180.52s
```

---

## ユーティリティコマンド

### info

オーディオファイル情報を表示します。

```bash
sonare info music.mp3
sonare info music.wav --json
```

**出力:**
```
Audio File: music.mp3
  Duration:    3:00.5 (180.52s)
  Sample Rate: 22050 Hz
  Samples:     3980000
  Peak Level:  -0.5 dB
  RMS Level:   -12.3 dB
```

---

### version

バージョン情報を表示します。

```bash
sonare version
sonare version --json
```

**出力:**
```
sonare-cli version 1.0.0
libsonare version 1.0.0
```

---

## 使用例

### 基本的な解析ワークフロー

```bash
# BPM とキーをクイックチェック
sonare bpm song.mp3
sonare key song.mp3

# スクリプト用に JSON 出力で完全解析
sonare analyze song.mp3 --json > analysis.json

# コード進行を検出
sonare chords song.mp3 --min-duration 0.5
```

### オーディオ処理ワークフロー

```bash
# 2半音上げる
sonare pitch-shift --semitones 2 original.wav -o transposed.wav

# 練習用に遅くする（80%速度）
sonare time-stretch --rate 0.8 song.wav -o practice.wav

# ドラムとメロディを分離
sonare hpss song.wav -o separated
# 作成: separated_harmonic.wav, separated_percussive.wav
```

### 機械学習用の特徴量抽出

```bash
# 機械学習用に特徴量を抽出
sonare mel song.mp3 --json > mel_features.json
sonare spectral song.mp3 --json > spectral_features.json
sonare chroma song.mp3 --json > chroma_features.json
```

### バッチ処理

```bash
# ディレクトリ内のすべての MP3 ファイルを解析
for f in *.mp3; do
  echo "Processing: $f"
  sonare analyze "$f" --json > "${f%.mp3}.json"
done

# すべてのファイルから BPM を抽出
for f in *.wav; do
  bpm=$(sonare bpm "$f" --json | jq -r '.bpm')
  echo "$f: $bpm BPM"
done
```

---

## 終了コード

| コード | 説明 |
|--------|------|
| 0 | 成功 |
| 1 | エラー（無効な引数、ファイルが見つからない、処理エラー） |

---

## 対応オーディオ形式

| 形式 | 拡張子 | 備考 |
|------|--------|------|
| WAV | `.wav` | 非圧縮 PCM |
| MP3 | `.mp3` | minimp3 でデコード |

---

## パフォーマンスのヒント

1. **大きなファイル**: 10分以上のファイルの場合、先に分割を検討:
   ```bash
   # 最初の60秒のみを解析（ffmpeg使用）
   ffmpeg -i long_song.mp3 -t 60 sample.wav
   sonare analyze sample.wav
   ```

2. **バッチ処理**: `--quiet` で出力オーバーヘッドを削減:
   ```bash
   sonare analyze song.mp3 --quiet --json > result.json
   ```

3. **FFT サイズ**: 小さい FFT サイズ（`--n-fft 1024`）は高速だが周波数分解能が低下。

4. **ホップ長**: 大きいホップ長（`--hop-length 1024`）は高速だが時間分解能が低下。
