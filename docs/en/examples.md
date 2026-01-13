# Usage Examples

## Basic Usage

### Load Audio and Detect BPM

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  // Load audio file
  auto audio = sonare::Audio::from_file("music.mp3");

  // Detect BPM
  float bpm = sonare::quick::detect_bpm(
    audio.data(), audio.size(), audio.sample_rate()
  );

  std::cout << "BPM: " << bpm << std::endl;
  return 0;
}
```

### Key Detection

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

### Beat Detection

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  auto beats = sonare::quick::detect_beats(
    audio.data(), audio.size(), audio.sample_rate()
  );

  std::cout << "Found " << beats.size() << " beats:" << std::endl;
  for (size_t i = 0; i < std::min(beats.size(), size_t(10)); ++i) {
    std::cout << "  Beat " << i + 1 << ": " << beats[i] << "s" << std::endl;
  }

  return 0;
}
```

## Full Analysis with MusicAnalyzer

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");
  sonare::MusicAnalyzer analyzer(audio);

  // Get full analysis result
  auto result = analyzer.analyze();

  std::cout << "=== Music Analysis ===" << std::endl;
  std::cout << "BPM: " << result.bpm << std::endl;
  std::cout << "Key: " << result.key.to_string() << std::endl;
  std::cout << "Time Signature: " << result.time_signature.numerator
            << "/" << result.time_signature.denominator << std::endl;

  std::cout << "\nBeats: " << result.beats.size() << std::endl;

  std::cout << "\nChords:" << std::endl;
  for (const auto& chord : result.chords) {
    std::cout << "  " << chord.to_string()
              << " [" << chord.start << "s - " << chord.end << "s]"
              << std::endl;
  }

  std::cout << "\nSections:" << std::endl;
  for (const auto& section : result.sections) {
    std::cout << "  " << section.to_string() << std::endl;
  }

  return 0;
}
```

## Feature Extraction

### Mel Spectrogram

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

  std::cout << "Mel spectrogram shape: "
            << mel.n_mels() << " x " << mel.n_frames() << std::endl;

  // Get MFCC
  auto mfcc = mel.mfcc(13);
  std::cout << "MFCC coefficients: " << mfcc.size() / mel.n_frames() << std::endl;

  return 0;
}
```

### Chroma Features

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  auto chroma = sonare::Chroma::compute(audio);
  auto energy = chroma.mean_energy();

  const char* notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

  std::cout << "Pitch class distribution:" << std::endl;
  for (int i = 0; i < 12; ++i) {
    std::cout << "  " << notes[i] << ": " << energy[i] << std::endl;
  }

  return 0;
}
```

## Audio Effects

### HPSS (Harmonic-Percussive Separation)

```cpp
#include <sonare/sonare.h>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  sonare::HpssConfig config;
  config.kernel_size_harmonic = 31;
  config.kernel_size_percussive = 31;

  auto result = sonare::hpss(audio, config);

  // result.harmonic contains melodic/harmonic content
  // result.percussive contains drums/percussion

  // Save separated audio (if you have audio writing functionality)
  // save_audio("harmonic.wav", result.harmonic);
  // save_audio("percussive.wav", result.percussive);

  return 0;
}
```

### Time Stretch

```cpp
#include <sonare/sonare.h>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  // Slow down to 50% speed (duration doubles)
  auto slow = sonare::time_stretch(audio, 0.5f);

  // Speed up to 150% speed
  auto fast = sonare::time_stretch(audio, 1.5f);

  return 0;
}
```

### Pitch Shift

```cpp
#include <sonare/sonare.h>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  // Shift up by 2 semitones
  auto higher = sonare::pitch_shift(audio, 2.0f);

  // Shift down by 3 semitones
  auto lower = sonare::pitch_shift(audio, -3.0f);

  // Shift up by one octave
  auto octave_up = sonare::pitch_shift(audio, 12.0f);

  return 0;
}
```

## C API Usage

```c
#include <sonare_c.h>
#include <stdio.h>

int main() {
  SonareAudio* audio = NULL;
  SonareError err;

  // Load audio
  err = sonare_audio_from_file("music.mp3", &audio);
  if (err != SONARE_OK) {
    printf("Error: %s\n", sonare_error_message(err));
    return 1;
  }

  // Detect BPM
  float bpm, confidence;
  err = sonare_detect_bpm(audio, &bpm, &confidence);
  if (err == SONARE_OK) {
    printf("BPM: %.1f (confidence: %.0f%%)\n", bpm, confidence * 100);
  }

  // Detect key
  SonareKey key;
  err = sonare_detect_key(audio, &key);
  if (err == SONARE_OK) {
    printf("Key: %s %s\n",
           key.root == 0 ? "C" : "...",  // Simplified
           key.mode == 0 ? "major" : "minor");
  }

  // Detect beats
  float* beat_times = NULL;
  size_t beat_count = 0;
  err = sonare_detect_beats(audio, &beat_times, &beat_count);
  if (err == SONARE_OK) {
    printf("Beats: %zu\n", beat_count);
    sonare_free_floats(beat_times);
  }

  // Cleanup
  sonare_audio_free(audio);

  return 0;
}
```

## Audio Slice and Analysis

```cpp
#include <sonare/sonare.h>
#include <iostream>

int main() {
  auto audio = sonare::Audio::from_file("music.mp3");

  // Analyze only the first 30 seconds
  auto intro = audio.slice(0.0f, 30.0f);

  auto key = sonare::quick::detect_key(
    intro.data(), intro.size(), intro.sample_rate()
  );

  std::cout << "Key of intro section: " << key.to_string() << std::endl;

  // Analyze a specific section (e.g., chorus at 60-90 seconds)
  auto chorus = audio.slice(60.0f, 90.0f);

  float bpm = sonare::quick::detect_bpm(
    chorus.data(), chorus.size(), chorus.sample_rate()
  );

  std::cout << "BPM of chorus: " << bpm << std::endl;

  return 0;
}
```

## Working with Raw Audio Data

```cpp
#include <sonare/sonare.h>
#include <vector>
#include <cmath>

int main() {
  // Generate a test signal (440Hz sine wave)
  const int sample_rate = 44100;
  const float duration = 1.0f;
  const int num_samples = static_cast<int>(sample_rate * duration);

  std::vector<float> samples(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    float t = static_cast<float>(i) / sample_rate;
    samples[i] = std::sin(2.0f * M_PI * 440.0f * t);
  }

  // Create Audio from buffer
  auto audio = sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);

  // Analyze
  auto chroma = sonare::Chroma::compute(audio);
  auto energy = chroma.mean_energy();

  // A (440Hz) should have the highest energy at pitch class 9
  std::cout << "Pitch class energies:" << std::endl;
  for (int i = 0; i < 12; ++i) {
    std::cout << i << ": " << energy[i] << std::endl;
  }

  return 0;
}
```
