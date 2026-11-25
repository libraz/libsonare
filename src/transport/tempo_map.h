#pragma once

/// @file tempo_map.h
/// @brief Piecewise tempo (constant or linearly-ramped) and time-signature map.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "rt/rt_publisher.h"
#include "util/constants.h"
#include "util/parameter_checks.h"

namespace sonare::transport {

/// Practical upper bound accepted by public control-plane PPQ APIs. At 120 BPM
/// (2 quarter notes/second) 1e12 quarter notes is on the order of 15,000 years,
/// which keeps obviously hostile magnitudes out of downstream timeline
/// arithmetic. TempoMap itself still saturates any larger finite value because
/// it is also used by internal/offline code.
inline constexpr double kMaxPublicPpq = 1.0e12;
/// Highest tempo accepted by public control-plane APIs. This is intentionally
/// far above a musical tempo while keeping MIDI-clock and beat-grid work
/// bounded enough for realtime code to enforce a small per-block budget.
inline constexpr double kMaxPublicTempoBpm = 100000.0;
/// Shortest lead-in a map can draw as one beat at kMaxPublicTempoBpm; beat detection places
/// a first beat nearer sample 0 than this on sample 0.
inline constexpr double kMinLeadInSec = ::sonare::constants::kSecondsPerMinute / kMaxPublicTempoBpm;
/// Lowest tempo accepted by public controls and used by internal tempo producers.
inline constexpr double kMinPublicTempoBpm = 1.0e-6;
/// Public tempo accepted by scalar controls and tempo segment endpoints.
inline constexpr auto kTempoBpmDomain =
    ::sonare::util::check::real_domain(kMinPublicTempoBpm, kMaxPublicTempoBpm);
/// Tempo segment endpoint domain with zero reserved for the no-ramp sentinel.
inline constexpr auto kTempoRampBpmDomain =
    ::sonare::util::check::with_sentinel(kTempoBpmDomain, 0.0);
/// Internal producers retain their existing lack of an upper tempo bound.
inline constexpr auto kInternalTempoBpmDomain =
    ::sonare::util::check::real_domain_above(kMinPublicTempoBpm, true);

inline bool valid_public_ppq(double ppq) noexcept { return ppq >= 0.0 && ppq <= kMaxPublicPpq; }

/// @brief Refuses a loop bound outside [0, kMaxPublicPpq], then, while @p enabled, an end at or
///        before the start.
/// @throws SonareParameterRefusal(InvalidParameter) naming the bound.
inline void check_loop_range(double start_ppq, double end_ppq, bool enabled) {
  ::sonare::util::check::in_range(start_ppq, 0.0, kMaxPublicPpq, "start_ppq");
  ::sonare::util::check::in_range(end_ppq, 0.0, kMaxPublicPpq, "end_ppq");
  if (enabled && !(end_ppq > start_ppq)) {
    ::sonare::util::check::refuse_relation(
        {"end_ppq", "start_ppq"}, "{0} must be greater than {1} while the loop is enabled");
  }
}

inline bool valid_public_tempo(double bpm) noexcept {
  return ::sonare::util::check::in_domain(bpm, kTempoBpmDomain);
}

inline bool valid_internal_tempo_bpm(double bpm) noexcept {
  return util::check::in_domain(bpm, kInternalTempoBpmDomain);
}

struct TempoSegment {
  double start_ppq = 0.0;
  double bpm = constants::kDefaultBpm;
  double start_sample = 0.0;
  /// Tempo (BPM) reached at the END of this segment (i.e. at the next segment's
  /// start_ppq). When <= 0 or equal to `bpm`, the segment is piecewise-constant
  /// and reduces EXACTLY to the legacy constant-tempo math. When > 0 and
  /// different from `bpm`, the segment ramps tempo linearly over ppq.
  double end_bpm = 0.0;
  /// Internal: ppq at the end of this segment (next segment start, or +inf for
  /// the last segment). Populated during normalization. Not user input.
  double end_ppq = 0.0;
};

inline bool valid_public_tempo_segment(const TempoSegment& segment) noexcept {
  return std::isfinite(segment.start_ppq) && valid_public_ppq(segment.start_ppq) &&
         valid_public_tempo(segment.bpm) &&
         (segment.end_bpm == *kTempoRampBpmDomain.sentinel ||
          util::check::in_domain(segment.end_bpm, kTempoRampBpmDomain));
}

namespace detail {

inline void check_tempo_segment_bpms(double bpm, double end_bpm, std::size_t index,
                                     const char* prefix, const char* end_field,
                                     const util::check::NumericDomain& bpm_domain,
                                     const util::check::NumericDomain& ramp_domain) {
  if (!util::check::in_domain(bpm, bpm_domain)) {
    util::check::refuse_outside(bpm_domain,
                                util::check::indexed_subject(prefix, index, "bpm").c_str());
  }
  if (end_bpm != *ramp_domain.sentinel && !util::check::in_domain(end_bpm, ramp_domain)) {
    util::check::refuse_outside(ramp_domain,
                                util::check::indexed_subject(prefix, index, end_field).c_str());
  }
}

}  // namespace detail

/// @brief Checks the authored BPM fields of one public tempo segment.
template <typename Segment>
inline void check_public_tempo_segment(const Segment& segment, std::size_t index,
                                       const char* prefix = "segments",
                                       const char* end_field = "end_bpm") {
  detail::check_tempo_segment_bpms(segment.bpm, segment.end_bpm, index, prefix, end_field,
                                   kTempoBpmDomain, kTempoRampBpmDomain);
}

/// @brief Checks the authored BPM fields in stored order.
inline void check_public_tempo_segments(const std::vector<TempoSegment>& segments,
                                        const char* prefix = "segments",
                                        const char* end_field = "end_bpm") {
  for (std::size_t i = 0; i < segments.size(); ++i) {
    check_public_tempo_segment(segments[i], i, prefix, end_field);
  }
}

/// @brief Internal tempo vectors have a lower bound but no upper bound.
inline void check_internal_tempo_segments(const std::vector<TempoSegment>& segments,
                                          const char* prefix = "segments") {
  constexpr auto ramp_domain = util::check::with_sentinel(kInternalTempoBpmDomain, 0.0);
  for (std::size_t i = 0; i < segments.size(); ++i) {
    detail::check_tempo_segment_bpms(segments[i].bpm, segments[i].end_bpm, i, prefix, "end_bpm",
                                     kInternalTempoBpmDomain, ramp_domain);
  }
}

struct TimeSignature {
  int numerator = 4;
  int denominator = 4;
};

/// The tempo of a timeline that states none: kDefaultBpm from ppq 0.
inline constexpr TempoSegment kDefaultTempoSegment{0.0, constants::kDefaultBpm, 0.0};
/// The meter of a timeline that states none.
inline constexpr TimeSignature kDefaultTimeSignature{};

/// Length of one bar of `sig` in quarter-note PPQ; numerator and denominator clamp to at least 1.
double bar_length_ppq(TimeSignature sig) noexcept;

struct TimeSignatureSegment {
  double start_ppq = 0.0;
  TimeSignature time_sig{};
  // SMF FF 58 notation-clock metadata, carried through import/export of a
  // Standard MIDI File only. These are intentionally NOT part of the realtime
  // transport API (they do not affect musical timing, only a MIDI file's
  // metronome-click notation) and are NOT serialized in the project JSON, which
  // stores only start_ppq + numerator/denominator. They round-trip through SMF,
  // not through the C-ABI time-signature struct or from_json/to_json.
  uint8_t clocks_per_metronome_click = 24;
  uint8_t thirty_seconds_per_quarter = 8;
};

inline bool valid_public_time_signature_segment(const TimeSignatureSegment& segment) noexcept {
  return std::isfinite(segment.start_ppq) && valid_public_ppq(segment.start_ppq) &&
         segment.time_sig.numerator > 0 && segment.time_sig.denominator > 0;
}

struct BarBeat {
  int64_t bar = 0;
  int beat = 1;
  double beat_fraction = 0.0;
};

/// Stable-orders segments by start_ppq; equal starts keep their order.
void sort_by_start(std::vector<TempoSegment>* segments);
void sort_by_start(std::vector<TimeSignatureSegment>* segments);

class TempoMap {
 public:
  void prepare(double sample_rate);
  /// @brief Installs a tempo map. Both endpoints must meet the transport floor;
  ///          a zero end_bpm means the segment is constant.
  void set_segments(std::vector<TempoSegment> segments);
  /// @brief Installs the meter map.
  /// @details Before the first signature the span is read in that signature: its bar grid starts
  ///          at 0, so a first signature placed after 0 leaves a pickup bar from 0 to it (counted
  ///          as a bar). An empty map is 4/4. A stored or imported map holds only the signatures
  ///          given to it and gets this reading here, never an inserted default.
  void set_time_signatures(std::vector<TimeSignatureSegment> time_signatures);

  double sample_to_ppq(int64_t sample) const noexcept;
  int64_t ppq_to_sample(double ppq) const noexcept;
  /// Seconds from ppq 0 to @p ppq, unrounded (ppq_to_sample rounds to a sample).
  double ppq_to_seconds(double ppq) const noexcept;
  /// PPQ at @p seconds from ppq 0, unrounded.
  double seconds_to_ppq(double seconds) const noexcept;
  /// Effective instantaneous tempo at @p ppq.
  /// Returns the default BPM when unprepared or @p ppq is non-finite.
  double bpm_at_ppq(double ppq) const noexcept;
  double bpm_at_sample(int64_t sample) const noexcept;
  TimeSignature time_signature_at_ppq(double ppq) const noexcept;
  /// Start of the time-signature segment active at @p ppq, where its bar grid begins.
  double time_signature_start_ppq(double ppq) const noexcept;
  BarBeat ppq_to_bar_beat(double ppq) const noexcept;
  double bar_start_ppq(double ppq) const noexcept;
  /// PPQ start of the requested zero-based bar.
  double bar_index_start_ppq(int64_t bar_index) const noexcept;

  double sample_rate() const noexcept { return sample_rate_; }

 private:
  static size_t segment_index_for_sample(const std::vector<TempoSegment>& segments,
                                         double sample) noexcept;
  static size_t segment_index_for_ppq(const std::vector<TempoSegment>& segments,
                                      double ppq) noexcept;
  static size_t time_signature_index_for_ppq(
      const std::vector<TimeSignatureSegment>& time_signatures, double ppq) noexcept;
  static double bar_start_ppq_in(const std::vector<TimeSignatureSegment>& time_signatures,
                                 double ppq) noexcept;

  double sample_rate_ = constants::kDefaultDawSampleRate;
  rt::RtSnapshot<std::vector<TempoSegment>> segments_;
  rt::RtSnapshot<std::vector<TimeSignatureSegment>> time_signatures_;
};

}  // namespace sonare::transport
