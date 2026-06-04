#pragma once

/// @file mod_matrix.h
/// @brief Small fixed-size modulation routing table for the NativeSynth
///        voice: source (envelopes / LFOs / velocity / key tracking / mod
///        wheel / seeded per-voice random) -> destination (pitch / cutoff /
///        amplitude / pan), each route scaled by a depth in destination
///        units. The hardwired patch modulations (filter envelope -> cutoff,
///        LFO1 -> vibrato, velocity -> brightness) stay as dedicated patch
///        fields; the matrix adds the free-form routings on top.
///
/// RT contract: evaluate() is allocation-free pure arithmetic, called once
/// per voice per sample. Determinism: kRandom is the voice's seeded constant
/// (no RNG).

#include <array>

namespace sonare::midi::synth {

enum class ModSource : int {
  kNone = 0,
  kAmpEnv = 1,     // amplitude envelope level, [0,1]
  kFilterEnv = 2,  // filter envelope level, [0,1]
  kLfo1 = 3,       // bipolar [-1,1] (the vibrato LFO)
  kLfo2 = 4,       // bipolar [-1,1]
  kVelocity = 5,   // note-on velocity, [0,1]
  kKeyTrack = 6,   // (note - 60)/12 octaves, bipolar
  kModWheel = 7,   // CC1, [0,1]
  kRandom = 8,     // per-voice seeded constant, bipolar [-1,1]
};

enum class ModDestination : int {
  kNone = 0,
  /// Pitch offset; depth in cents at full source.
  kPitchCents = 1,
  /// Filter cutoff offset; depth in cents at full source.
  kCutoffCents = 2,
  /// Amplitude; the gain multiplier accumulates 1 + depth * source.
  kAmpGain = 3,
  /// Stereo pan offset; depth in SF2 pan units (-500..500) at full source.
  kPanUnits = 4,
};

struct ModRoute {
  ModSource source = ModSource::kNone;
  ModDestination destination = ModDestination::kNone;
  /// Destination units at full source deflection.
  float depth = 0.0f;
};

inline constexpr int kMaxModRoutes = 8;

/// The patch's routing table (unused slots stay kNone).
struct ModMatrix {
  std::array<ModRoute, kMaxModRoutes> routes{};

  bool empty() const noexcept {
    for (const ModRoute& r : routes) {
      if (r.source != ModSource::kNone && r.destination != ModDestination::kNone &&
          r.depth != 0.0f) {
        return false;
      }
    }
    return true;
  }
};

/// Per-sample source snapshot (the voice fills this in).
struct ModSourceValues {
  float amp_env = 0.0f;
  float filter_env = 0.0f;
  float lfo1 = 0.0f;
  float lfo2 = 0.0f;
  float velocity = 0.0f;
  float key_track = 0.0f;
  float mod_wheel = 0.0f;
  float random = 0.0f;
};

/// Accumulated destination offsets for one sample.
struct ModOffsets {
  float pitch_cents = 0.0f;
  float cutoff_cents = 0.0f;
  float amp_gain = 1.0f;  // multiplicative, clamped to [0, 4]
  float pan_units = 0.0f;
};

/// Evaluates every active route. Allocation-free.
ModOffsets evaluate_mod_matrix(const ModMatrix& matrix, const ModSourceValues& values) noexcept;

}  // namespace sonare::midi::synth
