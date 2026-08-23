#include "metering/true_peak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "rt/true_peak_fir.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::metering {

using sonare::constants::kEpsilon;
using sonare::constants::kFloorDb;

namespace {

// Output indices are screened in groups of this many. Small enough that the
// local maximum stays tight around a transient, large enough that the bound is
// evaluated once per few dozen interpolations rather than per interpolation.
constexpr size_t kScreenBlock = 64;

// Relative slack on the skip bound. The bound itself is exact in real
// arithmetic; the margin absorbs the rounding of the tap sum and of the double
// accumulation inside interpolate_polyphase_sample, so a group is never dropped
// on a bound that rounded a hair below the sample it was covering. It is far
// larger than either error and far smaller than anything a peak meter resolves,
// so it costs no pruning.
constexpr double kBoundSlack = 1.0 + 1e-6;

// Per-phase sum of |tap|, the operator norm that bounds an interpolated sample
// by the largest input magnitude its stencil can reach.
std::vector<double> phase_gains(const ::sonare::rt::PolyphaseFir& fir) {
  std::vector<double> gains(static_cast<size_t>(std::max(0, fir.phases)), 0.0);
  for (int phase = 0; phase < fir.phases; ++phase) {
    const float* row = fir.phase_row(phase);
    double sum = 0.0;
    for (int tap = 0; tap < fir.taps_per_phase; ++tap) {
      sum += std::abs(static_cast<double>(row[tap]));
    }
    gains[static_cast<size_t>(phase)] = sum * kBoundSlack;
  }
  return gains;
}

// Running maxima of |data| over kScreenBlock-sized spans, so the bound for a
// group of output samples reads a handful of values instead of rescanning the
// stencil.
std::vector<float> block_maxima(const float* data, size_t length) {
  std::vector<float> maxima((length + kScreenBlock - 1) / kScreenBlock, 0.0f);
  for (size_t block = 0; block < maxima.size(); ++block) {
    const size_t begin = block * kScreenBlock;
    const size_t end = std::min(begin + kScreenBlock, length);
    maxima[block] = peak_abs(data + begin, end - begin);
  }
  return maxima;
}

/// Interpolates only where an interpolated sample could still beat the running
/// peak.
///
/// The polyphase stencil for output index @c i reaches inputs
/// `[i - (taps_per_phase - 1 - half), i + half]`, so `|y| <= gain(phase) *
/// max|x|` over that span. Whenever that bound does not exceed the peak found so
/// far, the whole group of output samples is provably unable to raise it and is
/// skipped. The peak is exact: only interpolations that cannot win are dropped,
/// and the ones that run are the same arithmetic in a different visiting order
/// (max does not care).
float upsampled_peak(const float* data, size_t length, int oversample_factor) {
  const auto& pf = ::sonare::rt::true_peak_fir_for(oversample_factor);
  float peak = peak_abs(data, length);
  if (pf.phases <= 0 || pf.taps_per_phase <= 0) return peak;

  const std::vector<double> gains = phase_gains(pf);
  const std::vector<float> maxima = block_maxima(data, length);
  const size_t half = static_cast<size_t>(pf.taps_per_phase / 2);
  const size_t lookbehind = static_cast<size_t>(pf.taps_per_phase - 1) - half;

  for (size_t begin = 0; begin < length; begin += kScreenBlock) {
    const size_t end = std::min(begin + kScreenBlock, length);
    // Widest input span any output sample in [begin, end) can read.
    const size_t span_begin = begin > lookbehind ? begin - lookbehind : 0;
    const size_t span_end = std::min(end + half, length);
    float local = 0.0f;
    for (size_t block = span_begin / kScreenBlock; block <= (span_end - 1) / kScreenBlock;
         ++block) {
      local = std::max(local, maxima[block]);
    }

    for (int phase = 0; phase < pf.phases; ++phase) {
      if (gains[static_cast<size_t>(phase)] * static_cast<double>(local) <=
          static_cast<double>(peak)) {
        continue;
      }
      for (size_t i = begin; i < end; ++i) {
        const float sample = ::sonare::rt::interpolate_polyphase_sample(data, length, i, phase, pf);
        peak = std::max(peak, std::abs(sample));
      }
    }
  }
  return peak;
}

}  // namespace

float true_peak(const float* data, size_t length, int oversample_factor) {
  SONARE_CHECK(oversample_factor >= 1, ErrorCode::InvalidParameter);
  // Reject factors without a dedicated polyphase design instead of silently
  // falling back to 4x and reporting a result mislabeled with the requested
  // factor. Supported factors: 1, 2, 4, 8, 16.
  SONARE_CHECK_MSG(::sonare::rt::is_supported_polyphase_oversample_factor(oversample_factor),
                   ErrorCode::InvalidParameter,
                   "true_peak oversample_factor must be one of 1, 2, 4, 8, 16");
  SONARE_CHECK(data != nullptr || length == 0, ErrorCode::InvalidParameter);
  if (length == 0) return 0.0f;

  if (oversample_factor == 1 || length < 2) {
    return peak_abs(data, length);
  }
  return upsampled_peak(data, length, oversample_factor);
}

float true_peak(const Audio& audio, int oversample_factor) {
  return true_peak(audio.data(), audio.size(), oversample_factor);
}

float true_peak_db(const Audio& audio, int oversample_factor) {
  const float peak = true_peak(audio, oversample_factor);
  // Silence floor: report the shared finite dB floor (kFloorDb, -120 dB) rather
  // than -inf, matching the spectrum and dynamic-range meters. A finite floor is
  // JSON-safe and gives every level-in-dB meter the same "silence" sentinel.
  if (peak < kEpsilon) return kFloorDb;
  return linear_to_db(peak);
}

}  // namespace sonare::metering
