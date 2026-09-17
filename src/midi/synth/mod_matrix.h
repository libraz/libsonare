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
  /// Filter resonance offset; depth in Q units at full source.
  kResonanceQ = 5,
  /// LFO1 -> pitch depth offset; depth in cents at full source. Reaches the
  /// depth the vibrato is spent at, not the pitch, so a source at zero leaves
  /// the patch's own vibrato standing.
  kVibratoDepthCents = 6,
  /// Scales the filter envelope's contribution to cutoff; the multiplier
  /// accumulates 1 + depth * source. Modulating how far the envelope sweeps
  /// rather than where it sweeps from.
  kFilterEnvDepth = 7,
  /// Multiplies LFO1's frequency; the multiplier accumulates 1 + depth *
  /// source. Applied one sample late, LFO1 being a source as well, which is
  /// what keeps the routing acyclic.
  kLfo1RateScale = 8,
  /// Excitation strength — bow force on a bowed string, breath pressure on a
  /// wind bore. Offset in normalized axis units, the same [0,1] scale the
  /// live-control CCs drive; the engine sums it onto its own value and clamps.
  kExcitationForce = 9,
  /// Where the exciter meets the resonator — bow contact point today. Same
  /// normalized units; an engine whose exciter has no position declines it.
  kExcitationPosition = 10,
  /// Exciter-side brightness: the bore's radiating filter on a wind engine.
  /// Same normalized units. Distinct from kCutoffCents, which moves the
  /// wrapper filter downstream of the engine rather than the engine itself.
  kExcitationBrightness = 11,
  /// Position along the two spectral tables a patch carries — the drawbar
  /// organ's second registration today. Not an excitation axis: a tonewheel
  /// has no exciter to move, and what this scans is the resonator's own
  /// spectrum. Same normalized units; an engine carrying one table declines.
  kSpectrumMorph = 12,
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

  /// True when at least one live route lands on an axis an engine owns rather
  /// than the wrapper. The voice precomputes this so a matrix that only moves
  /// pitch or cutoff never reaches an engine's control setters at all.
  bool has_engine_control_route() const noexcept {
    for (const ModRoute& r : routes) {
      if (r.source == ModSource::kNone || r.depth == 0.0f) continue;
      if (r.destination == ModDestination::kExcitationForce ||
          r.destination == ModDestination::kExcitationPosition ||
          r.destination == ModDestination::kExcitationBrightness ||
          r.destination == ModDestination::kSpectrumMorph) {
        return true;
      }
    }
    return false;
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
  float resonance_q = 0.0f;
  float vibrato_depth_cents = 0.0f;
  float filter_env_depth = 1.0f;  // multiplicative, clamped to [0, 4]
  float lfo1_rate_scale = 1.0f;   // multiplicative, clamped to [1/16, 16]
  // Engine-owned axes: additive offsets on a [0,1] engine axis, so a full-span
  // offset either way is the most that can mean anything. Additive rather than
  // multiplicative because several of these axes rest at 0 (polarization, mute,
  // half-valve, the morph position), where a 1 + depth * source scale could
  // never move them.
  float excitation_force = 0.0f;       // additive, clamped to [-1, 1]
  float excitation_position = 0.0f;    // additive, clamped to [-1, 1]
  float excitation_brightness = 0.0f;  // additive, clamped to [-1, 1]
  float spectrum_morph = 0.0f;         // additive, clamped to [-1, 1]
};

/// Evaluates every active route. Allocation-free.
ModOffsets evaluate_mod_matrix(const ModMatrix& matrix, const ModSourceValues& values) noexcept;

}  // namespace sonare::midi::synth
