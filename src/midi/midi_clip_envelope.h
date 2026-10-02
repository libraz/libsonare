#pragma once

/// @file midi_clip_envelope.h
/// @brief Pure per-destination MIDI clip gain/fade envelope evaluator.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/fade_curve.h"
#include "midi/midi_clip.h"
#include "util/numeric_validation.h"

namespace sonare::midi {

/// The clips deciding g(t) at one instant: the latest-started active clip, and
/// the most recently ended one that holds its end value while none is active.
struct MidiClipEnvelopeWinner {
  const MidiClipSchedule* active = nullptr;
  const MidiClipSchedule* last_ended = nullptr;
};

/// Picks the winner at timeline sample `t` for `destination_id`. A clip is
/// active once started and until `start_sample + length_samples` (open-ended
/// when `length_samples <= 0`); ties break on the higher clip id.
inline MidiClipEnvelopeWinner find_midi_clip_envelope_winner(
    const std::vector<MidiClipSchedule>& clips, uint32_t destination_id, int64_t t) noexcept {
  MidiClipEnvelopeWinner winner;
  int64_t last_ended_at = 0;
  for (const MidiClipSchedule& clip : clips) {
    if (clip.destination_id != destination_id || clip.start_sample > t) continue;
    const int64_t end_sample = numeric::saturating_add(clip.start_sample, clip.length_samples);
    const bool ended = clip.length_samples > 0 && t >= end_sample;
    if (!ended) {
      const MidiClipSchedule* active = winner.active;
      if (!active || clip.start_sample > active->start_sample ||
          (clip.start_sample == active->start_sample && clip.id > active->id)) {
        winner.active = &clip;
      }
    } else if (!winner.last_ended || end_sample > last_ended_at ||
               (end_sample == last_ended_at && clip.id > winner.last_ended->id)) {
      winner.last_ended = &clip;
      last_ended_at = end_sample;
    }
  }
  return winner;
}

/// `clip`'s gain times its fade at `position` samples into the clip.
inline float midi_clip_gain_at(const MidiClipSchedule& clip, int64_t position) noexcept {
  return clip.gain * clip_fade_gain(position, clip.length_samples, clip.fade_in_samples,
                                    clip.fade_out_samples, clip.fade_in_curve, clip.fade_out_curve);
}

/// Evaluates g(t): the gain a MIDI destination's rendered audio carries at
/// timeline sample `t`. The active winner contributes its gain and fade; with
/// none active the last ended clip holds its end value (0 after a fade-out);
/// before any clip has started g is 1. A clip's fade spans its full length
/// once -- an internal MIDI loop does not retrigger it. Pure, RT-safe.
inline float midi_clip_envelope_gain(const std::vector<MidiClipSchedule>& clips,
                                     uint32_t destination_id, int64_t t) noexcept {
  const MidiClipEnvelopeWinner winner = find_midi_clip_envelope_winner(clips, destination_id, t);
  if (winner.active != nullptr) {
    return midi_clip_gain_at(*winner.active,
                             numeric::saturating_sub(t, winner.active->start_sample));
  }
  if (winner.last_ended != nullptr) {
    return midi_clip_gain_at(*winner.last_ended, winner.last_ended->length_samples);
  }
  return 1.0f;
}

/// A run of samples over which g(t) follows one clip's fade (`clip` set) or
/// holds `constant_gain` (`clip == nullptr`).
struct MidiClipEnvelopeRun {
  int64_t frames = 0;
  const MidiClipSchedule* clip = nullptr;
  float constant_gain = 1.0f;
};

/// g(t) over one block, split into runs at clip starts and ends, the only
/// points where the winner can change. `overflowed` means the block had more
/// boundaries than fit and apply_midi_clip_envelope falls back to per-sample.
struct MidiClipEnvelopeBlock {
  static constexpr size_t kMaxRuns = 64;
  std::array<MidiClipEnvelopeRun, kMaxRuns> runs{};
  size_t run_count = 0;
  bool overflowed = false;
};

/// AUDIO thread: resolves g(t) for `destination_id` over
/// [block_start, block_start + length_samples) into `out`, once per
/// destination per block. RT-safe, no allocation.
inline void resolve_midi_clip_envelope(const std::vector<MidiClipSchedule>& clips,
                                       uint32_t destination_id, int64_t block_start,
                                       int64_t length_samples,
                                       MidiClipEnvelopeBlock* out) noexcept {
  out->run_count = 0;
  out->overflowed = false;
  if (length_samples <= 0) return;
  int64_t block_end = 0;
  const bool block_end_overflowed = !numeric::checked_add(block_start, length_samples, &block_end);
  if (block_end_overflowed) block_end = std::numeric_limits<int64_t>::max();

  // Boundaries strictly inside the block, sorted; one fewer than kMaxRuns.
  std::array<int64_t, MidiClipEnvelopeBlock::kMaxRuns - 1> breakpoints{};
  size_t breakpoint_count = 0;
  bool breakpoint_overflow = false;
  const auto add_breakpoint = [&](int64_t t) noexcept {
    if (t <= block_start || t >= block_end) return;
    for (size_t i = 0; i < breakpoint_count; ++i) {
      if (breakpoints[i] == t) return;
    }
    if (breakpoint_count >= breakpoints.size()) {
      breakpoint_overflow = true;
      return;
    }
    size_t pos = breakpoint_count;
    while (pos > 0 && breakpoints[pos - 1] > t) {
      breakpoints[pos] = breakpoints[pos - 1];
      --pos;
    }
    breakpoints[pos] = t;
    ++breakpoint_count;
  };
  for (const MidiClipSchedule& clip : clips) {
    if (clip.destination_id != destination_id) continue;
    add_breakpoint(clip.start_sample);
    if (clip.length_samples > 0) {
      add_breakpoint(numeric::saturating_add(clip.start_sample, clip.length_samples));
    }
  }
  if (breakpoint_overflow) {
    out->overflowed = true;
    return;
  }

  const auto append_run = [&](int64_t frames, int64_t sample) noexcept {
    if (frames <= 0) return true;
    if (out->run_count >= MidiClipEnvelopeBlock::kMaxRuns) {
      out->overflowed = true;
      return false;
    }
    MidiClipEnvelopeRun& run = out->runs[out->run_count++];
    run = MidiClipEnvelopeRun{};
    run.frames = frames;
    const MidiClipEnvelopeWinner winner =
        find_midi_clip_envelope_winner(clips, destination_id, sample);
    if (winner.active != nullptr) {
      run.clip = winner.active;
    } else if (winner.last_ended != nullptr) {
      run.constant_gain = midi_clip_gain_at(*winner.last_ended, winner.last_ended->length_samples);
    }
    return true;
  };

  int64_t segment_start = block_start;
  for (size_t i = 0; i <= breakpoint_count; ++i) {
    const int64_t segment_end = i < breakpoint_count ? breakpoints[i] : block_end;
    if (!append_run(segment_end - segment_start, segment_start)) return;
    segment_start = segment_end;
  }

  // A saturated block end: the remaining frames all evaluate at INT64_MAX.
  if (block_end_overflowed) {
    const int64_t represented = numeric::saturating_sub(segment_start, block_start);
    const int64_t remainder = numeric::saturating_sub(length_samples, represented);
    if (!append_run(remainder, block_end)) return;
  }
}

/// AUDIO thread: multiplies `channel_count` buffers of `num_frames` samples
/// (nullptr entries skipped) by g(t). `block` must come from
/// resolve_midi_clip_envelope for the same clips, destination and range; one
/// resolved block may be applied to several buffers. RT-safe, no allocation.
inline void apply_midi_clip_envelope(const MidiClipEnvelopeBlock& block,
                                     const std::vector<MidiClipSchedule>& clips,
                                     uint32_t destination_id, int64_t block_start,
                                     float* const* channels, int channel_count,
                                     int num_frames) noexcept {
  if (channels == nullptr || channel_count <= 0 || num_frames <= 0) return;
  const auto scale = [&](int64_t i, float gain) noexcept {
    if (gain == 1.0f) return;
    for (int ch = 0; ch < channel_count; ++ch) {
      if (channels[ch] != nullptr) channels[ch][i] *= gain;
    }
  };
  if (block.overflowed) {
    for (int i = 0; i < num_frames; ++i) {
      scale(i,
            midi_clip_envelope_gain(clips, destination_id,
                                    numeric::saturating_add(block_start, static_cast<int64_t>(i))));
    }
    return;
  }
  int64_t offset = 0;
  for (size_t r = 0; r < block.run_count; ++r) {
    const MidiClipEnvelopeRun& run = block.runs[r];
    if (run.clip == nullptr && run.constant_gain == 1.0f) {
      offset += run.frames;
      continue;
    }
    const int64_t run_end = numeric::saturating_add(offset, run.frames);
    for (int64_t i = offset; i < run_end; ++i) {
      const int64_t sample = numeric::saturating_add(block_start, i);
      scale(i, run.clip != nullptr
                   ? midi_clip_gain_at(*run.clip,
                                       numeric::saturating_sub(sample, run.clip->start_sample))
                   : run.constant_gain);
    }
    offset = run_end;
  }
}

}  // namespace sonare::midi
