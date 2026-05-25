#pragma once

/// @file transport_state.h
/// @brief Immutable per-block transport snapshot.

#include <cstdint>

#include "transport/tempo_map.h"

namespace sonare::transport {

struct TransportState {
  bool playing = false;
  bool looping = false;
  int64_t render_frame = 0;
  int64_t sample_position = 0;
  double ppq_position = 0.0;
  double bpm = 120.0;
  double bar_start_ppq = 0.0;
  int64_t bar_count = 0;
  TimeSignature time_sig{};
  double loop_start_ppq = 0.0;
  double loop_end_ppq = 0.0;
  double sample_rate = 48000.0;
};

}  // namespace sonare::transport
