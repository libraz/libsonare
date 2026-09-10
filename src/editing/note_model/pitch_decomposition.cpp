#include "editing/note_model/pitch_decomposition.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::editing::note_model {
namespace {

using sonare::constants::kCentsPerOctave;

/// Three passes of a length-N moving average are 3 dB down at
/// 0.263 * frame_rate / N, and the window length inverts that. Three rather than
/// one: a single rectangular pass leaks vibrato into drift through its sidelobe.
constexpr float kCutoffToTaps = 0.263f;
constexpr int kSmoothingPasses = 3;

bool usable_pitch(float hz) noexcept { return hz > 0.0f && std::isfinite(hz); }

/// Odd window length the cutoff asks for, never longer than the note itself.
int smoothing_taps(float frame_rate_hz, float vibrato_cutoff_hz, int n) noexcept {
  const float raw =
      std::min(kCutoffToTaps * frame_rate_hz / vibrato_cutoff_hz, static_cast<float>(n));
  int taps = std::max(1, static_cast<int>(std::lround(raw)));
  if (taps % 2 == 0) ++taps;
  if (taps > n) taps = (n % 2 == 0) ? n - 1 : n;
  return std::max(1, taps);
}

/// One centred moving-average pass, edges handled by replicating the endpoint
/// sample so the curve is not pulled toward zero at either end.
std::vector<float> moving_average(const std::vector<float>& values, int taps) {
  const int n = static_cast<int>(values.size());
  const int half = taps / 2;
  std::vector<float> smoothed(values.size());
  for (int i = 0; i < n; ++i) {
    double sum = 0.0;
    for (int k = -half; k <= half; ++k) {
      sum += static_cast<double>(values[static_cast<size_t>(std::clamp(i + k, 0, n - 1))]);
    }
    smoothed[static_cast<size_t>(i)] = static_cast<float>(sum / static_cast<double>(taps));
  }
  return smoothed;
}

}  // namespace

PitchDecomposition decompose_pitch(const NoteObject& note, const PitchDecompositionConfig& config) {
  SONARE_CHECK(std::isfinite(config.vibrato_cutoff_hz) && config.vibrato_cutoff_hz > 0.0f,
               ErrorCode::InvalidParameter);

  PitchDecomposition result;
  result.frame_rate_hz = note.f0_hz.frame_rate_hz;
  result.frame_offset = note.f0_hz.frame_offset;
  result.config = config;

  const std::vector<float>& values = note.f0_hz.values;
  if (values.empty() || !usable_pitch(note.median_hz)) return result;

  const size_t n = values.size();
  std::vector<float> cents(n);
  size_t first_usable = n;
  float held = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    if (usable_pitch(values[i])) {
      held = kCentsPerOctave * std::log2(values[i] / note.median_hz);
      if (first_usable == n) first_usable = i;
    }
    // An unusable frame carries no measurement, so it holds its last neighbour.
    cents[i] = held;
  }
  // No frame is usable: the note claims a median nothing in it supports, and an
  // entirely held curve would be fabricated rather than measured.
  if (first_usable == n) return result;
  // Below the short circuits, so a note with nothing to decompose never fails
  // on a cadence that could not have been read anyway.
  SONARE_CHECK(note.f0_hz.frame_rate_hz > 0.0f && std::isfinite(note.f0_hz.frame_rate_hz),
               ErrorCode::InvalidParameter);
  std::fill(cents.begin(), cents.begin() + static_cast<std::ptrdiff_t>(first_usable),
            cents[first_usable]);

  std::vector<float> drift = cents;
  const int taps =
      smoothing_taps(note.f0_hz.frame_rate_hz, config.vibrato_cutoff_hz, static_cast<int>(n));
  for (int pass = 0; pass < kSmoothingPasses; ++pass) {
    drift = moving_average(drift, taps);
  }

  // Subtraction rather than a second filter, so the two curves sum back to the
  // measured one.
  std::vector<float> vibrato(n);
  for (size_t i = 0; i < n; ++i) {
    vibrato[i] = cents[i] - drift[i];
  }

  result.centre_hz = note.median_hz;
  result.drift = std::move(drift);
  result.vibrato = std::move(vibrato);
  return result;
}

}  // namespace sonare::editing::note_model
