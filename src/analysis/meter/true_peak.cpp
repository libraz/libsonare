#include "analysis/meter/true_peak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::analysis::meter {
namespace {

constexpr double kPi = 3.14159265358979323846;

// BS.1770-4 inter-sample peak meter uses 4x oversampling with a 48-tap FIR
// (4 phases x 12 taps). The filter is a Hann-windowed sinc with cutoff at the
// base-rate Nyquist (i.e. fs/(2 * oversample)).
std::vector<float> design_lowpass_taps(int total_taps, int oversample_factor) {
  std::vector<float> taps(static_cast<size_t>(total_taps), 0.0f);
  const double center = (total_taps - 1) * 0.5;
  const double cutoff = 1.0 / static_cast<double>(oversample_factor);  // normalized to fs_os
  for (int n = 0; n < total_taps; ++n) {
    const double m = static_cast<double>(n) - center;
    double sinc_value;
    if (std::abs(m) < 1e-9) {
      sinc_value = cutoff;
    } else {
      sinc_value = std::sin(kPi * cutoff * m) / (kPi * m);
    }
    const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                               static_cast<double>(total_taps - 1));
    taps[static_cast<size_t>(n)] = static_cast<float>(sinc_value * window);
  }

  // Normalize to unity DC gain after polyphase decomposition (each phase keeps
  // gain ~ 1/oversample_factor, so total chain gain is 1).
  double dc_gain = 0.0;
  for (float v : taps) dc_gain += v;
  if (std::abs(dc_gain) > 1e-12) {
    const float scale = static_cast<float>(static_cast<double>(oversample_factor) / dc_gain);
    for (float& v : taps) v *= scale;
  }
  return taps;
}

struct PolyphaseFilter {
  int phases = 4;
  int taps_per_phase = 12;
  std::vector<std::vector<float>> phase_taps;
};

PolyphaseFilter build_polyphase(int oversample_factor, int total_taps) {
  PolyphaseFilter pf;
  pf.phases = oversample_factor;
  pf.taps_per_phase = total_taps / oversample_factor;
  pf.phase_taps.assign(static_cast<size_t>(pf.phases),
                       std::vector<float>(static_cast<size_t>(pf.taps_per_phase), 0.0f));
  const auto taps = design_lowpass_taps(total_taps, oversample_factor);
  for (int p = 0; p < pf.phases; ++p) {
    for (int k = 0; k < pf.taps_per_phase; ++k) {
      pf.phase_taps[static_cast<size_t>(p)][static_cast<size_t>(k)] =
          taps[static_cast<size_t>(k * pf.phases + p)];
    }
  }
  return pf;
}

const PolyphaseFilter& filter_for(int oversample_factor) {
  static const PolyphaseFilter kFilter4x = build_polyphase(4, 48);
  static const PolyphaseFilter kFilter8x = build_polyphase(8, 96);
  static const PolyphaseFilter kFilter2x = build_polyphase(2, 24);
  if (oversample_factor == 8) return kFilter8x;
  if (oversample_factor == 2) return kFilter2x;
  return kFilter4x;
}

float upsampled_peak(const float* data, size_t length, int oversample_factor) {
  const PolyphaseFilter& pf = filter_for(oversample_factor);
  const int taps = pf.taps_per_phase;
  const int half = taps / 2;
  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max(peak, std::abs(data[i]));
  }
  for (int phase = 1; phase < pf.phases; ++phase) {
    const auto& h = pf.phase_taps[static_cast<size_t>(phase)];
    for (size_t i = 0; i < length; ++i) {
      double accum = 0.0;
      for (int k = 0; k < taps; ++k) {
        const long src = static_cast<long>(i) + static_cast<long>(k) - static_cast<long>(half);
        if (src < 0 || src >= static_cast<long>(length)) continue;
        accum += static_cast<double>(h[static_cast<size_t>(k)]) *
                 static_cast<double>(data[static_cast<size_t>(src)]);
      }
      peak = std::max(peak, static_cast<float>(std::abs(accum)));
    }
  }
  return peak;
}

}  // namespace

float true_peak(const float* data, size_t length, int oversample_factor) {
  SONARE_CHECK(oversample_factor >= 1, ErrorCode::InvalidParameter);
  SONARE_CHECK(data != nullptr || length == 0, ErrorCode::InvalidParameter);
  if (length == 0) return 0.0f;

  if (oversample_factor == 1 || length < 2) {
    float peak = 0.0f;
    for (size_t i = 0; i < length; ++i) {
      peak = std::max(peak, std::abs(data[i]));
    }
    return peak;
  }
  return upsampled_peak(data, length, oversample_factor);
}

float true_peak(const Audio& audio, int oversample_factor) {
  return true_peak(audio.data(), audio.size(), oversample_factor);
}

float true_peak_db(const Audio& audio, int oversample_factor) {
  const float peak = true_peak(audio, oversample_factor);
  if (peak < kEpsilon) return -std::numeric_limits<float>::infinity();
  return 20.0f * std::log10(peak);
}

}  // namespace sonare::analysis::meter
