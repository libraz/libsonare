#pragma once

/// @file midi_clip_envelope.h
/// @brief Pure per-destination MIDI clip gain/fade envelope evaluator.

#include <cstdint>
#include <vector>

#include "core/fade_curve.h"
#include "midi/midi_clip.h"

namespace sonare::midi {

/// Evaluates g(t): the gain a MIDI destination's rendered audio carries at
/// timeline sample `t`.
///
/// A clip in `clips` is ACTIVE at `t` when it targets `destination_id`, has
/// started (`start_sample <= t`) and has not ended (`t < start_sample +
/// length_samples`; an open-ended clip, `length_samples <= 0`, never ends).
/// Among active clips the one with the latest `start_sample` wins (ties break
/// on the higher clip id); its `gain * clip_fade_gain(...)` is returned.
///
/// With no active clip, the most recently ENDED clip (by `start_sample +
/// length_samples`, ties on the higher clip id; open-ended clips never
/// qualify) holds its end-of-clip value: 0 under an active fade-out, its own
/// `gain` otherwise. Before any clip on this destination has started, the
/// result is 1.0.
///
/// A clip's fade is evaluated over its own full `length_samples` exactly once
/// -- an internal MIDI loop (`loop_mode` / `loop_length_samples`) does not
/// retrigger it. Pure, allocation-free, RT-safe.
inline float midi_clip_envelope_gain(const std::vector<MidiClipSchedule>& clips,
                                     uint32_t destination_id, int64_t t) noexcept {
  const MidiClipSchedule* active = nullptr;
  const MidiClipSchedule* last_ended = nullptr;
  int64_t last_ended_at = 0;

  for (const MidiClipSchedule& clip : clips) {
    if (clip.destination_id != destination_id) continue;
    if (clip.start_sample > t) continue;
    const bool open_ended = clip.length_samples <= 0;
    const int64_t end_sample = clip.start_sample + clip.length_samples;
    const bool ended = !open_ended && t >= end_sample;
    if (!ended) {
      if (!active || clip.start_sample > active->start_sample ||
          (clip.start_sample == active->start_sample && clip.id > active->id)) {
        active = &clip;
      }
    } else if (!last_ended || end_sample > last_ended_at ||
               (end_sample == last_ended_at && clip.id > last_ended->id)) {
      last_ended = &clip;
      last_ended_at = end_sample;
    }
  }

  if (active != nullptr) {
    const int64_t position = t - active->start_sample;
    return active->gain * clip_fade_gain(position, active->length_samples, active->fade_in_samples,
                                         active->fade_out_samples, active->fade_in_curve,
                                         active->fade_out_curve);
  }
  if (last_ended != nullptr) {
    return last_ended->gain * clip_fade_gain(last_ended->length_samples, last_ended->length_samples,
                                             last_ended->fade_in_samples,
                                             last_ended->fade_out_samples,
                                             last_ended->fade_in_curve, last_ended->fade_out_curve);
  }
  return 1.0f;
}

}  // namespace sonare::midi
