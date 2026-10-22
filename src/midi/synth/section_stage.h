#pragma once
/// @file section_stage.h
/// @brief A section of players on one voice's output: six taps into the
///        host-rate output history, each at its own seat delay plus a sinusoidal
///        wander at its own vibrato rate, summed at 1/sqrt(6). Several players
///        with independent vibrati are what widen a section's partials, and the
///        stage sits outside the engine's loop so the excitation never sees the
///        pitch move. Shared by the bowed string and the vocal core.
///
/// The history is not owned here — the host attaches a span per voice before
/// start(). start()/process() are allocation-free; the seats, rates and phases
/// are drawn from the voice seed, so identical event streams render
/// bit-identically. Off (spread 0, or no history attached) the stage is skipped
/// entirely and process() is never called.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"
#include "util/constants.h"

namespace sonare::midi::synth {

class SectionStage {
 public:
  static constexpr int kPlayers = 6;

  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the stage its output history of @p capacity samples.
  void attach(float* history, int capacity) noexcept {
    history_ = history;
    capacity_ = capacity;
  }

  /// Seats the players for a note. @p spread_cents is each player's peak vibrato
  /// deviation, @p rate_hz the rate they draw their own around (within
  /// @p rate_spread either side), @p seat_ms the path difference across the
  /// desks. A spread of 0 leaves the stage off.
  void start(float spread_cents, float rate_hz, float seat_ms, float rate_spread, double host_sr,
             uint64_t seed) noexcept {
    const float spread = std::clamp(spread_cents, 0.0f, 50.0f);
    on_ = spread > 0.0f && history_ != nullptr;
    if (!on_) return;
    const float srf = static_cast<float>(host_sr);
    // A delay swing d sin(w t) moves the pitch by a peak ratio of d w, so the swing
    // for a peak of spread cents at rate w is (2^(c/1200) - 1) / w samples.
    const float ratio = std::exp2(spread / ::sonare::constants::kCentsPerOctave) - 1.0f;
    const float rate = std::clamp(rate_hz, 0.5f, 12.0f);
    VoiceRandomSequence draw(voice_hash(seed ^ 0x5EC7105EC7ull));
    for (int k = 0; k < kPlayers; ++k) {
      Player& pl = players_[k];
      const float hz = rate * (1.0f + rate_spread * draw.next_bipolar());
      pl.step = ::sonare::constants::kTwoPi * hz / srf;
      pl.depth = ratio / pl.step;
      pl.phase = ::sonare::constants::kTwoPi * draw.next_unipolar();
      const float seat = (static_cast<float>(k) + draw.next_unipolar()) / kPlayers;
      pl.seat = 1.0f + pl.depth + seat * seat_ms * 0.001f * srf;
      // The furthest reach stays inside the attached history.
      pl.seat = std::min(pl.seat, static_cast<float>(capacity_ - 2) - pl.depth);
    }
    std::fill(history_, history_ + capacity_, 0.0f);
    write_ = 0;
  }

  bool on() const noexcept { return on_; }
  void stop() noexcept { on_ = false; }

  /// Writes @p x into the history and returns the players' sum.
  float process(float x) noexcept {
    const size_t cap = static_cast<size_t>(capacity_);
    history_[write_] = x;
    float sum = 0.0f;
    for (Player& pl : players_) {
      const float d = pl.seat + pl.depth * std::sin(pl.phase);
      pl.phase += pl.step;
      if (pl.phase >= ::sonare::constants::kTwoPi) pl.phase -= ::sonare::constants::kTwoPi;
      const float di = std::floor(d);
      const float frac = d - di;
      const size_t back = static_cast<size_t>(di);
      const size_t i0 = (write_ + cap - back) % cap;
      const size_t i1 = (i0 + cap - 1) % cap;
      sum += history_[i0] + frac * (history_[i1] - history_[i0]);
    }
    write_ = (write_ + 1) % cap;
    return sum * (1.0f / std::sqrt(static_cast<float>(kPlayers)));
  }

 private:
  struct Player {
    float seat = 0.0f;   // mean delay (samples)
    float depth = 0.0f;  // delay swing (samples) giving the peak pitch deviation
    float phase = 0.0f;  // radians
    float step = 0.0f;   // radians per host sample
  };
  Player players_[kPlayers];
  bool on_ = false;
  float* history_ = nullptr;
  int capacity_ = 0;
  size_t write_ = 0;
};

}  // namespace sonare::midi::synth
