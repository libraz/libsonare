#include "transport/tempo_map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "util/constants.h"

namespace sonare::transport {
namespace {

// Shared "find the last element whose key <= x" binary search used by all three
// segment lookups below, so the boundary semantics live in one place.
template <class Vec, class KeyFn>
size_t last_index_at_or_before(const Vec& v, double x, KeyFn key) noexcept {
  size_t lo = 0;
  size_t hi = v.size();
  while (lo + 1 < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (key(v[mid]) <= x) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

constexpr double kMinBpm = 1.0e-6;
// A ramp is treated as constant when the BPM change is below this, both to fall
// through to the exact legacy constant-tempo math and to avoid catastrophic
// cancellation in the logarithmic ramp formulas.
constexpr double kRampBpmEpsilon = 1.0e-9;

double samples_per_ppq(double sample_rate, double bpm) noexcept {
  const double safe_bpm = std::max(bpm, kMinBpm);
  return sample_rate * 60.0 / safe_bpm;
}

// Effective end BPM for a segment: a ramp is active only when end_bpm is
// finite, positive, has a meaningful difference from the start bpm, and the
// segment spans a positive ppq range.
double effective_end_bpm(const TempoSegment& segment) noexcept {
  if (!std::isfinite(segment.end_bpm) || segment.end_bpm <= 0.0) return segment.bpm;
  if (std::abs(segment.end_bpm - segment.bpm) < kRampBpmEpsilon) return segment.bpm;
  if (!std::isfinite(segment.end_ppq) || segment.end_ppq <= segment.start_ppq) return segment.bpm;
  return segment.end_bpm;
}

// Samples elapsed from a segment's start to ppq `p` (>= start_ppq). Constant
// segments use the exact legacy formula; ramped segments integrate 60/bpm(p).
double segment_samples_at_ppq(const TempoSegment& segment, double sample_rate, double p) noexcept {
  const double bpm0 = std::max(segment.bpm, kMinBpm);
  const double end_bpm = effective_end_bpm(segment);
  const double dp = p - segment.start_ppq;
  if (end_bpm == segment.bpm) {
    return dp * samples_per_ppq(sample_rate, bpm0);
  }
  // Linear BPM vs ppq: bpm(p) = bpm0 + slope * (p - start_ppq).
  const double slope = (std::max(end_bpm, kMinBpm) - bpm0) / (segment.end_ppq - segment.start_ppq);
  const double bpm_p = std::max(bpm0 + slope * dp, kMinBpm);
  // s(p) = (sr*60/slope) * ln(bpm(p)/bpm0).
  return (sample_rate * 60.0 / slope) * std::log(bpm_p / bpm0);
}

// Inverse of segment_samples_at_ppq: ppq reached after `s` samples from start.
double segment_ppq_at_samples(const TempoSegment& segment, double sample_rate, double s) noexcept {
  const double bpm0 = std::max(segment.bpm, kMinBpm);
  const double end_bpm = effective_end_bpm(segment);
  if (end_bpm == segment.bpm) {
    return segment.start_ppq + s / samples_per_ppq(sample_rate, bpm0);
  }
  const double slope = (std::max(end_bpm, kMinBpm) - bpm0) / (segment.end_ppq - segment.start_ppq);
  // bpm(p) = bpm0 * exp(s * slope / (sr*60)); p = start_ppq + (bpm(p) - bpm0)/slope.
  const double bpm_p = bpm0 * std::exp(s * slope / (sample_rate * 60.0));
  return segment.start_ppq + (bpm_p - bpm0) / slope;
}

double bar_length_ppq(TimeSignature sig) noexcept {
  const int numerator = std::max(sig.numerator, 1);
  const int denominator = std::max(sig.denominator, 1);
  return static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
}

std::vector<TempoSegment> normalize_segments(std::vector<TempoSegment> segments,
                                             double sample_rate) {
  segments.erase(std::remove_if(segments.begin(), segments.end(),
                                [](const TempoSegment& segment) {
                                  return !std::isfinite(segment.start_ppq) ||
                                         !std::isfinite(segment.bpm) || segment.bpm <= 0.0;
                                }),
                 segments.end());
  if (segments.empty()) {
    segments.push_back({0.0, constants::kDefaultBpm, 0.0});
  }
  std::sort(segments.begin(), segments.end(),
            [](const auto& a, const auto& b) { return a.start_ppq < b.start_ppq; });
  if (segments.front().start_ppq > 0.0) {
    segments.insert(segments.begin(), {0.0, segments.front().bpm, 0.0});
  }
  // Fill end_ppq (next segment start, or +inf for the last) so ramp formulas
  // know each segment's span before accumulating start_sample.
  for (size_t i = 0; i < segments.size(); ++i) {
    segments[i].end_ppq = (i + 1 < segments.size()) ? segments[i + 1].start_ppq
                                                    : std::numeric_limits<double>::infinity();
  }
  segments.front().start_sample = 0.0;
  for (size_t i = 1; i < segments.size(); ++i) {
    const TempoSegment& prev = segments[i - 1];
    // Accumulate the samples spanned by the previous segment up to this start.
    segments[i].start_sample =
        prev.start_sample + segment_samples_at_ppq(prev, sample_rate, segments[i].start_ppq);
  }
  return segments;
}

std::vector<TimeSignatureSegment> normalize_time_signatures(
    std::vector<TimeSignatureSegment> time_signatures) {
  time_signatures.erase(std::remove_if(time_signatures.begin(), time_signatures.end(),
                                       [](const TimeSignatureSegment& segment) {
                                         return !std::isfinite(segment.start_ppq) ||
                                                segment.time_sig.numerator <= 0 ||
                                                segment.time_sig.denominator <= 0;
                                       }),
                        time_signatures.end());
  if (time_signatures.empty()) {
    time_signatures.push_back({0.0, {4, 4}});
  }
  std::sort(time_signatures.begin(), time_signatures.end(),
            [](const auto& a, const auto& b) { return a.start_ppq < b.start_ppq; });
  if (time_signatures.front().start_ppq > 0.0) {
    TimeSignatureSegment first = time_signatures.front();
    first.start_ppq = 0.0;
    time_signatures.insert(time_signatures.begin(), first);
  }
  return time_signatures;
}

}  // namespace

void TempoMap::prepare(double sample_rate) {
  if (sample_rate > 0.0 && std::isfinite(sample_rate)) {
    sample_rate_ = sample_rate;
  }

  // Control-thread reads of the current snapshots are safe via load().
  const std::vector<TempoSegment>* segments = segments_.load();
  std::vector<TempoSegment> seg_values =
      segments ? *segments : std::vector<TempoSegment>{{0.0, constants::kDefaultBpm, 0.0}};
  segments_.publish(std::make_shared<const std::vector<TempoSegment>>(
      normalize_segments(std::move(seg_values), sample_rate_)));

  const std::vector<TimeSignatureSegment>* time_signatures = time_signatures_.load();
  std::vector<TimeSignatureSegment> sig_values =
      time_signatures ? *time_signatures : std::vector<TimeSignatureSegment>{{0.0, {4, 4}}};
  time_signatures_.publish(std::make_shared<const std::vector<TimeSignatureSegment>>(
      normalize_time_signatures(std::move(sig_values))));
}

void TempoMap::set_segments(std::vector<TempoSegment> segments) {
  segments_.publish(std::make_shared<const std::vector<TempoSegment>>(
      normalize_segments(std::move(segments), sample_rate_)));
}

void TempoMap::set_time_signatures(std::vector<TimeSignatureSegment> time_signatures) {
  time_signatures_.publish(std::make_shared<const std::vector<TimeSignatureSegment>>(
      normalize_time_signatures(std::move(time_signatures))));
}

double TempoMap::sample_to_ppq(int64_t sample) const noexcept {
  const std::vector<TempoSegment>* segments = segments_.load();
  if (!segments || segments->empty()) return 0.0;
  const double sample_d = static_cast<double>(sample);
  const size_t index = segment_index_for_sample(*segments, sample_d);
  const TempoSegment& segment = (*segments)[index];
  return segment_ppq_at_samples(segment, sample_rate_, sample_d - segment.start_sample);
}

int64_t TempoMap::ppq_to_sample(double ppq) const noexcept {
  const std::vector<TempoSegment>* segments = segments_.load();
  if (!segments || segments->empty()) return 0;
  // Guard non-finite input: llround on NaN/Inf is undefined behavior (mirrors
  // the isfinite guard in ppq_duration_to_samples).
  if (!std::isfinite(ppq)) return 0;
  const size_t index = segment_index_for_ppq(*segments, ppq);
  const TempoSegment& segment = (*segments)[index];
  const double sample = segment.start_sample + segment_samples_at_ppq(segment, sample_rate_, ppq);
  if (!std::isfinite(sample)) return ppq > 0.0 ? std::numeric_limits<int64_t>::max() : 0;
  const double clamped =
      std::clamp(sample, static_cast<double>(std::numeric_limits<int64_t>::min()),
                 static_cast<double>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(std::llround(clamped));
}

double TempoMap::bpm_at_sample(int64_t sample) const noexcept {
  const std::vector<TempoSegment>* segments = segments_.load();
  if (!segments || segments->empty()) return constants::kDefaultBpm;
  const TempoSegment& segment =
      (*segments)[segment_index_for_sample(*segments, static_cast<double>(sample))];
  const double end_bpm = effective_end_bpm(segment);
  if (end_bpm == segment.bpm) return segment.bpm;
  // Report the instantaneous ramped tempo at this sample position.
  const double ppq = segment_ppq_at_samples(segment, sample_rate_,
                                            static_cast<double>(sample) - segment.start_sample);
  const double slope = (std::max(end_bpm, kMinBpm) - std::max(segment.bpm, kMinBpm)) /
                       (segment.end_ppq - segment.start_ppq);
  return std::max(segment.bpm + slope * (ppq - segment.start_ppq), kMinBpm);
}

TimeSignature TempoMap::time_signature_at_ppq(double ppq) const noexcept {
  const std::vector<TimeSignatureSegment>* time_signatures = time_signatures_.load();
  if (!time_signatures || time_signatures->empty()) return TimeSignature{};
  return (*time_signatures)[time_signature_index_for_ppq(*time_signatures, ppq)].time_sig;
}

BarBeat TempoMap::ppq_to_bar_beat(double ppq) const noexcept {
  const std::vector<TimeSignatureSegment>* time_signatures = time_signatures_.load();
  if (!time_signatures || time_signatures->empty()) return BarBeat{};
  const auto& sigs = *time_signatures;
  const size_t sig_index = time_signature_index_for_ppq(sigs, ppq);
  const TimeSignature sig = sigs[sig_index].time_sig;
  const double start = bar_start_ppq_in(sigs, ppq);
  const double beat_len = 4.0 / static_cast<double>(std::max(sig.denominator, 1));
  const double offset = std::max(0.0, ppq - start);
  const int beat_index = static_cast<int>(std::floor(offset / beat_len));

  int64_t bar_count = 0;
  // Tolerance for treating a segment span as a whole number of bars, so a
  // signature that does fall on the bar grid is not bumped by floating-point
  // fuzz.
  constexpr double kBarEpsilon = 1.0e-9;
  for (size_t i = 0; i < sig_index; ++i) {
    const double next_start = sigs[i + 1].start_ppq;
    const double len = bar_length_ppq(sigs[i].time_sig);
    // A signature change that lands off the bar grid leaves a partial bar at the
    // end of this segment. That partial bar has still STARTED, so count it as a
    // full bar (ceil): otherwise the next segment's bar 0 would collide with the
    // partial bar's number, producing duplicate/wrong bar numbers across the
    // boundary. ceil reduces to floor when the span is an exact bar multiple.
    const double bars = std::max(0.0, next_start - sigs[i].start_ppq) / len;
    bar_count += static_cast<int64_t>(std::ceil(bars - kBarEpsilon));
  }
  const double current_len = bar_length_ppq(sig);
  bar_count += static_cast<int64_t>(
      std::floor(std::max(0.0, ppq - sigs[sig_index].start_ppq) / current_len));
  return {bar_count, beat_index + 1, (offset / beat_len) - static_cast<double>(beat_index)};
}

double TempoMap::bar_start_ppq(double ppq) const noexcept {
  const std::vector<TimeSignatureSegment>* time_signatures = time_signatures_.load();
  if (!time_signatures || time_signatures->empty()) return 0.0;
  return bar_start_ppq_in(*time_signatures, ppq);
}

double TempoMap::bar_start_ppq_in(const std::vector<TimeSignatureSegment>& time_signatures,
                                  double ppq) noexcept {
  const size_t sig_index = time_signature_index_for_ppq(time_signatures, ppq);
  const TimeSignature sig = time_signatures[sig_index].time_sig;
  const double len = bar_length_ppq(sig);
  if (len <= 0.0) return 0.0;
  const double sig_start = time_signatures[sig_index].start_ppq;
  return sig_start + std::floor(std::max(0.0, ppq - sig_start) / len) * len;
}

size_t TempoMap::segment_index_for_sample(const std::vector<TempoSegment>& segments,
                                          double sample) noexcept {
  return last_index_at_or_before(segments, sample,
                                 [](const TempoSegment& s) { return s.start_sample; });
}

size_t TempoMap::segment_index_for_ppq(const std::vector<TempoSegment>& segments,
                                       double ppq) noexcept {
  return last_index_at_or_before(segments, ppq, [](const TempoSegment& s) { return s.start_ppq; });
}

size_t TempoMap::time_signature_index_for_ppq(
    const std::vector<TimeSignatureSegment>& time_signatures, double ppq) noexcept {
  return last_index_at_or_before(time_signatures, ppq,
                                 [](const TimeSignatureSegment& s) { return s.start_ppq; });
}

}  // namespace sonare::transport
