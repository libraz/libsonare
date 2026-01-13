# アーキテクチャ

このドキュメントでは、libsonare の内部アーキテクチャ、モジュール構造、データフロー、設計上の決定について説明します。

## モジュール概要

```mermaid
graph TB
    subgraph "API レイヤー"
        WASM["WASM バインディング<br/>(Embind)"]
        CAPI["C API<br/>(sonare_c.h)"]
        QUICK["Quick API<br/>(quick.h)"]
        UNIFIED["統合ヘッダ<br/>(sonare.h)"]
    end

    subgraph "解析レイヤー"
        MUSIC["MusicAnalyzer"]
        BPM["BpmAnalyzer"]
        KEY["KeyAnalyzer"]
        BEAT["BeatAnalyzer"]
        CHORD["ChordAnalyzer"]
        SECTION["SectionAnalyzer"]
        TIMBRE["TimbreAnalyzer"]
        DYNAMICS["DynamicsAnalyzer"]
    end

    subgraph "エフェクトレイヤー"
        HPSS["HPSS"]
        TIMESTRETCH["タイムストレッチ"]
        PITCHSHIFT["ピッチシフト"]
        NORMALIZE["ノーマライズ"]
    end

    subgraph "特徴量レイヤー"
        MEL["メルスペクトログラム"]
        CHROMA["クロマ"]
        CQT["CQT"]
        SPECTRAL["スペクトル特徴量"]
        ONSET["オンセット検出"]
        PITCH["ピッチ追跡"]
    end

    subgraph "コアレイヤー"
        AUDIO["Audio"]
        SPECTRUM["スペクトログラム<br/>(STFT/iSTFT)"]
        FFT["FFT<br/>(KissFFT)"]
        WINDOW["窓関数"]
        CONVERT["単位変換"]
        RESAMPLE["リサンプリング<br/>(r8brain)"]
        AUDIO_IO["Audio I/O<br/>(dr_libs, minimp3)"]
    end

    subgraph "ユーティリティレイヤー"
        TYPES["型定義"]
        MATH["数学ユーティリティ"]
        EXCEPTION["例外"]
    end

    subgraph "フィルタバンク"
        MELFILTER["Mel フィルタバンク"]
        CHROMAFILTER["Chroma フィルタバンク"]
        DCT["DCT"]
        IIR["IIR フィルタ"]
    end

    WASM --> QUICK
    CAPI --> QUICK
    UNIFIED --> MUSIC
    QUICK --> MUSIC

    MUSIC --> BPM
    MUSIC --> KEY
    MUSIC --> BEAT
    MUSIC --> CHORD
    MUSIC --> SECTION
    MUSIC --> TIMBRE
    MUSIC --> DYNAMICS

    BPM --> ONSET
    KEY --> CHROMA
    BEAT --> ONSET
    CHORD --> CHROMA
    SECTION --> MEL
    TIMBRE --> MEL
    DYNAMICS --> AUDIO

    HPSS --> SPECTRUM
    TIMESTRETCH --> SPECTRUM
    PITCHSHIFT --> TIMESTRETCH

    MEL --> SPECTRUM
    MEL --> MELFILTER
    CHROMA --> SPECTRUM
    CHROMA --> CHROMAFILTER
    CQT --> FFT
    SPECTRAL --> SPECTRUM
    ONSET --> MEL
    PITCH --> AUDIO

    SPECTRUM --> FFT
    SPECTRUM --> WINDOW
    AUDIO --> AUDIO_IO
    AUDIO --> RESAMPLE

    FFT --> TYPES
    WINDOW --> TYPES
    CONVERT --> MATH
    MELFILTER --> CONVERT
    CHROMAFILTER --> CONVERT

    classDef api fill:#e1f5fe
    classDef analysis fill:#f3e5f5
    classDef effects fill:#fff3e0
    classDef feature fill:#e8f5e9
    classDef core fill:#fce4ec
    classDef util fill:#f5f5f5
    classDef filter fill:#fff8e1

    class WASM,CAPI,QUICK,UNIFIED api
    class MUSIC,BPM,KEY,BEAT,CHORD,SECTION,TIMBRE,DYNAMICS analysis
    class HPSS,TIMESTRETCH,PITCHSHIFT,NORMALIZE effects
    class MEL,CHROMA,CQT,SPECTRAL,ONSET,PITCH feature
    class AUDIO,SPECTRUM,FFT,WINDOW,CONVERT,RESAMPLE,AUDIO_IO core
    class TYPES,MATH,EXCEPTION util
    class MELFILTER,CHROMAFILTER,DCT,IIR filter
```

---

## ディレクトリ構造

```
src/
├── util/               # レベル 0: 基本ユーティリティ
│   ├── types.h         # MatrixView, ErrorCode, 列挙型
│   ├── exception.h     # SonareException
│   └── math_utils.h    # mean, variance, argmax など
│
├── core/               # レベル 1-3: コア DSP
│   ├── convert.h       # Hz/Mel/MIDI 変換
│   ├── window.h        # Hann, Hamming, Blackman
│   ├── fft.h           # KissFFT ラッパー
│   ├── spectrum.h      # STFT/iSTFT
│   ├── audio.h         # オーディオバッファ
│   ├── audio_io.h      # WAV/MP3 読み込み
│   └── resample.h      # r8brain リサンプリング
│
├── filters/            # レベル 4: フィルタバンク
│   ├── mel.h           # Mel フィルタバンク
│   ├── chroma.h        # Chroma フィルタバンク
│   ├── dct.h           # MFCC 用 DCT
│   └── iir.h           # IIR フィルタ
│
├── feature/            # レベル 4: 特徴量抽出
│   ├── mel_spectrogram.h
│   ├── chroma.h
│   ├── cqt.h
│   ├── vqt.h
│   ├── spectral.h
│   ├── onset.h
│   └── pitch.h
│
├── effects/            # レベル 5: オーディオエフェクト
│   ├── hpss.h          # 調波・打撃音分離
│   ├── time_stretch.h  # フェーズボコーダ タイムストレッチ
│   ├── pitch_shift.h   # ピッチシフト
│   ├── phase_vocoder.h # フェーズボコーダ コア
│   └── normalize.h     # ノーマライズ、トリム
│
├── analysis/           # レベル 6: 音楽解析
│   ├── music_analyzer.h    # ファサード
│   ├── bpm_analyzer.h
│   ├── key_analyzer.h
│   ├── beat_analyzer.h
│   ├── chord_analyzer.h
│   ├── section_analyzer.h
│   ├── timbre_analyzer.h
│   ├── dynamics_analyzer.h
│   ├── rhythm_analyzer.h
│   ├── melody_analyzer.h
│   ├── onset_analyzer.h
│   └── boundary_detector.h
│
├── quick.h             # シンプルな関数 API
├── sonare.h            # 統合インクルードヘッダ
├── sonare_c.h          # C API ヘッダ
└── wasm/
    └── bindings.cpp    # Embind バインディング
```

---

## 依存関係レベル

| レベル | モジュール | 依存先 |
|--------|-----------|--------|
| 0 | util/ | なし (math_utils 以外はヘッダオンリー) |
| 1 | core/convert, core/window | util/ |
| 2 | core/fft | util/, KissFFT |
| 3 | core/spectrum, core/audio | core/fft, core/window |
| 4 | filters/, feature/ | core/ |
| 5 | effects/ | core/, feature/ |
| 6 | analysis/ | feature/, effects/ |

---

## データフロー

### オーディオ解析パイプライン

```mermaid
flowchart LR
    subgraph 入力
        FILE[オーディオファイル<br/>WAV/MP3]
        BUFFER[生バッファ<br/>float*]
    end

    subgraph コア
        AUDIO[Audio]
        STFT[STFT]
        SPEC[スペクトログラム]
    end

    subgraph 特徴量
        MEL[メルスペクトログラム]
        CHROMA[クロマグラム]
        ONSET[オンセット強度]
    end

    subgraph 解析
        BPM[BPM 検出]
        KEY[キー検出]
        BEAT[ビート追跡]
        CHORD[コード認識]
    end

    subgraph 出力
        RESULT[AnalysisResult]
    end

    FILE --> AUDIO
    BUFFER --> AUDIO
    AUDIO --> STFT
    STFT --> SPEC
    SPEC --> MEL
    SPEC --> CHROMA
    MEL --> ONSET
    ONSET --> BPM
    ONSET --> BEAT
    CHROMA --> KEY
    CHROMA --> CHORD
    BPM --> RESULT
    KEY --> RESULT
    BEAT --> RESULT
    CHORD --> RESULT
```

### オーディオエフェクトパイプライン

```mermaid
flowchart LR
    subgraph 入力
        AUDIO[Audio]
    end

    subgraph 変換
        STFT[STFT]
        SPEC[複素<br/>スペクトログラム]
    end

    subgraph エフェクト
        HPSS[HPSS]
        PV[フェーズボコーダ]
    end

    subgraph 処理
        HARM[調波マスク]
        PERC[打撃音マスク]
        STRETCH[タイムストレッチ]
        SHIFT[ピッチシフト]
    end

    subgraph 再構築
        ISTFT[iSTFT]
        RESAMPLE[リサンプル]
    end

    subgraph 出力
        OUT[処理済み Audio]
    end

    AUDIO --> STFT
    STFT --> SPEC
    SPEC --> HPSS
    SPEC --> PV
    HPSS --> HARM
    HPSS --> PERC
    PV --> STRETCH
    STRETCH --> RESAMPLE
    RESAMPLE --> SHIFT
    HARM --> ISTFT
    PERC --> ISTFT
    SHIFT --> ISTFT
    ISTFT --> OUT
```

---

## クラス設計

### MusicAnalyzer (ファサードパターン)

```mermaid
classDiagram
    class MusicAnalyzer {
        -Audio audio_
        -MusicAnalyzerConfig config_
        -unique_ptr~BpmAnalyzer~ bpm_analyzer_
        -unique_ptr~KeyAnalyzer~ key_analyzer_
        -unique_ptr~BeatAnalyzer~ beat_analyzer_
        -unique_ptr~ChordAnalyzer~ chord_analyzer_
        -unique_ptr~SectionAnalyzer~ section_analyzer_
        +MusicAnalyzer(audio, config)
        +bpm() float
        +key() Key
        +beat_times() vector~float~
        +chords() vector~Chord~
        +analyze() AnalysisResult
        +set_progress_callback(callback)
    }

    class BpmAnalyzer {
        -OnsetAnalyzer onset_
        -float bpm_
        -float confidence_
        +BpmAnalyzer(audio, config)
        +bpm() float
        +confidence() float
        +bpm_candidates() vector~float~
    }

    class KeyAnalyzer {
        -Chroma chroma_
        -Key key_
        +KeyAnalyzer(audio, config)
        +key() Key
        +candidates(top_n) vector~KeyCandidate~
    }

    class BeatAnalyzer {
        -OnsetAnalyzer onset_
        -vector~Beat~ beats_
        +BeatAnalyzer(audio, config)
        +beats() vector~Beat~
        +beat_times() vector~float~
        +time_signature() TimeSignature
    }

    class ChordAnalyzer {
        -Chroma chroma_
        -vector~Chord~ chords_
        +ChordAnalyzer(audio, config)
        +chords() vector~Chord~
        +progression_pattern() string
    }

    class SectionAnalyzer {
        -BoundaryDetector detector_
        -vector~Section~ sections_
        +SectionAnalyzer(audio, config)
        +sections() vector~Section~
        +form() string
    }

    MusicAnalyzer --> BpmAnalyzer : 遅延生成
    MusicAnalyzer --> KeyAnalyzer : 遅延生成
    MusicAnalyzer --> BeatAnalyzer : 遅延生成
    MusicAnalyzer --> ChordAnalyzer : 遅延生成
    MusicAnalyzer --> SectionAnalyzer : 遅延生成
```

### Audio バッファ (共有所有権)

```mermaid
classDiagram
    class Audio {
        -shared_ptr~AudioData~ data_
        -size_t offset_
        -size_t size_
        -int sample_rate_
        +from_file(path) Audio
        +from_buffer(ptr, size, sr) Audio
        +from_vector(vec, sr) Audio
        +slice(start, end) Audio
        +data() const float*
        +size() size_t
        +duration() float
    }

    class AudioData {
        -vector~float~ samples_
        +samples() const float*
        +size() size_t
    }

    Audio --> AudioData : shared_ptr
    Audio --> Audio : slice (ゼロコピー)
```

---

## 主要な設計上の決定

### 1. 遅延初期化

MusicAnalyzer は個々のアナライザーに遅延初期化を使用します。これにより：
- 必要なものだけを計算
- アナライザー間で中間結果を共有
- 機能のサブセットのみ使用時のメモリ削減

```cpp
// BPM のみが計算される
float bpm = analyzer.bpm();

// キー検出はクロマ計算をトリガー
Key key = analyzer.key();

// フル解析はすべてを計算
AnalysisResult result = analyzer.analyze();
```

### 2. ゼロコピー オーディオスライシング

Audio はオフセット/サイズ付きの `shared_ptr` を使用してゼロコピースライシングを実現：

```cpp
auto full = Audio::from_file("song.mp3");  // 10 MB

// 両方とも同じ基礎バッファを共有
auto intro = full.slice(0, 30);     // 0-30 秒、ゼロコピー
auto chorus = full.slice(60, 90);   // 60-90 秒、ゼロコピー
```

### 3. WASM 互換性

コアモジュールは以下を回避：
- ファイル I/O (Audio I/O レイヤーで処理)
- スレッディング (シングルスレッド実行)
- 動的ローディング
- システム固有の API

すべての外部依存関係は以下のいずれか：
- ヘッダオンリー (Eigen3)
- 静的リンク (KissFFT, dr_libs, minimp3, r8brain)

### 4. librosa 互換性

デフォルトパラメータは librosa と一致し、移行を容易にします：

| パラメータ | デフォルト | librosa デフォルト |
|-----------|-----------|-------------------|
| sample_rate | 22050 | 22050 |
| n_fft | 2048 | 2048 |
| hop_length | 512 | 512 |
| n_mels | 128 | 128 |
| fmin | 0 | 0 |
| fmax | sr/2 | sr/2 |

Mel スケールは Slaney 式 (librosa デフォルト) を使用。

---

## サードパーティライブラリ

| ライブラリ | 場所 | 用途 | ライセンス |
|-----------|------|------|-----------|
| KissFFT | third_party/kissfft/ | FFT | BSD-3-Clause |
| Eigen3 | System/FetchContent | 行列演算 | MPL-2.0 |
| dr_libs | third_party/dr_libs/ | WAV デコード | Public Domain |
| minimp3 | third_party/minimp3/ | MP3 デコード | CC0-1.0 |
| r8brain | third_party/r8brain/ | リサンプリング | MIT |
| Catch2 | FetchContent | テスト | BSL-1.0 |

---

## パフォーマンス考慮事項

### メモリレイアウト

- スペクトログラムは列優先で格納 (周波数 x 時間)
- Eigen のデフォルトレイアウトと互換
- 周波数方向の演算に効率的

### キャッシング

- スペクトログラムは初回アクセス時にマグニチュード/パワーをキャッシュ
- MelSpectrogram はフィルタバンク行列を再利用
- Chroma はクロマフィルタバンクを再利用

### 並列化

- WASM 互換性のためシングルスレッド
- ネイティブビルドではフレームレベルの並列化が可能 (将来)
