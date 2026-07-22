#pragma once

/// @file transport_state.h
/// @brief Immutable per-block transport snapshot.

#include <cstdint>

#include "transport/tempo_map.h"
#include "util/constants.h"

namespace sonare::transport {

struct TransportState {
  bool playing = false;
  bool looping = false;
  int64_t render_frame = 0;
  int64_t sample_position = 0;
  double ppq_position = 0.0;
  double bpm = constants::kDefaultBpm;
  double bar_start_ppq = 0.0;
  int64_t bar_count = 0;
  TimeSignature time_sig{};
  double loop_start_ppq = 0.0;
  double loop_end_ppq = 0.0;
  double sample_rate = constants::kDefaultDawSampleRate;
  // Musical beat within the current bar. `beat` is one-based (matching the
  // BarBeat convention) while `bar_count` above is zero-based; `beat_fraction`
  // is the fractional position within the current beat, in [0, 1).
  int beat = 1;
  double beat_fraction = 0.0;
};

}  // namespace sonare::transport
