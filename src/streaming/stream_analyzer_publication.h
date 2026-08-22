#pragma once

/// @file stream_analyzer_publication.h
/// @brief Internal SPSC publication state for StreamAnalyzer.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "streaming/stream_frame.h"

namespace sonare {

struct StreamAnalyzerStatsSlot {
  AnalyzerStats storage;
  size_t chord_progression_size = 0;
  size_t bar_chord_progression_size = 0;
  size_t voted_pattern_size = 0;
  size_t all_pattern_scores_size = 0;
};

struct StreamAnalyzerPublication {
  static constexpr unsigned kStatsSlotCount = 3;
  enum class StatsSlotState : uint8_t { kFree, kWriting, kPublished, kReading };

  // Monotonic SPSC sequences. The producer owns producer_write_sequence and
  // publishes it to output_write_sequence only after a complete frame exists.
  std::atomic<uint64_t> output_read_sequence{0};
  std::atomic<uint64_t> output_write_sequence{0};
  uint64_t producer_write_sequence = 0;
  size_t producer_dropped_frames = 0;

  // Slot ownership is acquired by CAS, so producer and consumer can never
  // touch the same non-atomic snapshot storage. With three slots there is
  // always one free slot besides the current publication and one reader pin.
  // Scalar totals mirrored out of the published snapshot. frame_count() and
  // current_time() answer from these: reading them out of a slot would mean
  // pinning it with a CAS spin and copying four progression vectors (up to
  // max_progression_entries each) to return a single number, which a UI polling
  // the position at frame rate turns into a stream of heap allocations against
  // the audio thread. Each is a self-contained scalar written where the slot it
  // mirrors is written, so relaxed ordering is enough: nothing else's
  // visibility depends on it, and a reader sees some published snapshot's value.
  std::atomic<int> published_total_frames{0};
  std::atomic<float> published_duration_seconds{0.0f};

  std::array<StreamAnalyzerStatsSlot, kStatsSlotCount> stats_slots;
  std::array<std::atomic<StatsSlotState>, kStatsSlotCount> stats_slot_states{
      StatsSlotState::kPublished, StatsSlotState::kFree, StatsSlotState::kFree};
  std::atomic<unsigned> published_stats_slot{0};
};

}  // namespace sonare
