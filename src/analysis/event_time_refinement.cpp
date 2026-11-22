#include "analysis/event_time_refinement.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/db.h"

namespace sonare {

using constants::kMsPerSecondD;

namespace {

constexpr double kFineWindowMs = 5.0;
constexpr double kFineHopMs = 1.25;
/// Depth under the window's loudest fine frame below which levels are not told apart.
constexpr float kFloorDepthDb = 50.0f;
constexpr float kAbsoluteFloorDb = -100.0f;
/// Smallest rise read as an attack rather than as fluctuation of a sustained level.
constexpr float kMinRiseDb = 6.0f;

int samples_from_ms(double ms, int sample_rate) {
  return std::max(1, static_cast<int>(std::lround(ms * sample_rate / kMsPerSecondD)));
}

}  // namespace

float peak_short_time_rms(const Audio& audio, double start_s, double end_s) {
  const int sr = audio.sample_rate();
  const long n = static_cast<long>(audio.size());
  if (n == 0) return 0.0f;
  const long win = std::min<long>(samples_from_ms(kFineWindowMs, sr), n);
  const long first = std::clamp(std::lround(start_s * sr), 0L, n - win);
  const long last = std::clamp(std::lround(end_s * sr), first, n - win);
  const float* x = audio.data();
  double sum = 0.0;
  for (long k = first; k < first + win; ++k) {
    sum += static_cast<double>(x[k]) * x[k];
  }
  double peak = sum;
  for (long k = first + 1; k <= last; ++k) {
    sum += static_cast<double>(x[k + win - 1]) * x[k + win - 1] -
           static_cast<double>(x[k - 1]) * x[k - 1];
    peak = std::max(peak, sum);
  }
  return static_cast<float>(std::sqrt(std::max(0.0, peak) / static_cast<double>(win)));
}

EventTimeRefiner::EventTimeRefiner(const Audio& audio) {
  const int sr = audio.sample_rate();
  const int win = samples_from_ms(kFineWindowMs, sr);
  const int hop = samples_from_ms(kFineHopMs, sr);
  sample_rate_ = sr;
  hop_ = hop;
  half_window_ = win / 2;
  // The two frames a rise compares share no sample, so a sliver of the event cannot lift the
  // earlier one off the floor.
  rise_frames_ = (win + hop - 1) / hop;

  const float* x = audio.data();
  const long n = static_cast<long>(audio.size());
  const long frames = n / hop + 1;
  level_db_.resize(static_cast<size_t>(frames));
  for (long m = 0; m < frames; ++m) {
    const long start = m * hop - win / 2;
    const long lo = std::max(0L, start);
    const long hi = std::min(n, start + win);
    double sum = 0.0;
    for (long k = lo; k < hi; ++k) {
      sum += static_cast<double>(x[k]) * x[k];
    }
    level_db_[static_cast<size_t>(m)] = static_cast<float>(power_to_db_scalar(sum / win));
  }
}

std::vector<std::optional<float>> EventTimeRefiner::refine(const std::vector<float>& frame_times,
                                                           float half_window_s) const {
  const size_t count = frame_times.size();
  std::vector<std::optional<float>> refined(count);
  const long frames = static_cast<long>(level_db_.size());
  // Rise index m places the event at the start of fine frame m.
  auto time_of = [&](long m) {
    return static_cast<double>(m * hop_ - half_window_) / sample_rate_;
  };
  const long lookback = std::lround(half_window_s * sample_rate_ / hop_);
  for (size_t i = 0; i < count; ++i) {
    const double t = frame_times[i];
    double lo = t - half_window_s;
    double hi = t + half_window_s;
    const bool has_next = i + 1 < count;
    if (i > 0) lo = std::max(lo, 0.5 * (frame_times[i - 1] + t));
    if (has_next) hi = std::min(hi, 0.5 * (t + frame_times[i + 1]));

    // The next neighbour's midpoint is excluded so two refined events can never coincide.
    long m_lo = static_cast<long>(std::ceil((lo * sample_rate_ + half_window_) / hop_));
    long m_hi = static_cast<long>(std::floor((hi * sample_rate_ + half_window_) / hop_));
    if (has_next && time_of(m_hi) >= hi) --m_hi;
    m_lo = std::max<long>(m_lo, rise_frames_);
    m_hi = std::min(m_hi, frames - 1);
    if (m_lo > m_hi) continue;

    float peak = kAbsoluteFloorDb;
    for (long m = std::max(0L, m_lo - rise_frames_ - lookback); m <= m_hi; ++m) {
      peak = std::max(peak, level_db_[static_cast<size_t>(m)]);
    }
    const float floor_db = std::max(peak - kFloorDepthDb, kAbsoluteFloorDb);
    auto clamped = [&](long m) { return std::max(level_db_[static_cast<size_t>(m)], floor_db); };

    // A rise is measured over the loudest level of the lookback that ends where frame m begins,
    // so a modulated bed climbing out of its own dip does not count as new energy.
    float best_rise = 0.0f;
    long best_m = -1;
    for (long m = m_lo; m <= m_hi; ++m) {
      float before = floor_db;
      for (long k = std::max(0L, m - rise_frames_ - lookback); k <= m - rise_frames_; ++k) {
        before = std::max(before, clamped(k));
      }
      const float rise = clamped(m) - before;
      if (rise > best_rise) {
        best_rise = rise;
        best_m = m;
      }
    }
    if (best_m >= 0 && best_rise >= kMinRiseDb) {
      refined[i] = static_cast<float>(time_of(best_m));
    }
  }
  return refined;
}

}  // namespace sonare
