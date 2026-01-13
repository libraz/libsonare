# C++ API リファレンス

libsonare C++ インターフェースの完全な API リファレンス。

## 目次

- [名前空間](#名前空間)
- [コアクラス](#コアクラス)
- [特徴量抽出](#特徴量抽出)
- [エフェクト](#エフェクト)
- [解析](#解析)
- [Quick API](#quick-api)
- [C API](#c-api)
- [ユーティリティ関数](#ユーティリティ関数)
- [列挙型](#列挙型)
- [構造体](#構造体)

> アーキテクチャ図とデータフローについては、[アーキテクチャ](architecture.md)を参照してください。

---

## 名前空間

すべての libsonare 機能は `sonare` 名前空間に含まれています。

```cpp
namespace sonare {
  // コアクラス
  class Audio;
  class Spectrogram;

  // 特徴量抽出
  class MelSpectrogram;
  class Chroma;

  // 解析
  class MusicAnalyzer;
  class BpmAnalyzer;
  class KeyAnalyzer;
  // ... 等

  namespace quick {
    // シンプルな関数 API
  }
}
```

---

## コアクラス

### Audio

共有所有権とゼロコピースライシング対応のオーディオバッファ。

**ヘッダ:** `#include <sonare/core/audio.h>`

#### ファクトリメソッド

```cpp
/// 生のサンプルバッファから Audio を作成。
/// @param samples float サンプルへのポインタ (コピーされる)
/// @param size サンプル数
/// @param sample_rate サンプルレート (Hz)
static Audio Audio::from_buffer(const float* samples, size_t size, int sample_rate);

/// ベクターから Audio を作成 (所有権移転)。
/// @param samples サンプルのベクター (ムーブされる)
/// @param sample_rate サンプルレート (Hz)
static Audio Audio::from_vector(std::vector<float> samples, int sample_rate);

/// ファイルから Audio を読み込み (WAV, MP3)。
/// @param path ファイルパス
/// @throws SonareException ファイルが見つからないまたはデコードエラー時
static Audio Audio::from_file(const std::string& path);

/// メモリバッファから Audio を読み込み。
/// @param data オーディオファイルデータへのポインタ
/// @param size バイト数
/// @throws SonareException デコードエラー時
static Audio Audio::from_memory(const uint8_t* data, size_t size);
```

#### プロパティ

```cpp
const float* data() const;        // サンプルデータへのポインタ
size_t size() const;              // サンプル数
int sample_rate() const;          // サンプルレート (Hz)
float duration() const;           // 秒単位の長さ
int channels() const;             // 常に 1 (モノラル)
bool empty() const;               // サンプルがない場合 true
```

#### 操作

```cpp
/// スライスを作成 (ゼロコピー、バッファ共有)。
/// @param start_time 開始時間 (秒)
/// @param end_time 終了時間 (秒) (-1 = オーディオの終端)
Audio slice(float start_time, float end_time = -1.0f) const;

/// サンプルインデックスでスライスを作成。
/// @param start_sample 開始サンプルインデックス
/// @param end_sample 終了サンプルインデックス (-1 = 終端)
Audio slice_samples(size_t start_sample, size_t end_sample = -1) const;

/// モノラル版を返す (既にモノラルなのでコピーを返す)。
Audio to_mono() const;

/// サンプルアクセス用のインデックス演算子。
float operator[](size_t index) const;

/// イテレータサポート。
const float* begin() const;
const float* end() const;
```

#### 例

```cpp
#include <sonare/sonare.h>

// ファイルから読み込み
auto audio = sonare::Audio::from_file("song.mp3");
std::cout << "長さ: " << audio.duration() << "秒\n";
std::cout << "サンプルレート: " << audio.sample_rate() << " Hz\n";

// ゼロコピースライシング
auto intro = audio.slice(0.0f, 30.0f);  // 最初の30秒
auto chorus = audio.slice(60.0f, 90.0f);  // 60-90秒

// 両方とも同じ基盤バッファを共有
assert(intro.data() == audio.data());  // 同じポインタ!
```

---

### Spectrogram

オーディオ信号の短時間フーリエ変換 (STFT)。

**ヘッダ:** `#include <sonare/core/spectrum.h>`

#### 設定

```cpp
struct StftConfig {
  int n_fft = 2048;                      // FFT サイズ
  int hop_length = 512;                  // フレーム間ホップ長
  int win_length = 0;                    // 窓長 (0 = n_fft)
  WindowType window = WindowType::Hann;  // 窓関数
  bool center = true;                    // フレームを中央に配置するためパディング

  int actual_win_length() const;         // win_length または n_fft を返す
};
```

#### ファクトリメソッド

```cpp
/// オーディオの STFT を計算。
/// @param audio 入力オーディオ
/// @param config STFT 設定
/// @param progress_callback オプションの進捗コールバック (0.0 〜 1.0)
static Spectrogram compute(
  const Audio& audio,
  const StftConfig& config = StftConfig(),
  SpectrogramProgressCallback progress_callback = nullptr
);

/// 既存の複素スペクトルデータから作成。
static Spectrogram from_complex(
  const std::complex<float>* data,
  int n_bins, int n_frames,
  int n_fft, int hop_length, int sample_rate
);
```

#### プロパティ

```cpp
int n_bins() const;        // 周波数ビン数 (n_fft/2 + 1)
int n_frames() const;      // 時間フレーム数
int n_fft() const;         // FFT サイズ
int hop_length() const;    // ホップ長
int sample_rate() const;   // 元のサンプルレート
float duration() const;    // 秒単位の長さ
bool empty() const;        // 空の場合 true
```

#### データアクセス

```cpp
/// 複素スペクトルビュー [n_bins x n_frames]。
MatrixView<std::complex<float>> complex_view() const;

/// 生の複素データポインタ。
const std::complex<float>* complex_data() const;

/// マグニチュードスペクトル (遅延計算、キャッシュ)。
const std::vector<float>& magnitude() const;

/// パワースペクトル (遅延計算、キャッシュ)。
const std::vector<float>& power() const;

/// dB 単位のマグニチュード。
/// @param ref 基準値 (デフォルト 1.0)
/// @param amin 最小振幅 (デフォルト 1e-10)
std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

/// (bin, frame) の複素値にアクセス。
const std::complex<float>& at(int bin, int frame) const;
```

#### 再構成

```cpp
/// iSTFT によりオーディオを再構成。
/// @param length 目標サンプル長 (0 = 自動)
/// @param window 合成用窓関数
Audio to_audio(int length = 0, WindowType window = WindowType::Hann) const;
```

---

## 特徴量抽出

### MelSpectrogram

メル周波数スペクトログラムと MFCC 抽出。

**ヘッダ:** `#include <sonare/feature/mel_spectrogram.h>`

#### 設定

```cpp
struct MelConfig {
  int n_mels = 128;       // メルバンド数
  float fmin = 0.0f;      // 最小周波数
  float fmax = 0.0f;      // 最大周波数 (0 = sr/2)
  StftConfig stft;        // STFT 設定
};
```

#### メソッド

```cpp
/// オーディオからメルスペクトログラムを計算。
static MelSpectrogram compute(const Audio& audio, const MelConfig& config = MelConfig());

/// 既存のスペクトログラムから計算。
static MelSpectrogram from_spectrogram(
  const Spectrogram& spec,
  int sample_rate,
  const MelFilterConfig& config = MelFilterConfig()
);

/// メルパワースペクトル [n_mels x n_frames] を返す。
MatrixView<float> power() const;

/// dB 単位のメルスペクトルを返す。
std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

/// MFCC を計算。
/// @param n_mfcc 係数の数 (デフォルト 13)
/// @param lifter リフタリング係数 (0 = リフタリングなし)
/// @return MFCC 係数 [n_mfcc x n_frames]
std::vector<float> mfcc(int n_mfcc = 13, float lifter = 0.0f) const;

/// デルタ特徴量を計算。
/// @param data 入力特徴量
/// @param width デルタウィンドウ幅
static std::vector<float> delta(const std::vector<float>& data, int n_features, int n_frames, int width = 9);

int n_mels() const;
int n_frames() const;
```

---

### Chroma

クロマグラム (ピッチクラスエネルギー分布)。

**ヘッダ:** `#include <sonare/feature/chroma.h>`

```cpp
struct ChromaConfig {
  int n_chroma = 12;         // クロマビン数 (通常 12)
  float tuning = 0.0f;       // チューニングオフセット (セント)
  StftConfig stft;           // STFT 設定
};

class Chroma {
public:
  static Chroma compute(const Audio& audio, const ChromaConfig& config = ChromaConfig());

  static Chroma from_spectrogram(
    const Spectrogram& spec,
    int sample_rate,
    const ChromaConfig& config = ChromaConfig()
  );

  /// クロマ特徴量 [12 x n_frames]。
  MatrixView<float> features() const;

  /// ピッチクラスごとの平均エネルギー。
  std::array<float, 12> mean_energy() const;

  /// フレームごとの支配的ピッチクラス。
  std::vector<int> dominant_pitch_class() const;

  int n_frames() const;
};
```

---

## エフェクト

### HPSS (ハーモニック・パーカッシブ音源分離)

**ヘッダ:** `#include <sonare/effects/hpss.h>`

```cpp
struct HpssConfig {
  int kernel_size_harmonic = 31;    // 水平 (時間) カーネルサイズ
  int kernel_size_percussive = 31;  // 垂直 (周波数) カーネルサイズ
  float power = 2.0f;               // マスク強調パワー
  float margin_harmonic = 1.0f;     // ハーモニックマージン
  float margin_percussive = 1.0f;   // パーカッシブマージン
};

struct HpssAudioResult {
  Audio harmonic;
  Audio percussive;
};

/// オーディオに対する HPSS (便利関数)。
HpssAudioResult hpss(
  const Audio& audio,
  const HpssConfig& config = HpssConfig(),
  const StftConfig& stft_config = StftConfig()
);

/// ハーモニック成分のみ抽出。
Audio harmonic(const Audio& audio, const HpssConfig& config = HpssConfig());

/// パーカッシブ成分のみ抽出。
Audio percussive(const Audio& audio, const HpssConfig& config = HpssConfig());
```

---

### タイムストレッチ

**ヘッダ:** `#include <sonare/effects/time_stretch.h>`

```cpp
/// ピッチを変えずにオーディオを時間伸縮。
/// @param audio 入力オーディオ
/// @param rate 伸縮率 (0.5 = 半分の速度、2.0 = 2倍の速度)
Audio time_stretch(
  const Audio& audio,
  float rate,
  const TimeStretchConfig& config = TimeStretchConfig()
);
```

---

### ピッチシフト

**ヘッダ:** `#include <sonare/effects/pitch_shift.h>`

```cpp
/// 半音単位でピッチをシフト。
/// @param audio 入力オーディオ
/// @param semitones 半音数 (正 = 上、負 = 下)
Audio pitch_shift(
  const Audio& audio,
  float semitones,
  const PitchShiftConfig& config = PitchShiftConfig()
);

/// 周波数比率でピッチをシフト。
/// @param ratio ピッチ比率 (2.0 = 1オクターブ上)
Audio pitch_shift_ratio(
  const Audio& audio,
  float ratio,
  const PitchShiftConfig& config = PitchShiftConfig()
);
```

---

## 解析

### MusicAnalyzer

遅延初期化による統合音楽解析ファサード。

**ヘッダ:** `#include <sonare/analysis/music_analyzer.h>`

#### 設定

```cpp
struct MusicAnalyzerConfig {
  int n_fft = 2048;
  int hop_length = 512;
  float bpm_min = 60.0f;
  float bpm_max = 200.0f;
  float start_bpm = 120.0f;
};
```

#### コンストラクタ

```cpp
explicit MusicAnalyzer(
  const Audio& audio,
  const MusicAnalyzerConfig& config = MusicAnalyzerConfig()
);
```

#### 進捗コールバック

```cpp
using ProgressCallback = std::function<void(float progress, const char* stage)>;

void set_progress_callback(ProgressCallback callback);
```

進捗ステージ:
- `"bpm"` - BPM 解析
- `"key"` - キー検出
- `"beats"` - ビートトラッキング
- `"chords"` - コード認識
- `"sections"` - セクション検出
- `"timbre"` - 音色解析
- `"dynamics"` - ダイナミクス解析

#### クイックアクセスメソッド

```cpp
float bpm();                      // 推定 BPM
Key key();                        // 検出キー
std::vector<float> beat_times();  // 秒単位のビート位置
std::vector<Chord> chords();      // コード進行
std::string form();               // 楽曲形式文字列 ("IABABCO")
```

#### アナライザーアクセス (遅延初期化)

```cpp
BpmAnalyzer& bpm_analyzer();
KeyAnalyzer& key_analyzer();
BeatAnalyzer& beat_analyzer();
ChordAnalyzer& chord_analyzer();
OnsetAnalyzer& onset_analyzer();
DynamicsAnalyzer& dynamics_analyzer();
RhythmAnalyzer& rhythm_analyzer();
TimbreAnalyzer& timbre_analyzer();
MelodyAnalyzer& melody_analyzer();
SectionAnalyzer& section_analyzer();
BoundaryDetector& boundary_detector();
```

#### フル解析

```cpp
AnalysisResult analyze();
```

---

## Quick API

一般的な解析タスク用のシンプルなステートレス関数。

**ヘッダ:** `#include <sonare/quick.h>`

```cpp
namespace sonare::quick {

/// BPM を検出。
/// @param samples モノラル float32 サンプルへのポインタ
/// @param size サンプル数
/// @param sample_rate サンプルレート (Hz)
/// @return 推定 BPM
float detect_bpm(const float* samples, size_t size, int sample_rate);

/// 音楽キーを検出。
Key detect_key(const float* samples, size_t size, int sample_rate);

/// オンセット時刻を検出。
/// @return 秒単位のオンセット時刻のベクター
std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate);

/// ビート時刻を検出。
/// @return 秒単位のビート時刻のベクター
std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate);

/// 完全な解析を実行。
AnalysisResult analyze(const float* samples, size_t size, int sample_rate);

}
```

---

## C API

FFI 統合用の C 互換 API。

**ヘッダ:** `#include <sonare_c.h>`

```c
// エラーコード
typedef enum {
  SONARE_OK = 0,
  SONARE_ERROR_FILE_NOT_FOUND,
  SONARE_ERROR_INVALID_FORMAT,
  SONARE_ERROR_DECODE_FAILED,
  SONARE_ERROR_INVALID_PARAMETER,
  SONARE_ERROR_OUT_OF_MEMORY,
} SonareError;

// オーディオ関数
SonareError sonare_audio_from_buffer(
  const float* data, size_t len, int sample_rate, SonareAudio** out
);
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
void sonare_audio_free(SonareAudio* audio);

// 解析関数
SonareError sonare_detect_bpm(
  const SonareAudio* audio, float* out_bpm, float* out_confidence
);
SonareError sonare_detect_key(const SonareAudio* audio, SonareKey* out_key);
SonareError sonare_detect_beats(
  const SonareAudio* audio, float** out_times, size_t* out_count
);
SonareError sonare_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// メモリ管理
void sonare_free_floats(float* ptr);
void sonare_free_result(SonareAnalysisResult* result);

// ユーティリティ
const char* sonare_error_message(SonareError error);
const char* sonare_version(void);
```

---

## ユーティリティ関数

### 単位変換

**ヘッダ:** `#include <sonare/core/convert.h>`

```cpp
// Hz <-> Mel (Slaney 式、librosa デフォルト)
float hz_to_mel(float hz);
float mel_to_hz(float mel);

// Hz <-> Mel (HTK 式)
float hz_to_mel_htk(float hz);
float mel_to_hz_htk(float mel);

// Hz <-> MIDI ノート番号 (A4 = 440Hz = 69)
float hz_to_midi(float hz);
float midi_to_hz(float midi);

// Hz <-> ノート名
std::string hz_to_note(float hz);     // 例: "A4", "C#5"
float note_to_hz(const std::string& note);

// 時間 <-> フレーム
float frames_to_time(int frames, int sample_rate, int hop_length);
int time_to_frames(float time, int sample_rate, int hop_length);

// サンプル <-> 時間
float samples_to_time(int samples, int sample_rate);
int time_to_samples(float time, int sample_rate);

// FFT ビン <-> Hz
float bin_to_hz(int bin, int sample_rate, int n_fft);
int hz_to_bin(float hz, int sample_rate, int n_fft);
```

---

## 列挙型

```cpp
enum class PitchClass {
  C = 0, Cs = 1, D = 2, Ds = 3, E = 4, F = 5,
  Fs = 6, G = 7, Gs = 8, A = 9, As = 10, B = 11
};

enum class Mode {
  Major,
  Minor
};

enum class ChordQuality {
  Major,
  Minor,
  Diminished,
  Augmented,
  Dominant7,
  Major7,
  Minor7,
  Sus2,
  Sus4
};

enum class SectionType {
  Intro,
  Verse,
  PreChorus,
  Chorus,
  Bridge,
  Instrumental,
  Outro
};

enum class WindowType {
  Hann,
  Hamming,
  Blackman,
  Rectangular
};
```

---

## 構造体

### Key

```cpp
struct Key {
  PitchClass root;
  Mode mode;
  float confidence;  // 0.0 - 1.0

  std::string to_string() const;       // "C major", "A minor"
  std::string short_name() const;      // "C", "Am"
};
```

### Beat

```cpp
struct Beat {
  float time;      // 秒単位の時間
  int frame;       // フレームインデックス
  float strength;  // ビート強度 (0.0 - 1.0)
};
```

### Chord

```cpp
struct Chord {
  PitchClass root;
  ChordQuality quality;
  float start;       // 開始時間 (秒)
  float end;         // 終了時間 (秒)
  float confidence;

  std::string to_string() const;  // "C", "Am", "G7", "Bdim"
};
```

### Section

```cpp
struct Section {
  SectionType type;
  float start;
  float end;
  float energy_level;  // 相対エネルギー (0.0 - 1.0)
  float confidence;

  std::string to_string() const;  // "Verse (0.0-15.2s)"
};
```

### AnalysisResult

```cpp
struct AnalysisResult {
  float bpm;
  float bpm_confidence;
  Key key;
  TimeSignature time_signature;
  std::vector<Beat> beats;
  std::vector<Chord> chords;
  std::vector<Section> sections;
  Timbre timbre;
  Dynamics dynamics;
  RhythmFeatures rhythm;
  std::string form;  // 楽曲形式、例: "IABABCO"
};
```

---

## エラーハンドリング

libsonare はエラーハンドリングに例外を使用します。

```cpp
class SonareException : public std::exception {
public:
  explicit SonareException(ErrorCode code, const std::string& message);

  ErrorCode code() const;
  const char* what() const noexcept override;
};
```

### 例

```cpp
try {
  auto audio = sonare::Audio::from_file("nonexistent.mp3");
} catch (const sonare::SonareException& e) {
  std::cerr << "エラー: " << e.what() << "\n";
  if (e.code() == sonare::ErrorCode::FileNotFound) {
    // ファイルが見つからない場合の処理
  }
}
```
