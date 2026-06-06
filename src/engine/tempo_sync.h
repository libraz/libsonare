#pragma once

/// @file tempo_sync.h
/// @brief Transport-aware conversion for tempo-synced processors.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transport/musical_time.h"
#include "transport/transport_state.h"

namespace sonare::engine {

struct TempoSyncValue {
  int denominator = 4;
  transport::NoteModifier modifier = transport::NoteModifier::kStraight;
};

double tempo_sync_ppq(TempoSyncValue value) noexcept;
int64_t tempo_sync_samples(TempoSyncValue value, const transport::TransportState& state) noexcept;

struct TempoSyncWarpSegment {
  size_t source_offset = 0;
  size_t source_samples = 0;
  size_t target_samples = 0;
};

struct TempoSyncWarpBakeConfig {
  int sample_rate = 48000;
  int n_fft = 2048;
  int hop_length = 512;
  bool phase_lock = true;
  size_t join_crossfade_samples = 128;
};

std::vector<float> bake_tempo_sync_warp_channel(const float* source, size_t source_samples,
                                                const std::vector<TempoSyncWarpSegment>& segments,
                                                const TempoSyncWarpBakeConfig& config = {});

}  // namespace sonare::engine
