#include "editing/voice_changer/streaming_f0.h"

#include <algorithm>
#include <cmath>

#include "feature/pitch.h"
#include "rt/polyphase_fir.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kSqrt2D;

namespace {

constexpr int kDecimation = 3;
constexpr int kDecimationTaps = 49;
// Linear-phase group delay of the decimation FIR, in input and in analysis samples.
constexpr int kGroupDelayInput = (kDecimationTaps - 1) / 2;
constexpr int kGroupDelayAnalysis = kGroupDelayInput / kDecimation;
static_assert(kGroupDelayInput % kDecimation == 0,
              "the group delay must land on an analysis sample");
constexpr double kHopSeconds = 0.002;
constexpr double kMaxF0Hz = 1400.0;
constexpr float kAbsoluteThreshold = 0.12f;
// Half an octave: dips up to this ratio above the first one belong to its candidate, short of
// the subharmonic alternative at twice the lag.
constexpr double kSameCandidateRatio = kSqrt2D;
constexpr float kVoicedBelow = 0.15f;
constexpr float kUnvoicedAbove = 0.25f;
constexpr int kVoicingRun = 2;
constexpr float kJumpCents = 300.0f;
constexpr float kContinuityMargin = 0.05f;

/// Vertex offset in [-1, 1] of the parabola through (tau - 1, tau, tau + 1).
float parabolic_offset(const std::vector<float>& v, int tau) noexcept {
  if (tau < 1 || tau + 1 >= static_cast<int>(v.size())) return 0.0f;
  const float a = v[static_cast<size_t>(tau - 1)];
  const float b = v[static_cast<size_t>(tau)];
  const float c = v[static_cast<size_t>(tau + 1)];
  const float denom = a - 2.0f * b + c;
  if (!(denom > 0.0f)) return 0.0f;
  return std::clamp(0.5f * (a - c) / denom, -1.0f, 1.0f);
}

bool is_local_minimum(const std::vector<float>& v, int tau) noexcept {
  if (tau < 1 || tau + 1 >= static_cast<int>(v.size())) return false;
  const float b = v[static_cast<size_t>(tau)];
  return b <= v[static_cast<size_t>(tau - 1)] && b <= v[static_cast<size_t>(tau + 1)];
}

/// Depth of the dip at tau, read at the parabola vertex: at a lag between two integers both
/// neighbouring samples sit above the true minimum.
float dip_depth(const std::vector<float>& v, int tau) noexcept {
  const float b = v[static_cast<size_t>(tau)];
  if (tau < 1 || tau + 1 >= static_cast<int>(v.size())) return b;
  const float a = v[static_cast<size_t>(tau - 1)];
  const float c = v[static_cast<size_t>(tau + 1)];
  const float denom = a - 2.0f * b + c;
  if (!(denom > 0.0f)) return b;
  return std::clamp(b - 0.125f * (a - c) * (a - c) / denom, 0.0f, b);
}

}  // namespace

void StreamingF0Tracker::prepare(double sr, int max_block, float f0_min_hz) {
  if (!std::isfinite(sr) || sr <= 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be finite and positive");
  }
  if (max_block < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block must be non-negative");
  }
  if (!std::isfinite(f0_min_hz) || f0_min_hz <= 0.0f ||
      static_cast<double>(f0_min_hz) >= kMaxF0Hz) {
    throw SonareException(ErrorCode::InvalidParameter, "f0_min_hz must lie in (0, 1400) Hz");
  }
  analysis_rate_ = sr / kDecimation;
  hop_analysis_ = std::max(1, static_cast<int>(std::lround(kHopSeconds * analysis_rate_)));
  hop_input_ = kDecimation * hop_analysis_;
  tau_max_ = static_cast<int>(std::ceil(analysis_rate_ / static_cast<double>(f0_min_hz)));
  tau_min_ = std::max(2, static_cast<int>(std::floor(analysis_rate_ / kMaxF0Hz)));
  tau_max_ = std::max(tau_max_, tau_min_ + 2);
  // The window's later half (T_max) plus the FIR group delay, rounded up to the hop grid.
  const int needed = kDecimation * tau_max_ + kGroupDelayInput;
  decision_lag_ = (needed + hop_input_ - 1) / hop_input_ * hop_input_;

  const std::vector<float> taps = rt::design_windowed_sinc_lowpass(kDecimationTaps, kDecimation);
  fir_taps_.resize(taps.size());
  for (size_t i = 0; i < taps.size(); ++i) fir_taps_[i] = taps[i] / kDecimation;
  fir_history_.assign(2 * static_cast<size_t>(kDecimationTaps), 0.0f);

  // Holds the full 2 * tau_max span plus the samples decimated after it within one hop.
  std::int64_t ring_size = 1;
  while (ring_size < 2 * tau_max_ + 2 * hop_analysis_ + kGroupDelayAnalysis + 1) ring_size <<= 1;
  ring_.assign(static_cast<size_t>(ring_size), 0.0f);
  ring_mask_ = ring_size - 1;

  frame_.assign(2 * static_cast<size_t>(tau_max_), 0.0f);
  diff_.assign(static_cast<size_t>(tau_max_) + 1, 0.0f);
  cmndf_.assign(diff_.size(), 0.0f);
  reset();
}

void StreamingF0Tracker::reset() {
  std::fill(fir_history_.begin(), fir_history_.end(), 0.0f);
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  fir_pos_ = 0;
  decimated_count_ = 0;
  input_count_ = 0;
  voiced_ = false;
  voiced_run_ = 0;
  unvoiced_run_ = 0;
  previous_lag_ = 0.0f;
}

int StreamingF0Tracker::decision_lag_samples() const noexcept { return decision_lag_; }

int StreamingF0Tracker::hop_input_samples() const noexcept { return hop_input_; }

float StreamingF0Tracker::lag_to_hz(float lag) const noexcept {
  return static_cast<float>(analysis_rate_ / static_cast<double>(lag));
}

void StreamingF0Tracker::push_decimated(float y) noexcept {
  ring_[static_cast<size_t>(decimated_count_ & ring_mask_)] = y;
  ++decimated_count_;
}

int StreamingF0Tracker::process(const float* in, int n, F0Estimate* out, int max_out) noexcept {
  if (hop_input_ <= 0 || in == nullptr || n <= 0) return 0;
  int written = 0;
  const int taps = kDecimationTaps;
  for (int i = 0; i < n; ++i) {
    fir_history_[static_cast<size_t>(fir_pos_)] = in[i];
    fir_history_[static_cast<size_t>(fir_pos_ + taps)] = in[i];
    if (input_count_ % kDecimation == 0) {
      // Newest sample at fir_pos_ + taps, oldest at fir_pos_ + 1.
      const float* newest = fir_history_.data() + fir_pos_ + taps;
      float y = 0.0f;
      for (int j = 0; j < taps; ++j) y += fir_taps_[static_cast<size_t>(j)] * newest[-j];
      push_decimated(y);
    }
    fir_pos_ = fir_pos_ + 1 == taps ? 0 : fir_pos_ + 1;
    ++input_count_;
    if (input_count_ % hop_input_ == 0 && input_count_ >= decision_lag_) {
      const F0Estimate estimate = analyse(input_count_ - decision_lag_);
      if (out != nullptr && written < max_out) out[written++] = estimate;
    }
  }
  return written;
}

F0Estimate StreamingF0Tracker::analyse(std::int64_t centre_sample) noexcept {
  // Analysis sample c represents input sample kDecimation * c - kGroupDelayInput.
  const std::int64_t c = (centre_sample + kGroupDelayInput) / kDecimation;
  const std::int64_t first = c - tau_max_;
  const int span = 2 * tau_max_;
  for (int f = 0; f < span; ++f) {
    frame_[static_cast<size_t>(f)] = ring_[static_cast<size_t>((first + f) & ring_mask_)];
  }

  // Each lag compares two windows of tau_max samples placed symmetrically about the centre.
  const int window = tau_max_;
  diff_[0] = 0.0f;
  for (int tau = 1; tau <= tau_max_; ++tau) {
    const int start = tau_max_ - (window + tau + 1) / 2;
    const float* a = frame_.data() + start;
    const float* b = a + tau;
    float sum = 0.0f;
    for (int i = 0; i < window; ++i) {
      const float d = a[i] - b[i];
      sum += d * d;
    }
    diff_[static_cast<size_t>(tau)] = sum;
  }
  yin_cmndf_into(diff_, cmndf_);

  // The first dip under the threshold names the candidate; its deepest dip within half an octave
  // is the period, since an upper-formant ripple can dip under the threshold on the near flank.
  int best = -1;
  for (int tau = tau_min_; tau < tau_max_; ++tau) {
    if (!is_local_minimum(cmndf_, tau) || !(dip_depth(cmndf_, tau) < kAbsoluteThreshold)) continue;
    const int last = std::min(tau_max_ - 1, static_cast<int>(std::ceil(tau * kSameCandidateRatio)));
    best = tau;
    for (int t = tau + 1; t <= last; ++t) {
      if (is_local_minimum(cmndf_, t) && dip_depth(cmndf_, t) < dip_depth(cmndf_, best)) best = t;
    }
    break;
  }
  if (best < 0) {
    best = tau_min_;
    for (int tau = tau_min_ + 1; tau <= tau_max_; ++tau) {
      if (cmndf_[static_cast<size_t>(tau)] < cmndf_[static_cast<size_t>(best)]) best = tau;
    }
  }

  // A jump of more than kJumpCents from a voiced predecessor yields to a near-equal dip close to
  // the previous period.
  float lag = static_cast<float>(best) + parabolic_offset(diff_, best);
  if (voiced_ && previous_lag_ > 0.0f &&
      std::abs(kCentsPerOctave * std::log2(lag / previous_lag_)) > kJumpCents) {
    const float ratio = std::exp2(kJumpCents / kCentsPerOctave);
    const int lo = std::max(tau_min_, static_cast<int>(std::ceil(previous_lag_ / ratio)));
    const int hi = std::min(tau_max_ - 1, static_cast<int>(std::floor(previous_lag_ * ratio)));
    int near = -1;
    for (int tau = lo; tau <= hi; ++tau) {
      if (is_local_minimum(cmndf_, tau) &&
          (near < 0 || dip_depth(cmndf_, tau) < dip_depth(cmndf_, near))) {
        near = tau;
      }
    }
    if (near >= 0 && dip_depth(cmndf_, near) - dip_depth(cmndf_, best) < kContinuityMargin) {
      best = near;
      lag = static_cast<float>(best) + parabolic_offset(diff_, best);
    }
  }

  const float score = dip_depth(cmndf_, best);
  if (score < kVoicedBelow) {
    ++voiced_run_;
    unvoiced_run_ = 0;
  } else if (score > kUnvoicedAbove) {
    ++unvoiced_run_;
    voiced_run_ = 0;
  } else {
    voiced_run_ = 0;
    unvoiced_run_ = 0;
  }
  if (!voiced_ && voiced_run_ >= kVoicingRun) voiced_ = true;
  if (voiced_ && unvoiced_run_ >= kVoicingRun) voiced_ = false;
  previous_lag_ = lag;

  F0Estimate estimate{};
  estimate.f0_hz = lag_to_hz(lag);
  estimate.confidence = std::clamp(1.0f - score, 0.0f, 1.0f);
  estimate.voiced = voiced_;
  estimate.centre_sample = centre_sample;
  return estimate;
}

}  // namespace sonare::editing::voice_changer
