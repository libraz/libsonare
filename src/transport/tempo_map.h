#pragma once

/// @file tempo_map.h
/// @brief Piecewise tempo (constant or linearly-ramped) and time-signature map.

#include <cstdint>
#include <memory>
#include <vector>

#include "rt/rt_publisher.h"

namespace sonare::transport {

struct TempoSegment {
  double start_ppq = 0.0;
  double bpm = 120.0;
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

struct TimeSignature {
  int numerator = 4;
  int denominator = 4;
};

struct TimeSignatureSegment {
  double start_ppq = 0.0;
  TimeSignature time_sig{};
};

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

  double sample_rate_ = 48000.0;
  rt::RtSnapshot<std::vector<TempoSegment>> segments_;
  rt::RtSnapshot<std::vector<TimeSignatureSegment>> time_signatures_;
};

}  // namespace sonare::transport
