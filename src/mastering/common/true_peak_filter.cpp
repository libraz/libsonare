#include "mastering/common/true_peak_filter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::common {
namespace {

// ITU-R BS.1770 Annex 2 coefficient example for a 48-tap, 4-phase FIR.
// Commercial redistribution of the literal table should be reviewed before
// release; the surrounding implementation can also use a self-designed FIR.
inline constexpr float kBs1770Polyphase4x[12][4] = {
    {0.0017089843750f, -0.0291748046875f, -0.0189208984375f, -0.0083007812500f},
    {0.0109863281250f, 0.0292968750000f, 0.0330810546875f, 0.0148925781250f},
    {-0.0196533203125f, -0.0517578125000f, -0.0582275390625f, -0.0266113281250f},
    {0.0332031250000f, 0.0891113281250f, 0.1015625000000f, 0.0476074218750f},
    {-0.0594482421875f, -0.1665039062500f, -0.2003173828125f, -0.1022949218750f},
    {0.1373291015625f, 0.4650878906250f, 0.7797851562500f, 0.9721679687500f},
    {0.9721679687500f, 0.7797851562500f, 0.4650878906250f, 0.1373291015625f},
    {-0.1022949218750f, -0.2003173828125f, -0.1665039062500f, -0.0594482421875f},
    {0.0476074218750f, 0.1015625000000f, 0.0891113281250f, 0.0332031250000f},
    {-0.0266113281250f, -0.0582275390625f, -0.0517578125000f, -0.0196533203125f},
    {0.0148925781250f, 0.0330810546875f, 0.0292968750000f, 0.0109863281250f},
    {-0.0083007812500f, -0.0189208984375f, -0.0291748046875f, 0.0017089843750f},
};

PolyphaseFir make_bs1770_fir() {
  PolyphaseFir fir;
  fir.phases = 4;
  fir.taps_per_phase = 12;
  fir.phase_taps.assign(4, std::vector<float>(12, 0.0f));
  for (int tap = 0; tap < 12; ++tap) {
    for (int phase = 0; phase < 4; ++phase) {
      fir.phase_taps[static_cast<size_t>(phase)][static_cast<size_t>(tap)] =
          kBs1770Polyphase4x[tap][phase];
    }
  }
  return fir;
}

PolyphaseFir make_true_peak_fir(int factor) {
  if (factor == 4) return make_bs1770_fir();
  if (factor == 2) return design_polyphase_lowpass(2, 24, 7.85726, true);
  throw std::invalid_argument("TruePeakFilter factor must be 2 or 4");
}

void validate_buffers(const float* const* input, int num_channels, int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (input == nullptr) {
    throw std::invalid_argument("input must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (input[ch] == nullptr) {
      throw std::invalid_argument("input channel must not be null");
    }
  }
}

}  // namespace

TruePeakFilter::TruePeakFilter(int num_channels, int factor)
    : factor_(factor), fir_(make_true_peak_fir(factor)) {
  if (num_channels < 0) {
    throw std::invalid_argument("num_channels must be non-negative");
  }
}

float TruePeakFilter::process(const float* const* input, int num_channels, int num_samples) const {
  validate_buffers(input, num_channels, num_samples);
  float peak = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      for (int phase = 0; phase < factor_; ++phase) {
        const float sample = interpolate_polyphase_sample(
            input[ch], static_cast<size_t>(num_samples), static_cast<size_t>(i), phase, fir_);
        peak = std::max(peak, std::abs(sample));
      }
    }
  }
  return peak;
}

void TruePeakFilter::upsample(const float* const* input, float* const* output_oversampled,
                              int num_channels, int num_samples) const {
  validate_buffers(input, num_channels, num_samples);
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (output_oversampled == nullptr) {
    throw std::invalid_argument("output must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw std::invalid_argument("output channel must not be null");
    }
    for (int i = 0; i < num_samples; ++i) {
      for (int phase = 0; phase < factor_; ++phase) {
        output_oversampled[ch][i * factor_ + phase] = interpolate_polyphase_sample(
            input[ch], static_cast<size_t>(num_samples), static_cast<size_t>(i), phase, fir_);
      }
    }
  }
}

void TruePeakFilter::upsample_with_history(const float* const* input,
                                           float* const* output_oversampled, int num_channels,
                                           int num_samples,
                                           std::vector<std::vector<float>>& history) const {
  validate_buffers(input, num_channels, num_samples);
  if (num_channels == 0 || num_samples == 0) return;
  if (output_oversampled == nullptr) {
    throw std::invalid_argument("output must not be null");
  }
  const size_t history_size = static_cast<size_t>(std::max(0, fir_.taps_per_phase));
  if (history.size() != static_cast<size_t>(num_channels)) {
    history.assign(static_cast<size_t>(num_channels), std::vector<float>(history_size, 0.0f));
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    if (output_oversampled[ch] == nullptr) {
      throw std::invalid_argument("output channel must not be null");
    }
    auto& channel_history = history[static_cast<size_t>(ch)];
    if (channel_history.size() != history_size) {
      channel_history.assign(history_size, 0.0f);
    }

    std::vector<float> extended;
    extended.reserve(history_size + static_cast<size_t>(num_samples));
    extended.insert(extended.end(), channel_history.begin(), channel_history.end());
    extended.insert(extended.end(), input[ch], input[ch] + num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const size_t index = history_size + static_cast<size_t>(i);
      for (int phase = 0; phase < factor_; ++phase) {
        output_oversampled[ch][i * factor_ + phase] =
            interpolate_polyphase_sample(extended.data(), extended.size(), index, phase, fir_);
      }
    }

    const size_t keep = std::min(history_size, extended.size());
    std::copy(extended.end() - static_cast<std::ptrdiff_t>(keep), extended.end(),
              channel_history.end() - static_cast<std::ptrdiff_t>(keep));
    if (keep < history_size) {
      std::fill(channel_history.begin(), channel_history.end() - static_cast<std::ptrdiff_t>(keep),
                0.0f);
    }
  }
}

}  // namespace sonare::mastering::common
