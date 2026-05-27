#include "engine/tempo_sync.h"

#include "transport/musical_time.h"

namespace sonare::engine {

double tempo_sync_ppq(TempoSyncValue value) noexcept {
  return transport::note_length_ppq(value.denominator, value.modifier);
}

int64_t tempo_sync_samples(TempoSyncValue value, const transport::TransportState& state) noexcept {
  return transport::ppq_duration_to_samples(tempo_sync_ppq(value), state.bpm, state.sample_rate);
}

}  // namespace sonare::engine
