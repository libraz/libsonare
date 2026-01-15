/// @file convert.cpp
/// @brief Implementation of unit conversion functions.

#include "core/convert.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace sonare {

namespace {
/// @brief Slaney mel scale constants (librosa default).
/// @details Uses exact runtime calculation for precision matching with librosa.
constexpr float kMelFMin = 0.0f;
constexpr float kMelFSp = 200.0f / 3.0f;  // 66.67 Hz
constexpr float kMinLogHz = 1000.0f;
constexpr float kMinLogMel = (kMinLogHz - kMelFMin) / kMelFSp;  // 15.0
// Runtime calculation matches librosa's np.log(6.4) / 27.0 = 0.06875177742094912
// Using inline function to avoid static initialization order issues
inline float log_step() {
  static const float value = std::log(6.4f) / 27.0f;
  return value;
}
}  // namespace

float hz_to_mel(float hz) {
  if (hz < kMinLogHz) {
    return (hz - kMelFMin) / kMelFSp;
  }
  return kMinLogMel + std::log(hz / kMinLogHz) / log_step();
}

float mel_to_hz(float mel) {
  if (mel < kMinLogMel) {
    return kMelFMin + kMelFSp * mel;
  }
  return kMinLogHz * std::exp(log_step() * (mel - kMinLogMel));
}

float hz_to_mel_htk(float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); }

float mel_to_hz_htk(float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); }

float hz_to_midi(float hz) {
  if (hz <= 0) return 0.0f;
  return 12.0f * std::log2(hz / 440.0f) + 69.0f;
}

float midi_to_hz(float midi) { return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f); }

std::string hz_to_note(float hz) {
  if (hz <= 0) return "?";

  float midi = hz_to_midi(hz);
  int midi_int = static_cast<int>(std::round(midi));

  static const char* note_names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};

  int octave = midi_int / 12 - 1;
  int note = midi_int % 12;

  return std::string(note_names[note]) + std::to_string(octave);
}

float note_to_hz(const std::string& note) {
  if (note.empty()) return 0.0f;

  /// Note offsets from C
  static const int note_offsets[] = {
      0,   // C
      2,   // D
      4,   // E
      5,   // F
      7,   // G
      9,   // A
      11,  // B
  };

  char base = std::toupper(note[0]);
  if (base < 'A' || base > 'G') return 0.0f;

  int offset = note_offsets[(base - 'C' + 7) % 7];

  size_t idx = 1;
  if (idx < note.size()) {
    if (note[idx] == '#') {
      offset += 1;
      idx++;
    } else if (note[idx] == 'b') {
      offset -= 1;
      idx++;
    }
  }

  int octave = 4;  // default
  if (idx < note.size()) {
    try {
      size_t pos = 0;
      octave = std::stoi(note.substr(idx), &pos);
      // Validate entire remaining string was consumed
      if (pos != note.size() - idx) {
        return 0.0f;  // Invalid note format
      }
    } catch (const std::exception&) {
      return 0.0f;  // Invalid note format
    }
  }

  int midi = (octave + 1) * 12 + offset;
  return midi_to_hz(static_cast<float>(midi));
}

float frames_to_time(int frames, int sr, int hop_length) {
  return static_cast<float>(frames * hop_length) / sr;
}

int time_to_frames(float time, int sr, int hop_length) {
  // Use floor for librosa compatibility (np.floor in librosa.core.time_to_frames)
  return static_cast<int>(std::floor(time * static_cast<float>(sr) / static_cast<float>(hop_length)));
}

float samples_to_time(int samples, int sr) { return static_cast<float>(samples) / sr; }

int time_to_samples(float time, int sr) { return static_cast<int>(time * sr); }

float bin_to_hz(int bin, int sr, int n_fft) { return static_cast<float>(bin * sr) / n_fft; }

int hz_to_bin(float hz, int sr, int n_fft) { return static_cast<int>(std::round(hz * n_fft / sr)); }

}  // namespace sonare
