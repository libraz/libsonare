#pragma once

/// @file cab_voicing.h
/// @brief Cabinet and microphone voicing: the fixed EQ centres of the amp-sim's
///        cab stage, and the biquad design that turns them into a close-miked
///        response.
///
/// This is the single source of truth for what a cabinet sounds like in this
/// library. Two consumers share it, and they must agree: the realtime analytic
/// cab chain inside `AmpSim`, and the offline cabinet-IR generator in
/// `cab_ir.h`. A second copy of these tables would be a hand-maintained mirror
/// of exactly the kind that drifts silently, so the generator does not restate
/// the voicing — it convolves this design with the geometry the analytic chain
/// cannot express.

#include <vector>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::mastering::saturation {

/// Cabinet voicing model. Selects the fixed EQ centres of the cab-EQ stage
/// (`cab == true`); has no effect when the cab EQ is bypassed.
enum class CabModel {
  /// Guitar 4x12, close-mic: 75 Hz cut, 110 Hz body bump, 3.8 kHz presence,
  /// 4.8 kHz roll-off (the original AmpSim voicing).
  kGuitar4x12 = 0,
  /// Bass 8x10 (SVT-style): extends lower (40 Hz cut, 80 Hz body bump), a
  /// darker, presence-restrained top (2.2 kHz presence, 3.5 kHz roll-off) so a
  /// bass NativeSynth voice sits in a big-cab response rather than a guitar's.
  kBass8x10 = 1,
};

/// Microphone capsule voicing in front of the cabinet. The cab EQ already
/// approximates a close-miked response, so `kNone` — no explicit mic stage —
/// is the default and leaves the chain bit-identical to the original. Selecting
/// a capsule is also what makes `mic_axis` / `mic_distance_cm` do anything.
enum class MicModel {
  /// No explicit mic stage: the cab voicing's own baked-in close-mic response.
  kNone = 0,
  /// Dynamic cardioid: a pronounced upper-mid presence peak and a firm low end
  /// — the standard close-mic on a guitar cab.
  kDynamic = 1,
  /// Ribbon: dark and smooth, with the top rolling off early and a strong
  /// proximity lift (the figure-8 pattern's pronounced close-range bass).
  kRibbon = 2,
  /// Condenser: the most extended top and the flattest low end — an airier,
  /// more detailed capture that also hears more of the cab's edge.
  kCondenser = 3,
};

/// Largest mic distance the delay line is sized for (cm). Requests beyond this
/// clamp, so a "room mic" belongs downstream in a reverb insert rather than
/// here — this stage models the mic on the cab, not the room around it.
inline constexpr float kMaxMicDistanceCm = 100.0f;

/// Reference close-mic distance (cm): the distance at which a capsule's
/// proximity lift is quoted, and the point below which it stops growing.
inline constexpr float kMicReferenceDistanceCm = 2.5f;

/// Speed of sound in cm/s, for turning a mic distance into a path-length delay.
inline constexpr float kSoundSpeedCmPerS = 100.0f * constants::kSoundSpeedMps;

/// Top-end loss per doubling of mic distance (dB), and its floor. Air
/// absorption plus the cab's own directivity: back the mic off and the top goes
/// first.
inline constexpr float kDistanceHfLossPerDoubleDb = -1.5f;
inline constexpr float kDistanceHfLossFloorDb = -9.0f;

/// How far off-axis pulls the cab's top-end roll-off down, fully off-axis.
inline constexpr float kOffAxisRolloffScale = 0.35f;

/// Longest cab impulse response retained, in MILLISECONDS. A guitar cab IR's
/// useful energy is gone within a few tens of milliseconds — the low resonance
/// of a 4x12 is the slowest part of it — and anything past that is capturing the
/// room, which belongs in a reverb insert. A longer IR is truncated rather than
/// rejected, and the convolution is a direct FIR so the cab costs no latency;
/// for an amp that matters more than tail length.
///
/// The budget is a duration and not a sample count on purpose: a fixed count
/// would silently halve the modelled tail at 96 kHz and quarter it at 192 kHz,
/// so the same IR would voice a different cabinet depending on the session rate.
/// It lives here rather than with the processor because the cabinet-IR generator
/// has to produce within the same budget the convolution consumes.
inline constexpr float kMaxCabIrMs = 40.0f;

/// Hard ceiling on the IR length in samples, whatever the rate. This bounds the
/// per-sample convolution cost at extreme rates rather than expressing anything
/// about cabinets.
inline constexpr int kMaxCabIrSamples = 8192;

/// Fixed EQ centres per cabinet model (see CabModel). The guitar values are the
/// original AmpSim voicing; the bass values model a big 8x10.
struct CabVoicing {
  float highpass_hz;  ///< low cut
  float bump_hz;      ///< body bump centre
  float bump_db;      ///< body bump gain
  float presence_hz;  ///< presence peak centre
  float rolloff_hz;   ///< top-end roll-off corner
};

CabVoicing cab_voicing(CabModel model) noexcept;

/// Mic capsule voicing: what an explicit microphone adds ON TOP of the cab's own
/// response (see MicModel). Not user parameters — the position and distance
/// controls ride on top of the selected capsule.
struct MicVoicing {
  float presence_hz;    ///< capsule presence peak centre
  float presence_db;    ///< its gain on-axis (falls to 0 fully off-axis)
  float rolloff_scale;  ///< multiplies the cab's top-end roll-off corner
  float proximity_hz;   ///< proximity low-shelf centre
  float proximity_db;   ///< its gain at the reference close distance
  float off_axis_hz;    ///< top-end shelf the position and distance both work on
  float off_axis_db;    ///< that shelf's gain fully off-axis
};

MicVoicing mic_voicing(MicModel model) noexcept;

/// One cabinet-plus-capsule design: the five cabinet biquads, plus the three a
/// capsule adds when one is selected.
struct CabDesign {
  rt::BiquadCoeffs hp, bump, presence, lp1, lp2;
  /// False when the mic model is kNone: the three mic biquads are then not
  /// stepped at all, which is what keeps the default chain bit-identical.
  bool mic = false;
  rt::BiquadCoeffs mic_prox, mic_presence, mic_top;
};

/// @brief Designs one cabinet-plus-capsule chain.
/// @param cab_model Cabinet voicing.
/// @param mic_model Capsule, or `kNone` for the cab's own baked-in close-mic
///        response (the three mic biquads are then left untouched).
/// @param axis Position across the cone in [0,1]; 0 is on-axis at the dust cap.
/// @param distance_cm Mic distance from the grille.
/// @param presence_db Presence peak gain, the one user control on the cabinet.
/// @param sample_rate Design rate.
CabDesign design_cab_stage(CabModel cab_model, MicModel mic_model, float axis, float distance_cm,
                           float presence_db, double sample_rate);

/// @brief Runs a signal through a cabinet design, from rest, and returns it.
/// @details Offline only (it allocates). This is the design's own impulse or
///          step response when fed one — the cabinet-IR generator's building
///          block, and the reason `CabDesign` is shared rather than private to
///          the realtime processor.
std::vector<float> render_cab_design(const CabDesign& design, const std::vector<float>& input);

}  // namespace sonare::mastering::saturation
