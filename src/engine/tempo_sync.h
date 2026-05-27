#pragma once

/// @file tempo_sync.h
/// @brief Transport-aware conversion for tempo-synced processors.

#include <cstdint>

#include "transport/musical_time.h"
#include "transport/transport_state.h"

namespace sonare::engine {

struct TempoSyncValue {
  int denominator = 4;
  transport::NoteModifier modifier = transport::NoteModifier::kStraight;
};

double tempo_sync_ppq(TempoSyncValue value) noexcept;
int64_t tempo_sync_samples(TempoSyncValue value, const transport::TransportState& state) noexcept;

}  // namespace sonare::engine
