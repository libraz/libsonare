#pragma once
/// @file section_stage.h
/// @brief A section of players on one voice's output: six taps into the
///        host-rate output history, each at its own seat delay plus a sinusoidal
///        wander at its own vibrato rate, summed at 1/sqrt(6). Several players
///        with independent vibrati are what widen a section's partials, and the
///        stage sits outside the engine's loop so the excitation never sees the
///        pitch move. Shared by the bowed string and the vocal core.
///
/// A unison is never in tune: a second, much slower swing per tap holds each
/// player a few cents off the others, apart from their vibrato, and the sum beats.
///
/// The history is not owned here — the host attaches a span per voice before
/// start(). start()/process() are allocation-free; the seats, rates and phases
/// are drawn from the voice seed, so identical event streams render
/// bit-identically. Off (spread and detune 0, or no history attached) the stage
/// is skipped and process() is never called; detune 0 leaves a spread unchanged.

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
  /// Widest per-player detune (cents, peak) and the drift rate each player draws
  /// theirs around, within kDriftRateSpread either side: seconds-long wander.
  static constexpr float kMaxDetuneCents = 20.0f;
  static constexpr float kDriftHz = 0.15f;
  static constexpr float kDriftRateSpread = 0.5f;
  /// History the drift adds (s): both swings at the widest detune and slowest
  /// drift, 2 (2^(20/1200) - 1) / (2 pi 0.075 Hz) = 49 ms.
  static constexpr double kDriftReachS = 0.05;

  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the stage its output history of @p capacity samples.
  void attach(float* history, int capacity) noexcept {
    history_ = history;
    capacity_ = capacity;
  }

  /// Seats the players for a note. @p spread_cents is each player's peak vibrato
  /// deviation, @p rate_hz the rate they draw their own around (within
  /// @p rate_spread either side), @p seat_ms the path difference across the
  /// desks, @p detune_cents each player's own slow pitch offset (peak). A spread
  /// and a detune of 0 leave the stage off.
  void start(float spread_cents, float rate_hz, float seat_ms, float rate_spread, double host_sr,
             uint64_t seed, float detune_cents = 0.0f) noexcept {
    const float spread = std::clamp(spread_cents, 0.0f, 50.0f);
    const float detune = std::clamp(detune_cents, 0.0f, kMaxDetuneCents);
    on_ = (spread > 0.0f || detune > 0.0f) && history_ != nullptr;
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
    // Own stream, so detune 0 keeps the seats; antiphase pairs hold the section's centre on pitch.
    drift_ = detune > 0.0f;
    if (drift_) {
      const float detune_ratio = std::exp2(detune / ::sonare::constants::kCentsPerOctave) - 1.0f;
      VoiceRandomSequence wander(voice_hash(seed ^ 0xD71F7D71F7ull));
      constexpr int kPairs = kPlayers / 2;
      for (int k = 0; k < kPlayers; ++k) {
        Player& pl = players_[k];
        if (k < kPairs) {
          const float hz = kDriftHz * (1.0f + kDriftRateSpread * wander.next_bipolar());
          pl.drift_step = ::sonare::constants::kTwoPi * hz / srf;
          pl.drift_depth = detune_ratio / pl.drift_step;
          pl.drift_phase = ::sonare::constants::kTwoPi * wander.next_unipolar();
        } else {
          const Player& partner = players_[k - kPairs];
          pl.drift_step = partner.drift_step;
          pl.drift_depth = partner.drift_depth;
          pl.drift_phase = partner.drift_phase + ::sonare::constants::kPi;
          if (pl.drift_phase >= ::sonare::constants::kTwoPi) {
            pl.drift_phase -= ::sonare::constants::kTwoPi;
          }
        }
        pl.seat = std::min(pl.seat + pl.drift_depth,
                           static_cast<float>(capacity_ - 2) - pl.depth - pl.drift_depth);
      }
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
      float d = pl.seat + pl.depth * std::sin(pl.phase);
      pl.phase += pl.step;
      if (pl.phase >= ::sonare::constants::kTwoPi) pl.phase -= ::sonare::constants::kTwoPi;
      if (drift_) {
        d += pl.drift_depth * std::sin(pl.drift_phase);
        pl.drift_phase += pl.drift_step;
        if (pl.drift_phase >= ::sonare::constants::kTwoPi) {
          pl.drift_phase -= ::sonare::constants::kTwoPi;
        }
      }
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
    float seat = 0.0f;         // mean delay (samples)
    float depth = 0.0f;        // delay swing (samples) giving the peak pitch deviation
    float phase = 0.0f;        // radians
    float step = 0.0f;         // radians per host sample
    float drift_depth = 0.0f;  // delay swing (samples) giving the peak detune
    float drift_phase = 0.0f;  // radians
    float drift_step = 0.0f;   // radians per host sample
  };
  Player players_[kPlayers];
  bool on_ = false;
  bool drift_ = false;
  float* history_ = nullptr;
  int capacity_ = 0;
  size_t write_ = 0;
};

}  // namespace sonare::midi::synth
