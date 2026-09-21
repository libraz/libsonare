#pragma once

/// @file wind_bore.h
/// @brief One acoustic-bore delay line: a fractional-delay line seeded with a
///        short noise burst so the coupled exciter has an f0 component to
///        lock onto rather than swelling up from silence.
///
/// Shared by the engines whose exciter drives a single bore column (brass,
/// reed, flute). What it deliberately does NOT own is anything that happens
/// *inside* the loop for one instrument only — the phase lead/lag folded into
/// `comp`, an in-loop DC blocker, a tonehole tap — the same split
/// string_loop.h states at its own top (string_loop.h:14-18). Those stay at
/// the call site; this header is the delay line and its seed alone.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"
#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

/// One bore delay line: a circular buffer read at a fractional offset, closed
/// by the caller through whatever loss or reflection filter the instrument
/// owns.
struct WindBore {
  float* buffer = nullptr;
  int capacity = 0;
  size_t write = 0;
  int prefill_span = 0;
  float period = 0.0f;
  float comp = 0.0f;
  float out = 0.0f;

  /// Sets the line up for a note. @p slab is the voice's own delay-line span
  /// (not owned here); @p period_samples the ideal loop period at ratio == 1;
  /// @p comp_samples the loop delay not in the line itself (the feedback
  /// register plus whatever phase lead/lag the caller assembled); @p
  /// span_factor how far past the period the seed reaches — an engine-owned
  /// choice, never a constant this header may default.
  void configure(float* slab, int capacity_samples, float period_samples, float comp_samples,
                 float span_factor) noexcept {
    buffer = slab;
    capacity = capacity_samples;
    period = period_samples;
    comp = comp_samples;
    out = 0.0f;
    const float eff = std::max(2.0f, period - comp);
    const int span = static_cast<int>(eff * span_factor) + 8;
    prefill_span = std::min(capacity, std::max(16, span));
    write = capacity > 0 ? static_cast<size_t>(prefill_span % capacity) : 0;
  }

  /// Seeds the line's history with a burst of @p level scaled noise from
  /// @p rng, and clears the rest of the span beyond the seed — a slab slot the
  /// previous note wrote, read before this one writes it.
  void seed(float level, VoiceRandomSequence& rng) noexcept {
    if (buffer == nullptr) return;
    for (int i = prefill_span; i < capacity; ++i) buffer[static_cast<size_t>(i)] = 0.0f;
    for (int i = 0; i < prefill_span; ++i) {
      buffer[static_cast<size_t>(i)] = level * rng.bipolar_at(static_cast<uint64_t>(i));
    }
  }

  /// Writes @p input into the line and reads back the delayed sample at the
  /// current period, @p ratio (the per-sample pitch factor) and comp — the
  /// loop's own reflected pressure.
  float advance(float input, float ratio) noexcept {
    const float delay = std::clamp(period / ratio - comp, 1.0f, static_cast<float>(capacity - 4));
    out = rt::lagrange3_fractional_delay(buffer, static_cast<size_t>(capacity), write,
                                         static_cast<int>(delay * 256.0f), input);
    return out;
  }
};

}  // namespace sonare::midi::synth
