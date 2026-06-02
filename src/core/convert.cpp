/// @file convert.cpp
/// @brief Implementation of unit conversion functions.

#include "core/convert.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

namespace {
/// @brief Slaney mel scale constants.
/// @details Uses exact runtime calculation for stable numeric behavior.
constexpr float kMelFMin = 0.0f;
constexpr float kMelFSp = 200.0f / 3.0f;  // 66.67 Hz
constexpr float kMinLogHz = 1000.0f;
constexpr float kMinLogMel = (kMinLogHz - kMelFMin) / kMelFSp;  // 15.0
// Runtime calculation of np.log(6.4) / 27.0
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
  // 0 is a valid MIDI value (C-1), so it must not double as the
  // invalid-input sentinel. Return -inf for non-positive Hz, matching the
  // log2 limit (librosa propagates -inf / NaN here as well). All in-tree
  // callers pass strictly positive frequencies (fmin / f0), so this only
  // affects genuinely invalid input.
  if (hz <= 0) return -std::numeric_limits<float>::infinity();
  return constants::kSemitonesPerOctave * std::log2(hz / constants::kA4Hz) + constants::kMidiA4;
}

float midi_to_hz(float midi) {
  return constants::kA4Hz *
         std::pow(2.0f, (midi - constants::kMidiA4) / constants::kSemitonesPerOctave);
}

std::string hz_to_note(float hz) {
  if (hz <= 0) return "?";

  float midi = hz_to_midi(hz);
  int midi_int = static_cast<int>(std::round(midi));

  static const char* note_names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};

  int note = ((midi_int % 12) + 12) % 12;
  int octave = (midi_int - note) / 12 - 1;

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

  char base = static_cast<char>(std::toupper(static_cast<unsigned char>(note[0])));
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
  if (sr <= 0 || hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "frames_to_time: sr and hop_length must be positive");
  }
  return static_cast<float>(frames) * static_cast<float>(hop_length) / static_cast<float>(sr);
}

int time_to_frames(float time, int sr, int hop_length) {
  if (sr <= 0 || hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "time_to_frames: sr and hop_length must be positive");
  }
  // Use floor for deterministic frame conversion.
  return static_cast<int>(
      std::floor(time * static_cast<float>(sr) / static_cast<float>(hop_length)));
}

float samples_to_time(int samples, int sr) {
  if (sr <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "samples_to_time: sr must be positive");
  }
  return static_cast<float>(samples) / sr;
}

int time_to_samples(float time, int sr) {
  if (sr <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "time_to_samples: sr must be positive");
  }
  return static_cast<int>(time * sr);
}

namespace {
// Compute frames*hop + offset in 64-bit and saturate into the int return type,
// so large frame indices (long files) cannot overflow int (UB). Mirrors the
// double-precision promotion already used by bin_to_hz.
int frame_index_to_sample(int frame, int hop_length, int offset) noexcept {
  const int64_t value =
      static_cast<int64_t>(frame) * static_cast<int64_t>(hop_length) + static_cast<int64_t>(offset);
  return static_cast<int>(
      std::clamp<int64_t>(value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}
}  // namespace

int frames_to_samples(int frames, int hop_length, int n_fft) {
  const int offset = (n_fft > 0) ? (n_fft / 2) : 0;
  return frame_index_to_sample(frames, hop_length, offset);
}

std::vector<int> frames_to_samples(const std::vector<int>& frames, int hop_length, int n_fft) {
  const int offset = (n_fft > 0) ? (n_fft / 2) : 0;
  std::vector<int> out;
  out.reserve(frames.size());
  for (int f : frames) out.push_back(frame_index_to_sample(f, hop_length, offset));
  return out;
}

int samples_to_frames(int samples, int hop_length, int n_fft) {
  const int offset = (n_fft > 0) ? (n_fft / 2) : 0;
  // Use floor for deterministic behavior with negative numerators.
  const int adjusted = samples - offset;
  if (adjusted >= 0) {
    return adjusted / hop_length;
  }
  // Mirror Python floor-division on negatives.
  return -((-adjusted + hop_length - 1) / hop_length);
}

std::vector<int> samples_to_frames(const std::vector<int>& samples, int hop_length, int n_fft) {
  std::vector<int> out;
  out.reserve(samples.size());
  for (int s : samples) out.push_back(samples_to_frames(s, hop_length, n_fft));
  return out;
}

float bin_to_hz(int bin, int sr, int n_fft) {
  // Promote to double before multiplying so bin * sr cannot overflow int at
  // high sample rates / large FFT bins (e.g. bin=2048, sr=192000 overflows a
  // 32-bit int). Accumulate in double, return float.
  return static_cast<float>(static_cast<double>(bin) * static_cast<double>(sr) /
                            static_cast<double>(n_fft));
}

int hz_to_bin(float hz, int sr, int n_fft) {
  if (sr <= 0 || n_fft <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "hz_to_bin: sr and n_fft must be positive");
  }
  return static_cast<int>(std::round(hz * n_fft / sr));
}

}  // namespace sonare
