#pragma once

/// @file midi_sequence_render.h
/// @brief Sample-accurate offline rendering of a timed MIDI sequence.
///
/// NativeSynth applies an event at the head of the next process() call, so the
/// render is split at every event position and each event lands on the sample
/// it names. Blocks between events are at most @c max_block long.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/midi_render.h"

namespace sonare::test {

/// One event of a sequence, placed at a sample position from the render start.
struct TimedUmp {
  int64_t sample = 0;
  sonare::midi::Ump ump{};
};

/// Renders @p events through a fresh NativeSynth and returns the mono mix
/// (the mean of left and right), @p num_samples long. Events at or past
/// @p num_samples are not delivered; events sharing a position keep their order.
inline std::vector<float> render_sequence_mono(
    std::vector<TimedUmp> events, int sample_rate, int num_samples,
    const sonare::midi::synth::NativeSynthConfig& config = {}, int max_block = 256) {
  std::stable_sort(events.begin(), events.end(),
                   [](const TimedUmp& a, const TimedUmp& b) { return a.sample < b.sample; });

  sonare::midi::synth::NativeSynth synth(config);
  synth.prepare(sample_rate, max_block);

  std::vector<float> mono(static_cast<std::size_t>(num_samples), 0.0f);
  std::vector<float> left(static_cast<std::size_t>(max_block), 0.0f);
  std::vector<float> right(static_cast<std::size_t>(max_block), 0.0f);
  std::size_t next = 0;
  int64_t at = 0;
  while (at < num_samples) {
    while (next < events.size() && events[next].sample <= at) {
      synth.on_event(0, event(events[next].ump));
      ++next;
    }
    int64_t end = std::min<int64_t>(at + max_block, num_samples);
    if (next < events.size()) end = std::min(end, events[next].sample);
    const int n = static_cast<int>(end - at);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    float* channels[2] = {left.data(), right.data()};
    synth.process(channels, 2, n);
    for (int i = 0; i < n; ++i) {
      mono[static_cast<std::size_t>(at + i)] =
          0.5f * (left[static_cast<std::size_t>(i)] + right[static_cast<std::size_t>(i)]);
    }
    at = end;
  }
  return mono;
}

}  // namespace sonare::test
