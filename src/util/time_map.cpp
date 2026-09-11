#include "util/time_map.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <utility>

#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"

namespace sonare {
namespace {

constexpr size_t kMaxCount =
    std::min<size_t>(resource::kMaxOfflineAudioSamples, static_cast<size_t>(INT_MAX));

/// Rounds a real output position up to a whole frame and rejects anything the
/// scalar form would have rejected.
size_t checked_ceil(double position) {
  SONARE_CHECK(std::isfinite(position), ErrorCode::InvalidParameter);
  const double rounded = std::ceil(position);
  SONARE_CHECK(rounded >= 0.0 && rounded <= static_cast<double>(kMaxCount),
               ErrorCode::InvalidParameter);
  return static_cast<size_t>(rounded);
}

/// Segment covering output position @p x.
size_t segment_at(const std::vector<double>& output_start, double x) {
  size_t index = 0;
  while (index + 1 < output_start.size() && x >= output_start[index + 1]) ++index;
  return index;
}

/// The piecewise-affine position itself, before any narrowing. Both counts are
/// defined on this rather than on what input_position returns: the narrowed
/// value can round up onto the input end and report a frame not yet reached.
double exact_position(const std::vector<TimeStretchSegment>& segments,
                      const std::vector<double>& output_start, double x) {
  const size_t index = segment_at(output_start, x);
  return static_cast<double>(segments[index].input_start) +
         (x - output_start[index]) * static_cast<double>(segments[index].rate);
}

/// Output position reading exactly @p target input frames, the inverse of the
/// piecewise-affine map on the reals.
double solve_output(const std::vector<TimeStretchSegment>& segments,
                    const std::vector<double>& output_start, double target) {
  SONARE_CHECK(std::isfinite(target), ErrorCode::InvalidParameter);
  size_t index = 0;
  while (index + 1 < segments.size() &&
         target >= static_cast<double>(segments[index + 1].input_start)) {
    ++index;
  }
  return output_start[index] + (target - static_cast<double>(segments[index].input_start)) /
                                   static_cast<double>(segments[index].rate);
}

/// Smallest float at or above @p value. A rate stored below the exact one
/// lengthens the stretch region and costs a whole frame at the ceiling.
float narrow_up(double value) {
  const float narrowed = static_cast<float>(value);
  return static_cast<double>(narrowed) < value
             ? std::nextafterf(narrowed, std::numeric_limits<float>::infinity())
             : narrowed;
}

}  // namespace

TimeStretchMap::TimeStretchMap(float rate) { assign(rate); }

TimeStretchMap::TimeStretchMap(std::vector<TimeStretchSegment> segments) { assign(segments); }

void TimeStretchMap::assign(float rate) {
  SONARE_CHECK(numeric::finite_positive(rate), ErrorCode::InvalidParameter);
  segments_.assign(1, TimeStretchSegment{0.0f, rate});
  output_start_.assign(1, 0.0);
}

void TimeStretchMap::assign(const std::vector<TimeStretchSegment>& segments) {
  // Validated in full, including the derived output starts, before anything is
  // written: a rejected profile leaves the map as it was.
  SONARE_CHECK(!segments.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(segments.front().input_start == 0.0f, ErrorCode::InvalidParameter);
  for (size_t i = 0; i < segments.size(); ++i) {
    SONARE_CHECK(numeric::finite_positive(segments[i].rate), ErrorCode::InvalidParameter);
    // Finiteness as well as order: an infinite breakpoint is strictly greater
    // than its predecessor and would place a segment no output frame reaches.
    SONARE_CHECK(numeric::finite_non_negative(segments[i].input_start),
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(i == 0 || segments[i].input_start > segments[i - 1].input_start,
                 ErrorCode::InvalidParameter);
  }
  double running = 0.0;
  for (size_t i = 1; i < segments.size(); ++i) {
    const double span = static_cast<double>(segments[i].input_start) -
                        static_cast<double>(segments[i - 1].input_start);
    const double next = running + span / static_cast<double>(segments[i - 1].rate);
    // A far breakpoint over a tiny rate puts the running start high enough that
    // the next increment is absorbed, which would leave two segments sharing an
    // output position and break the strict increase monotonicity rests on.
    SONARE_CHECK(next > running, ErrorCode::InvalidParameter);
    running = next;
  }

  segments_ = segments;
  output_start_.assign(segments_.size(), 0.0);
  for (size_t i = 1; i < segments_.size(); ++i) {
    const double span = static_cast<double>(segments_[i].input_start) -
                        static_cast<double>(segments_[i - 1].input_start);
    output_start_[i] = output_start_[i - 1] + span / static_cast<double>(segments_[i - 1].rate);
  }
}

float TimeStretchMap::input_position(int output_frame) const noexcept {
  if (segments_.size() == 1) {
    // The expression the scalar sites use, not an equivalent of it, so the
    // equality holds by construction rather than by a rounding argument.
    return static_cast<float>(output_frame) * segments_[0].rate;
  }
  return static_cast<float>(
      exact_position(segments_, output_start_, static_cast<double>(output_frame)));
}

int TimeStretchMap::output_frame_count(int input_frame_count) const {
  // Zero is a count, not a rejection: the scalar form projects it to zero and
  // the equality with it covers that argument too. Callers keep their own
  // positivity rejection where they have one.
  SONARE_CHECK(input_frame_count >= 0, ErrorCode::InvalidParameter);
  if (segments_.size() == 1) {
    size_t count = 0;
    SONARE_CHECK(numeric::checked_projected_count(static_cast<size_t>(input_frame_count),
                                                  segments_[0].rate, kMaxCount, &count),
                 ErrorCode::InvalidParameter);
    return static_cast<int>(count);
  }
  return static_cast<int>(
      checked_ceil(solve_output(segments_, output_start_, static_cast<double>(input_frame_count))));
}

size_t TimeStretchMap::output_sample_count(size_t input_sample_count, int hop_length) const {
  SONARE_CHECK(hop_length > 0, ErrorCode::InvalidParameter);
  if (segments_.size() == 1) {
    size_t count = 0;
    SONARE_CHECK(
        numeric::checked_projected_count(input_sample_count, segments_[0].rate, kMaxCount, &count),
        ErrorCode::InvalidParameter);
    return count;
  }
  const double hop = static_cast<double>(hop_length);
  const double frames =
      solve_output(segments_, output_start_, static_cast<double>(input_sample_count) / hop);
  return checked_ceil(frames * hop);
}

bool TimeStretchMap::constant() const noexcept { return segments_.size() == 1; }

float TimeStretchMap::constant_rate() const noexcept { return segments_[0].rate; }

bool TimeStretchMap::agrees_through(const TimeStretchMap& other,
                                    int committed_output_frames) const noexcept {
  if (committed_output_frames < 0) return true;
  const double limit = static_cast<double>(committed_output_frames);
  double x = 0.0;
  while (true) {
    if (exact_position(segments_, output_start_, x) !=
        exact_position(other.segments_, other.output_start_, x)) {
      return false;
    }
    if (x >= limit) return true;
    const size_t here = segment_at(output_start_, x);
    const size_t there = segment_at(other.output_start_, x);
    if (segments_[here].rate != other.segments_[there].rate) return false;
    double next = limit;
    if (here + 1 < output_start_.size()) next = std::min(next, output_start_[here + 1]);
    if (there + 1 < other.output_start_.size()) {
      next = std::min(next, other.output_start_[there + 1]);
    }
    if (!(next > x)) return true;
    x = next;
  }
}

std::vector<TimeStretchSegment> hold_profile(const std::vector<HoldRange>& holds,
                                             int input_frame_count, float target_rate) {
  SONARE_CHECK(input_frame_count > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(target_rate), ErrorCode::InvalidParameter);

  int64_t held = 0;
  int cursor = 0;
  for (const HoldRange& hold : holds) {
    SONARE_CHECK(hold.frame_count > 0 && hold.first_frame >= cursor &&
                     hold.first_frame + hold.frame_count <= input_frame_count,
                 ErrorCode::InvalidParameter);
    cursor = hold.first_frame + hold.frame_count;
    held += hold.frame_count;
  }

  if (holds.empty()) {
    return {TimeStretchSegment{0.0f, target_rate}};
  }

  const double n = input_frame_count;
  const double h = static_cast<double>(held);
  const double requested = n / static_cast<double>(target_rate);
  // Two conditions, not one. With every frame held there is no stretch region
  // and no rate changes the total, so only unity can be satisfied.
  if (held == input_frame_count) {
    SONARE_CHECK(target_rate == 1.0f, ErrorCode::InvalidParameter);
    return {TimeStretchSegment{0.0f, 1.0f}};
  }
  SONARE_CHECK(requested > h, ErrorCode::InvalidParameter);
  const float rate = narrow_up((n - h) / (requested - h));

  std::vector<TimeStretchSegment> segments;
  segments.reserve(2 * holds.size() + 1);
  cursor = 0;
  for (const HoldRange& hold : holds) {
    // No stretch segment where a hold starts at 0 or abuts the previous one:
    // a zero-width span would repeat an input_start and break strict increase.
    if (hold.first_frame > cursor) {
      segments.push_back(TimeStretchSegment{static_cast<float>(cursor), rate});
    }
    segments.push_back(TimeStretchSegment{static_cast<float>(hold.first_frame), 1.0f});
    cursor = hold.first_frame + hold.frame_count;
  }
  if (cursor < input_frame_count) {
    segments.push_back(TimeStretchSegment{static_cast<float>(cursor), rate});
  }

  // Validated through the path a map takes rather than by inspecting the result.
  const TimeStretchMap validated{segments};
  (void)validated;
  return segments;
}

}  // namespace sonare
