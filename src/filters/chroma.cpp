#include "filters/chroma.h"

#include <cmath>

#include "core/convert.h"
#include "util/exception.h"

namespace sonare {

namespace {
constexpr float kC1Hz = 32.70319566257483f;  // C1 frequency in Hz
}

int hz_to_pitch_class(float hz, float tuning) {
  if (hz <= 0.0f) {
    return -1;
  }
  float chroma = hz_to_chroma(hz, tuning);
  return static_cast<int>(chroma) % 12;
}

float hz_to_chroma(float hz, float tuning) {
  if (hz <= 0.0f) {
    return -1.0f;
  }

  // Convert Hz to MIDI note number (with tuning adjustment)
  float midi = hz_to_midi(hz) - tuning;

  // Extract fractional pitch class [0, 12)
  float chroma = std::fmod(midi, 12.0f);
  if (chroma < 0.0f) {
    chroma += 12.0f;
  }
  return chroma;
}

std::vector<float> create_chroma_filterbank(int sr, int n_fft, const ChromaFilterConfig& config) {
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_chroma > 0, ErrorCode::InvalidParameter);

  int n_bins = n_fft / 2 + 1;
  int n_chroma = config.n_chroma;

  // Minimum frequency (default to C1)
  float fmin = config.fmin > 0.0f ? config.fmin : kC1Hz;

  // Create filterbank [n_chroma x n_bins]
  std::vector<float> filterbank(n_chroma * n_bins, 0.0f);

  // For each FFT bin, compute contribution to each chroma bin
  float bin_width = static_cast<float>(sr) / n_fft;

  for (int k = 1; k < n_bins; ++k) {  // Skip DC bin
    float freq = k * bin_width;

    if (freq < fmin) {
      continue;
    }

    // Get fractional chroma for this frequency
    float chroma = hz_to_chroma(freq, config.tuning);
    if (chroma < 0.0f) {
      continue;
    }

    // Scale to n_chroma bins
    float scaled_chroma = chroma * n_chroma / 12.0f;

    // Distribute energy to neighboring chroma bins (using triangular window)
    int chroma_low = static_cast<int>(std::floor(scaled_chroma)) % n_chroma;
    int chroma_high = (chroma_low + 1) % n_chroma;
    float frac = scaled_chroma - std::floor(scaled_chroma);

    // Weight by proximity
    filterbank[chroma_low * n_bins + k] += (1.0f - frac);
    filterbank[chroma_high * n_bins + k] += frac;
  }

  // Normalize each chroma bin
  for (int c = 0; c < n_chroma; ++c) {
    float sum = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      sum += filterbank[c * n_bins + k];
    }
    if (sum > 0.0f) {
      for (int k = 0; k < n_bins; ++k) {
        filterbank[c * n_bins + k] /= sum;
      }
    }
  }

  return filterbank;
}

std::vector<float> apply_chroma_filterbank(const float* power, int n_bins, int n_frames,
                                           const float* filterbank, int n_chroma) {
  SONARE_CHECK(power != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(filterbank != nullptr, ErrorCode::InvalidParameter);

  // Output: [n_chroma x n_frames]
  std::vector<float> chromagram(n_chroma * n_frames, 0.0f);

  // Matrix multiply: chromagram = filterbank @ power
  for (int c = 0; c < n_chroma; ++c) {
    for (int t = 0; t < n_frames; ++t) {
      float sum = 0.0f;
      for (int k = 0; k < n_bins; ++k) {
        sum += filterbank[c * n_bins + k] * power[k * n_frames + t];
      }
      chromagram[c * n_frames + t] = sum;
    }
  }

  return chromagram;
}

}  // namespace sonare
