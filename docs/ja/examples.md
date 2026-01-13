# 使用例

## 基本的な使い方

### 音声読み込みと BPM 検出

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  // 音声ファイルの読み込み
  auto audio = sonare::Audio::from_file("music.mp3");

  // BPM 検出
  float bpm = sonare::quick::detect_bpm(
    audio.data(), audio.size(), audio.sample_rate()
  );

  std::cout << "BPM: " << bpm << std::endl;
  return 0;
}
```

### キー検出

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  auto key = sonare::quick::detect_key(
    audio.data(), audio.size(), audio.sample_rate()
  );

  std::cout << "Key: " << key.to_string() << std::endl;
  std::cout << "Confidence: " << (key.confidence * 100) << "%" << std::endl;

  return 0;
}
```

### ビート検出

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  auto beats = sonare::quick::detect_beats(
    audio.data(), audio.size(), audio.sample_rate()
  );

  std::cout << beats.size() << " 個のビートを検出:" << std::endl;
  for (size_t i = 0; i < std::min(beats.size(), size_t(10)); ++i) {
    std::cout << "  ビート " << i + 1 << ": " << beats[i] << "秒" << std::endl;
  }

  return 0;
}
```

## MusicAnalyzer によるフル解析

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");
  sonare::MusicAnalyzer analyzer(audio);

  // フル解析結果を取得
  auto result = analyzer.analyze();

  std::cout << "=== 音楽解析 ===" << std::endl;
  std::cout << "BPM: " << result.bpm << std::endl;
  std::cout << "キー: " << result.key.to_string() << std::endl;
  std::cout << "拍子: " << result.time_signature.numerator
            << "/" << result.time_signature.denominator << std::endl;

  std::cout << "\nビート数: " << result.beats.size() << std::endl;

  std::cout << "\nコード:" << std::endl;
  for (const auto& chord : result.chords) {
    std::cout << "  " << chord.to_string()
              << " [" << chord.start << "秒 - " << chord.end << "秒]"
              << std::endl;
  }

  std::cout << "\nセクション:" << std::endl;
  for (const auto& section : result.sections) {
    std::cout << "  " << section.to_string() << std::endl;
  }

  return 0;
}
```

## 特徴量抽出

### メルスペクトログラム

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  sonare::MelConfig config;
  config.n_mels = 128;
  config.stft.n_fft = 2048;
  config.stft.hop_length = 512;

  auto mel = sonare::MelSpectrogram::compute(audio, config);

  std::cout << "メルスペクトログラムの形状: "
            << mel.n_mels() << " x " << mel.n_frames() << std::endl;

  // MFCC を取得
  auto mfcc = mel.mfcc(13);
  std::cout << "MFCC 係数: " << mfcc.size() / mel.n_frames() << std::endl;

  return 0;
}
```

### クロマ特徴量

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  auto chroma = sonare::Chroma::compute(audio);
  auto energy = chroma.mean_energy();

  const char* notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

  std::cout << "ピッチクラス分布:" << std::endl;
  for (int i = 0; i < 12; ++i) {
    std::cout << "  " << notes[i] << ": " << energy[i] << std::endl;
  }

  return 0;
}
```

## オーディオエフェクト

### HPSS (ハーモニック・パーカッシブ分離)

```cpp
#include <sonare/sonare.h>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  sonare::HpssConfig config;
  config.kernel_size_harmonic = 31;
  config.kernel_size_percussive = 31;

  auto result = sonare::hpss(audio, config);

  // result.harmonic にはメロディック/ハーモニック成分が含まれる
  // result.percussive にはドラム/パーカッション成分が含まれる

  // 分離した音声を保存 (音声書き込み機能がある場合)
  // save_audio("harmonic.wav", result.harmonic);
  // save_audio("percussive.wav", result.percussive);

  return 0;
}
```

### タイムストレッチ

```cpp
#include <sonare/sonare.h>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  // 50% の速度に減速 (再生時間が2倍になる)
  auto slow = sonare::time_stretch(audio, 0.5f);

  // 150% の速度に加速
  auto fast = sonare::time_stretch(audio, 1.5f);

  return 0;
}
```

### ピッチシフト

```cpp
#include <sonare/sonare.h>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  // 2半音上にシフト
  auto higher = sonare::pitch_shift(audio, 2.0f);

  // 3半音下にシフト
  auto lower = sonare::pitch_shift(audio, -3.0f);

  // 1オクターブ上にシフト
  auto octave_up = sonare::pitch_shift(audio, 12.0f);

  return 0;
}
```

## C API の使用

```c
#include <sonare_c.h>
#include <stdio.h>

int main() {
  SonareAudio* audio = NULL;
  SonareError err;

  // 音声の読み込み
  err = sonare_audio_from_file("music.mp3", &audio);
  if (err != SONARE_OK) {
    printf("エラー: %s\n", sonare_error_message(err));
    return 1;
  }

  // BPM 検出
  float bpm, confidence;
  err = sonare_detect_bpm(audio, &bpm, &confidence);
  if (err == SONARE_OK) {
    printf("BPM: %.1f (信頼度: %.0f%%)\n", bpm, confidence * 100);
  }

  // キー検出
  SonareKey key;
  err = sonare_detect_key(audio, &key);
  if (err == SONARE_OK) {
    printf("キー: %s %s\n",
           key.root == 0 ? "C" : "...",  // 簡略化
           key.mode == 0 ? "メジャー" : "マイナー");
  }

  // ビート検出
  float* beat_times = NULL;
  size_t beat_count = 0;
  err = sonare_detect_beats(audio, &beat_times, &beat_count);
  if (err == SONARE_OK) {
    printf("ビート数: %zu\n", beat_count);
    sonare_free_floats(beat_times);
  }

  // クリーンアップ
  sonare_audio_free(audio);

  return 0;
}
```

## 音声スライスと解析

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  // 最初の30秒だけを解析
  auto intro = audio.slice(0.0f, 30.0f);

  auto key = sonare::quick::detect_key(
    intro.data(), intro.size(), intro.sample_rate()
  );

  std::cout << "イントロセクションのキー: " << key.to_string() << std::endl;

  // 特定のセクションを解析 (例: 60-90秒のサビ)
  auto chorus = audio.slice(60.0f, 90.0f);

  float bpm = sonare::quick::detect_bpm(
    chorus.data(), chorus.size(), chorus.sample_rate()
  );

  std::cout << "サビの BPM: " << bpm << std::endl;

  return 0;
}
```

## 生のオーディオデータの操作

```cpp
#include <sonare/sonare.h>
#include <vector>
#include <cmath>

int main() {
  // テスト信号を生成 (440Hz サイン波)
  const int sample_rate = 44100;
  const float duration = 1.0f;
  const int num_samples = static_cast<int>(sample_rate * duration);

  std::vector<float> samples(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    float t = static_cast<float>(i) / sample_rate;
    samples[i] = std::sin(2.0f * M_PI * 440.0f * t);
  }

  // バッファから Audio を作成
  auto audio = sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);

  // 解析
  auto chroma = sonare::Chroma::compute(audio);
  auto energy = chroma.mean_energy();

  // A (440Hz) はピッチクラス 9 で最も高いエネルギーを持つはず
  std::cout << "ピッチクラスエネルギー:" << std::endl;
  for (int i = 0; i < 12; ++i) {
    std::cout << i << ": " << energy[i] << std::endl;
  }

  return 0;
}
```
