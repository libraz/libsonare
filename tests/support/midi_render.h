#pragma once

/// @file midi_render.h
/// @brief Shared MIDI event construction and offline rendering for synth tests.
///
/// Both helpers are reproduced verbatim from the per-file copies they replace,
/// so a test that switches to this header renders bit-identically. render_left
/// is a template because the copies were spelled against three different
/// concrete types (NativeSynth, Sf2Player, Instrument); each instantiation is
/// the copy it replaces.

#include <cstddef>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::test {

/// Wraps one UMP word in the MidiEvent the synths consume. render_frame stays
/// at its default: on_event does not read it, so an event lands at the head of
/// the next process() call regardless.
inline sonare::midi::MidiEvent event(const sonare::midi::Ump& ump) {
  sonare::midi::MidiEvent e;
  e.ump = ump;
  return e;
}

/// Renders @p num_samples frames in one process() call and returns the left
/// channel. The right channel is allocated because the synths write two.
template <typename Synth>
std::vector<float> render_left(Synth& synth, int num_samples) {
  std::vector<float> left(static_cast<std::size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<std::size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, num_samples);
  return left;
}

/// Both channels of a render, for callers that hash or compare the stereo pair.
struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

/// Renders @p num_samples frames in one process() call and returns both
/// channels. Same buffer setup as render_left.
template <typename Synth>
StereoRender render_stereo(Synth& synth, int num_samples) {
  StereoRender out;
  out.left.assign(static_cast<std::size_t>(num_samples), 0.0f);
  out.right.assign(static_cast<std::size_t>(num_samples), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  synth.process(chans, 2, num_samples);
  return out;
}

}  // namespace sonare::test
