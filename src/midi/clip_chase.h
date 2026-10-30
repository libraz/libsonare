#pragma once

/// @file clip_chase.h
/// @brief Chase keys and the per-clip chase index the sequencer seeks with.

#include <cstdint>
#include <limits>
#include <memory>

#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "util/numeric_validation.h"

namespace sonare::midi::detail {

// Chase keys: (group, channel) above kChaseKindBits, the state kind below it.
constexpr uint32_t kChaseKindBits = 18;
constexpr uint32_t kChaseKindMask = (1u << kChaseKindBits) - 1u;
// Resets are not states: they only retire the older states they cover. Both flag bits set
// never occurs in a registered or assignable key, so these cannot collide with one.
constexpr uint32_t kChaseKindReset = 0x30000;
constexpr uint32_t kChaseKindSystemReset = 0x30001;
constexpr int kChasePasses = 4;

inline int64_t saturating_clip_end(const MidiClipSchedule& clip) noexcept {
  if (clip.length_samples <= 0) return std::numeric_limits<int64_t>::max();
  return numeric::saturating_add(clip.start_sample, clip.length_samples);
}

/// Offset of @p frame into its loop iteration; exact for any frame >= clip.start_sample.
inline int64_t loop_phase(const MidiClipSchedule& clip, int64_t frame) noexcept {
  const uint64_t elapsed = static_cast<uint64_t>(frame) - static_cast<uint64_t>(clip.start_sample);
  return static_cast<int64_t>(elapsed % static_cast<uint64_t>(clip.loop_length_samples));
}

bool is_reset_key(uint32_t key) noexcept;
bool reset_covers(uint32_t key) noexcept;
int chase_pass(uint32_t key) noexcept;
std::shared_ptr<const MidiClipChaseIndex> build_chase_index(const MidiClipSchedule& clip);
const MidiEvent* chased_event(const MidiClipSchedule& clip, const uint32_t* first,
                              const uint32_t* last, int64_t at, int64_t* frame) noexcept;

}  // namespace sonare::midi::detail
