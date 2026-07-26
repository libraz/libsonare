#pragma once

/// @file tempo_map.h
/// @brief Piecewise tempo (constant or linearly-ramped) and time-signature map.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "rt/rt_publisher.h"
#include "util/constants.h"

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

inline bool valid_public_ppq(double ppq) noexcept { return ppq >= 0.0 && ppq <= kMaxPublicPpq; }

inline bool valid_public_tempo(double bpm) noexcept {
  return std::isfinite(bpm) && bpm > 0.0 && bpm <= kMaxPublicTempoBpm;
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
         valid_public_tempo(segment.bpm) && std::isfinite(segment.end_bpm) &&
         segment.end_bpm >= 0.0 && (segment.end_bpm == 0.0 || valid_public_tempo(segment.end_bpm));
}

struct TimeSignature {
  int numerator = 4;
  int denominator = 4;
};

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

class TempoMap {
 public:
  void prepare(double sample_rate);
  void set_segments(std::vector<TempoSegment> segments);
  void set_time_signatures(std::vector<TimeSignatureSegment> time_signatures);

  double sample_to_ppq(int64_t sample) const noexcept;
  int64_t ppq_to_sample(double ppq) const noexcept;
  double bpm_at_sample(int64_t sample) const noexcept;
  TimeSignature time_signature_at_ppq(double ppq) const noexcept;
  BarBeat ppq_to_bar_beat(double ppq) const noexcept;
  double bar_start_ppq(double ppq) const noexcept;

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
