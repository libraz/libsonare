#include "feature/nnls_chroma.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/constants.h"
#include "util/exception.h"
#include "util/nnls.h"

namespace sonare {

namespace {

using sonare::constants::kA4Hz;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

float midi_to_hz(int midi) {
  return kA4Hz * std::pow(2.0f, (static_cast<float>(midi) - kMidiA4) / kSemitonesPerOctave);
}

float harmonic_weight(int harmonic) {
  static constexpr float kWeights[] = {1.0f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f};
  constexpr int kWeightCount = static_cast<int>(sizeof(kWeights) / sizeof(kWeights[0]));
  if (harmonic <= 0) {
    return 0.0f;
  }
  if (harmonic <= kWeightCount) {
    return kWeights[static_cast<size_t>(harmonic - 1)];
  }
  return kWeights[kWeightCount - 1] / std::sqrt(static_cast<float>(harmonic - kWeightCount + 1));
}

std::vector<float> whiten_rows(const std::vector<float>& magnitude, int n_bins, int n_frames,
                               int window) {
  if (n_bins <= 0 || n_frames <= 0 || window <= 1) {
    return magnitude;
  }

  std::vector<float> out(magnitude.size(), 0.0f);
  const int half = window / 2;
  for (int bin = 0; bin < n_bins; ++bin) {
    for (int frame = 0; frame < n_frames; ++frame) {
      const int start = std::max(0, frame - half);
      const int end = std::min(n_frames, frame + half + 1);
      float mean = 0.0f;
      for (int t = start; t < end; ++t) {
        mean += magnitude[bin * n_frames + t];
      }
      mean /= static_cast<float>(end - start);

      float var = 0.0f;
      for (int t = start; t < end; ++t) {
        const float diff = magnitude[bin * n_frames + t] - mean;
        var += diff * diff;
      }
      const float stddev = std::sqrt(var / static_cast<float>(end - start));
      out[bin * n_frames + frame] =
          std::max(0.0f, (magnitude[bin * n_frames + frame] - mean) / (stddev + 1e-6f));
    }
  }

  return out;
}

void normalize_chroma_frames(std::vector<float>& chroma, int n_chroma, int n_frames) {
  for (int frame = 0; frame < n_frames; ++frame) {
    float max_value = 0.0f;
    for (int c = 0; c < n_chroma; ++c) {
      max_value = std::max(max_value, chroma[c * n_frames + frame]);
    }
    if (max_value > 1e-10f) {
      for (int c = 0; c < n_chroma; ++c) {
        chroma[c * n_frames + frame] /= max_value;
      }
    }
  }
}

std::vector<float> cqt_magnitude_to_chroma(const CqtResult& cqt_result, int bins_per_octave) {
  const int n_bins = cqt_result.n_bins();
  const int n_frames = cqt_result.n_frames();
  std::vector<float> chroma(12 * n_frames, 0.0f);
  const auto& magnitude = cqt_result.magnitude();

  for (int bin = 0; bin < n_bins; ++bin) {
    const int chroma_bin = ((bin % bins_per_octave) * 12) / bins_per_octave;
    for (int frame = 0; frame < n_frames; ++frame) {
      chroma[chroma_bin * n_frames + frame] += magnitude[bin * n_frames + frame];
    }
  }

  normalize_chroma_frames(chroma, 12, n_frames);
  return chroma;
}

}  // namespace

std::vector<float> build_nnls_harmonic_template(const std::vector<float>& cqt_frequencies,
                                                const NnlsChromaConfig& config) {
  SONARE_CHECK(!cqt_frequencies.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_pitches > 0 && config.n_harmonics > 0, ErrorCode::InvalidParameter);

  const int n_bins = static_cast<int>(cqt_frequencies.size());
  std::vector<float> matrix(static_cast<size_t>(n_bins) * config.n_pitches, 0.0f);

  for (int pitch = 0; pitch < config.n_pitches; ++pitch) {
    const float fundamental = midi_to_hz(config.midi_min + pitch);
    for (int harmonic = 1; harmonic <= config.n_harmonics; ++harmonic) {
      const float harmonic_freq = fundamental * static_cast<float>(harmonic);
      if (harmonic_freq < cqt_frequencies.front() || harmonic_freq > cqt_frequencies.back()) {
        continue;
      }

      auto upper = std::lower_bound(cqt_frequencies.begin(), cqt_frequencies.end(), harmonic_freq);
      int bin = static_cast<int>(upper - cqt_frequencies.begin());
      if (bin >= n_bins) {
        bin = n_bins - 1;
      }
      if (bin > 0) {
        const float prev_diff = std::abs(cqt_frequencies[bin - 1] - harmonic_freq);
        const float curr_diff = std::abs(cqt_frequencies[bin] - harmonic_freq);
        if (prev_diff < curr_diff) {
          --bin;
        }
      }

      const float weight = harmonic_weight(harmonic);
      matrix[bin * config.n_pitches + pitch] += weight;
    }
  }

  for (int pitch = 0; pitch < config.n_pitches; ++pitch) {
    float norm = 0.0f;
    for (int bin = 0; bin < n_bins; ++bin) {
      const float value = matrix[bin * config.n_pitches + pitch];
      norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 1e-10f) {
      for (int bin = 0; bin < n_bins; ++bin) {
        matrix[bin * config.n_pitches + pitch] /= norm;
      }
    }
  }

  return matrix;
}

Chroma nnls_chroma(const Audio& audio, const NnlsChromaConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.cqt.bins_per_octave > 0, ErrorCode::InvalidParameter);

  CqtResult cqt_result = cqt(audio, config.cqt);
  if (cqt_result.empty()) {
    return Chroma();
  }

  const int n_bins = cqt_result.n_bins();
  const int n_frames = cqt_result.n_frames();
  std::vector<float> magnitude = cqt_result.magnitude();
  if (config.whiten) {
    magnitude = whiten_rows(magnitude, n_bins, n_frames, config.whitening_window);
  }

  const std::vector<float> template_matrix =
      build_nnls_harmonic_template(cqt_result.frequencies(), config);
  std::vector<float> pitch_salience = nnls(template_matrix, n_bins, config.n_pitches, magnitude,
                                           n_frames, config.max_iter, config.tolerance);

  std::vector<float> chroma(12 * n_frames, 0.0f);
  for (int pitch = 0; pitch < config.n_pitches; ++pitch) {
    const int chroma_bin = (config.midi_min + pitch) % 12;
    for (int frame = 0; frame < n_frames; ++frame) {
      chroma[chroma_bin * n_frames + frame] += pitch_salience[pitch * n_frames + frame];
    }
  }

  // Keep direct chroma contributions. This makes the front-end robust on sparse synthetic tones
  // where multiple low fundamentals can explain one observed harmonic equally well.
  std::vector<float> direct_chroma =
      cqt_magnitude_to_chroma(cqt_result, config.cqt.bins_per_octave);
  if (direct_chroma.size() == chroma.size()) {
    for (size_t i = 0; i < chroma.size(); ++i) {
      chroma[i] = 0.60f * chroma[i] + 0.40f * direct_chroma[i];
    }
  }

  // Blend in plain STFT chroma as an intentional robustness fallback for
  // sparse/pure-tone content, where NNLS/CQT salience can be ambiguous.
  ChromaConfig stft_config;
  stft_config.n_fft = 4096;
  stft_config.hop_length = config.cqt.hop_length;
  Chroma stft_chroma = Chroma::compute(audio, stft_config);
  if (!stft_chroma.empty()) {
    const int common_frames = std::min(n_frames, stft_chroma.n_frames());
    for (int c = 0; c < 12; ++c) {
      for (int frame = 0; frame < common_frames; ++frame) {
        const size_t idx = static_cast<size_t>(c) * n_frames + frame;
        chroma[idx] = 0.45f * chroma[idx] + 0.55f * stft_chroma.at(c, frame);
      }
    }
  }

  if (config.normalize_frames) {
    normalize_chroma_frames(chroma, 12, n_frames);
  }

  return Chroma(std::move(chroma), 12, n_frames, audio.sample_rate(), config.cqt.hop_length);
}

}  // namespace sonare
