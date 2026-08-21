#include "mixing/alignment_delay.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "rt/fractional_delay.h"

namespace sonare::mixing {

namespace {

int clamp_integer_delay(int delay_samples) noexcept {
  return std::min(std::max(0, delay_samples), kMaxAlignmentDelaySamples);
}

}  // namespace

AlignmentDelay::AlignmentDelay(int delay_samples)
    : delay_samples_(clamp_integer_delay(delay_samples)),
      delay_samples_q8_(clamp_integer_delay(delay_samples) << 8) {}

void AlignmentDelay::prepare(double, int) {
  prepared_channels_ = std::max(prepared_channels_, 2);
  prepare_storage();
}

void AlignmentDelay::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  // A zero delay is a pass-through by definition: every plane would be written
  // back with the value it already holds. Returning early keeps a bank that
  // rests at zero (every PDC bank in a project with no latent insert) free
  // rather than merely bit-identical, and it keeps the overflow high-water mark
  // meaningful -- a zero-delay bank cannot misalign anything.
  if (delay_samples_q8_ == 0) {
    return;
  }
  note_channel_overflow(num_channels);
  // Do not allocate on the audio thread: process only the channels prepared
  // for (clamp to the prepared count). prepare() preallocates the maximum.
  const int channels_to_process = std::min(num_channels, prepared_channels_);
  for (int ch = 0; ch < channels_to_process; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    if (fractional_mode_ == FractionalDelayMode::None || (delay_samples_q8_ & 0xff) == 0) {
      rt::DelayLine& delay = delays_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] = delay.process(channels[ch][i]);
      }
    } else {
      FractionalState& state = fractional_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] = process_fractional(state, channels[ch][i]);
      }
    }
  }
}

void AlignmentDelay::prime(const float* const* channels, int num_channels,
                           int num_samples) noexcept {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  if (delay_samples_q8_ == 0) {
    return;
  }
  note_channel_overflow(num_channels);
  // Mirrors process() rather than sharing a per-sample helper on purpose: the
  // fractional-mode test stays hoisted out of the inner loop in both paths.
  const int channels_to_process = std::min(num_channels, prepared_channels_);
  for (int ch = 0; ch < channels_to_process; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    if (fractional_mode_ == FractionalDelayMode::None || (delay_samples_q8_ & 0xff) == 0) {
      rt::DelayLine& delay = delays_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        delay.process(channels[ch][i]);
      }
    } else {
      FractionalState& state = fractional_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        process_fractional(state, channels[ch][i]);
      }
    }
  }
}

void AlignmentDelay::reset() {
  for (rt::DelayLine& delay : delays_) {
    delay.reset();
  }
  for (FractionalState& state : fractional_) {
    std::fill(state.buffer.begin(), state.buffer.end(), 0.0f);
    state.write_index = 0;
  }
}

void AlignmentDelay::set_prepared_channels(int num_channels) {
  const int next = std::max(1, num_channels);
  if (next == prepared_channels_) {
    return;
  }
  const int previous = prepared_channels_;
  prepared_channels_ = next;
  // Before prepare() this is only a declaration of the width to come, so no
  // storage exists to resize. Afterwards it is a real widening and has to be
  // honoured here: with the no-op guard in place a later same-value delay
  // change would not reallocate, and the bank would stay short of the width it
  // was just told about.
  if (delays_.empty()) {
    return;
  }
  try {
    prepare_storage();
  } catch (...) {
    prepared_channels_ = previous;
    throw;
  }
}

void AlignmentDelay::set_delay_samples(int delay_samples) {
  delay_samples_ = clamp_integer_delay(delay_samples);
  delay_samples_q8_ = delay_samples_ << 8;
  fractional_mode_ = FractionalDelayMode::None;
  prepare_storage();
}

void AlignmentDelay::set_delay_samples_q8(int delay_samples_q8, FractionalDelayMode mode) {
  delay_samples_q8_ = std::min(std::max(0, delay_samples_q8), kMaxAlignmentDelaySamples << 8);
  delay_samples_ = delay_samples_q8_ >> 8;
  fractional_mode_ = (delay_samples_q8_ & 0xff) == 0 ? FractionalDelayMode::None : mode;
  prepare_storage();
}

bool AlignmentDelay::try_set_delay_samples_q8(int delay_samples_q8,
                                              FractionalDelayMode mode) noexcept {
  // prepare_storage() commits by swapping fully-built replacements in, so a
  // throw leaves the storage untouched and restoring these three scalars
  // restores the whole object.
  const int previous_q8 = delay_samples_q8_;
  const int previous_samples = delay_samples_;
  const FractionalDelayMode previous_mode = fractional_mode_;
  try {
    set_delay_samples_q8(delay_samples_q8, mode);
    return true;
  } catch (...) {
    delay_samples_q8_ = previous_q8;
    delay_samples_ = previous_samples;
    fractional_mode_ = previous_mode;
    return false;
  }
}

AlignmentDelay::StorageSpec AlignmentDelay::required_storage() const noexcept {
  StorageSpec spec;
  spec.channels = prepared_channels_;
  spec.integer_delay = static_cast<size_t>(delay_samples_);
  if (fractional_mode_ == FractionalDelayMode::None || (delay_samples_q8_ & 0xff) == 0) {
    return spec;
  }
  const int integer_delay = delay_samples_q8_ >> 8;
  spec.fractional_size = static_cast<size_t>(std::max(8, integer_delay + 8));
  return spec;
}

void AlignmentDelay::prepare_storage() {
  if (prepared_channels_ <= 0) {
    return;
  }

  const StorageSpec want = required_storage();
  // The single point every mutator funnels through. When the storage a
  // configuration needs is the shape already held, the delay lines and their
  // history are left exactly as they are -- so re-applying an unchanged
  // alignment (which the mixer does on every unrelated strip edit) costs
  // nothing and drops no audio. A caller cannot opt out of this.
  const size_t want_fractional_rows =
      want.fractional_size == 0 ? 0u : static_cast<size_t>(want.channels);
  if (want == storage_ && delays_.size() == static_cast<size_t>(want.channels) &&
      fractional_.size() == want_fractional_rows) {
    return;
  }

  // Build the replacements first, commit by swapping: a caller that catches the
  // allocation failure finds the bank exactly as it was.
  std::vector<rt::DelayLine> next_delays(static_cast<size_t>(want.channels));
  for (rt::DelayLine& delay : next_delays) {
    delay.prepare(want.integer_delay);
  }
  std::vector<FractionalState> next_fractional;
  if (want.fractional_size != 0) {
    next_fractional.assign(static_cast<size_t>(want.channels), FractionalState{});
    for (FractionalState& state : next_fractional) {
      state.buffer.assign(want.fractional_size, 0.0f);
      state.write_index = 0;
    }
  }

  delays_.swap(next_delays);
  fractional_.swap(next_fractional);
  storage_ = want;
  ++storage_generation_;
}

void AlignmentDelay::note_channel_overflow(int num_channels) noexcept {
  channel_overflow_high_water_.raise_to(num_channels - prepared_channels_);
}

float AlignmentDelay::process_fractional(FractionalState& state, float input) const noexcept {
  return rt::lagrange3_fractional_delay(state.buffer, state.write_index, delay_samples_q8_, input);
}

}  // namespace sonare::mixing
