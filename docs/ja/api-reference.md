# API リファレンス

## コア型

### Audio

ゼロコピースライシング対応のオーディオバッファクラス。

```cpp
class Audio {
public:
  // ファクトリメソッド
  static Audio from_buffer(const float* data, size_t length, int sample_rate);
  static Audio from_vector(std::vector<float> data, int sample_rate);
  static Audio from_file(const std::string& path);  // ネイティブのみ
  static Audio from_memory(const uint8_t* data, size_t length);

  // プロパティ
  const float* data() const;
  size_t size() const;
  int sample_rate() const;
  float duration() const;
  int channels() const;  // 常に 1 (モノラル)

  // 操作
  Audio slice(float start_sec, float end_sec) const;  // ゼロコピー
  Audio slice_samples(size_t start, size_t end) const;
  Audio to_mono() const;
};
```

### Key

音楽キーの表現。

```cpp
struct Key {
  PitchClass root;      // C, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
  Mode mode;            // Major, Minor
  float confidence;     // 0.0 - 1.0

  std::string to_string() const;  // 例: "C major", "A minor"
};
```

### Beat

ビート情報。

```cpp
struct Beat {
  float time;       // 秒単位の時間
  int frame;        // フレームインデックス
  float strength;   // ビート強度 (0.0 - 1.0)
};
```

### Chord

コード情報。

```cpp
struct Chord {
  PitchClass root;
  ChordQuality quality;  // Major, Minor, Diminished, Augmented など
  float start;           // 開始時間 (秒)
  float end;             // 終了時間 (秒)
  float confidence;

  std::string to_string() const;  // 例: "C", "Am", "G7"
};
```

### Section

楽曲構造セクション情報。

```cpp
struct Section {
  SectionType type;    // Intro, Verse, Chorus, Bridge など
  float start;         // 開始時間 (秒)
  float end;           // 終了時間 (秒)
  float energy_level;  // 相対エネルギー (0.0 - 1.0)
  float confidence;

  std::string to_string() const;  // 例: "Verse (0.0-15.2s)"
};
```

### AnalysisResult

完全な解析結果。

```cpp
struct AnalysisResult {
  float bpm;
  Key key;
  TimeSignature time_signature;
  std::vector<Beat> beats;
  std::vector<Chord> chords;
  std::vector<Section> sections;
  Timbre timbre;
  Dynamics dynamics;
};
```

## Quick API

一般的な解析タスク用のシンプルな関数。

```cpp
namespace sonare::quick {
  // BPM 検出 (±2 BPM の精度)
  float detect_bpm(const float* samples, size_t length, int sample_rate);

  // 信頼度付きキー検出
  Key detect_key(const float* samples, size_t length, int sample_rate);

  // ビート時刻 (秒単位)
  std::vector<float> detect_beats(const float* samples, size_t length, int sample_rate);

  // オンセット時刻 (秒単位)
  std::vector<float> detect_onsets(const float* samples, size_t length, int sample_rate);

  // フル解析
  AnalysisResult analyze(const float* samples, size_t length, int sample_rate);
}
```

## MusicAnalyzer

遅延初期化による包括的な音楽解析用の Facade クラス。

```cpp
class MusicAnalyzer {
public:
  explicit MusicAnalyzer(const Audio& audio);

  // 個別アナライザー (遅延初期化)
  BpmAnalyzer& bpm_analyzer();
  KeyAnalyzer& key_analyzer();
  OnsetAnalyzer& onset_analyzer();
  BeatAnalyzer& beat_analyzer();
  ChordAnalyzer& chord_analyzer();
  SectionAnalyzer& section_analyzer();
  RhythmAnalyzer& rhythm_analyzer();
  TimbreAnalyzer& timbre_analyzer();
  MelodyAnalyzer& melody_analyzer();
  DynamicsAnalyzer& dynamics_analyzer();

  // クイックアクセスメソッド
  float bpm() const;
  Key key() const;
  std::vector<Beat> beats() const;
  std::vector<Chord> chords() const;
  std::vector<Section> sections() const;
  TimeSignature time_signature() const;
  Timbre timbre() const;
  Dynamics dynamics() const;

  // フル解析
  AnalysisResult analyze() const;
};
```

## 特徴量抽出

### MelSpectrogram

```cpp
struct MelConfig {
  int n_mels = 128;
  float fmin = 0.0f;
  float fmax = 0.0f;  // 0 = sr/2
  StftConfig stft;
};

class MelSpectrogram {
public:
  static MelSpectrogram compute(const Audio& audio, const MelConfig& config = {});

  MatrixView<float> power() const;
  std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;
  std::vector<float> mfcc(int n_mfcc = 13, float lifter = 0.0f) const;

  int n_mels() const;
  int n_frames() const;
};
```

### Chroma

```cpp
class Chroma {
public:
  static Chroma compute(const Audio& audio, const ChromaConfig& config = {});

  MatrixView<float> features() const;  // [12 x n_frames]
  std::array<float, 12> mean_energy() const;
  std::vector<int> dominant_pitch_class() const;

  int n_frames() const;
};
```

### スペクトル特徴量

```cpp
// フレームごとのスペクトル重心 (Hz)
std::vector<float> spectral_centroid(const Spectrogram& spec, int sr);

// フレームごとのスペクトル帯域幅 (Hz)
std::vector<float> spectral_bandwidth(const Spectrogram& spec, int sr);

// フレームごとのスペクトルロールオフ (Hz)
std::vector<float> spectral_rolloff(const Spectrogram& spec, int sr, float percent = 0.85f);

// フレームごとのスペクトル平坦度
std::vector<float> spectral_flatness(const Spectrogram& spec);

// フレームごとのスペクトルコントラスト
std::vector<float> spectral_contrast(const Spectrogram& spec, int sr, int n_bands = 6);
```

## エフェクト

### HPSS (ハーモニック・パーカッシブ音源分離)

```cpp
struct HpssConfig {
  int kernel_size_harmonic = 31;
  int kernel_size_percussive = 31;
  float power = 2.0f;
};

struct HpssAudioResult {
  Audio harmonic;
  Audio percussive;
};

HpssAudioResult hpss(const Audio& audio, const HpssConfig& config = {});
Audio harmonic(const Audio& audio, const HpssConfig& config = {});
Audio percussive(const Audio& audio, const HpssConfig& config = {});
```

### タイムストレッチ

```cpp
// rate で音声を伸縮 (0.5 = 半分の速度、2.0 = 2倍の速度)
Audio time_stretch(const Audio& audio, float rate, const TimeStretchConfig& config = {});
```

### ピッチシフト

```cpp
// 半音単位でピッチをシフト (正 = 高く、負 = 低く)
Audio pitch_shift(const Audio& audio, float semitones, const PitchShiftConfig& config = {});

// 比率でピッチをシフト (2.0 = 1オクターブ上)
Audio pitch_shift_ratio(const Audio& audio, float ratio, const PitchShiftConfig& config = {});
```

### ノーマライズ

```cpp
// 目標 dB へのピークノーマライズ
Audio normalize(const Audio& audio, float target_db = 0.0f);

// RMS ノーマライズ
Audio normalize_rms(const Audio& audio, float target_db = -20.0f);

// 開始と終了から無音をトリム
Audio trim(const Audio& audio, float threshold_db = -60.0f);
```

## C API

```c
// 音声読み込み
SonareError sonare_audio_from_buffer(const float* data, size_t len, int sr, SonareAudio** out);
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
void sonare_audio_free(SonareAudio* audio);

// 解析
SonareError sonare_detect_bpm(const SonareAudio* audio, float* out_bpm, float* out_confidence);
SonareError sonare_detect_key(const SonareAudio* audio, SonareKey* out_key);
SonareError sonare_detect_beats(const SonareAudio* audio, float** out_times, size_t* out_count);
SonareError sonare_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// メモリ管理
void sonare_free_floats(float* ptr);
void sonare_free_result(SonareAnalysisResult* result);

// ユーティリティ
const char* sonare_error_message(SonareError error);
const char* sonare_version(void);
```

## 単位変換

```cpp
// Hz ↔ Mel
float hz_to_mel(float hz);       // Slaney 式
float mel_to_hz(float mel);
float hz_to_mel_htk(float hz);   // HTK 式
float mel_to_hz_htk(float mel);

// Hz ↔ MIDI ノート番号
float hz_to_midi(float hz);      // A4 = 440Hz = 69
float midi_to_hz(float midi);

// Hz ↔ ノート名
std::string hz_to_note(float hz);    // 例: "A4", "C#5"
float note_to_hz(const std::string& note);

// 時間 ↔ フレーム
float frames_to_time(int frames, int sr, int hop_length);
int time_to_frames(float time, int sr, int hop_length);

// サンプル ↔ 時間
float samples_to_time(int samples, int sr);
int time_to_samples(float time, int sr);

// ビン ↔ Hz
float bin_to_hz(int bin, int sr, int n_fft);
int hz_to_bin(float hz, int sr, int n_fft);
```

## 列挙型

```cpp
enum class PitchClass {
  C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
};

enum class Mode {
  Major, Minor
};

enum class ChordQuality {
  Major, Minor, Diminished, Augmented,
  Dominant7, Major7, Minor7, Sus2, Sus4
};

enum class SectionType {
  Intro, Verse, PreChorus, Chorus, Bridge, Instrumental, Outro
};

enum class WindowType {
  Hann, Hamming, Blackman, Rectangular
};
```
