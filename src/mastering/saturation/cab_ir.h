#pragma once

/// @file cab_ir.h
/// @brief Synthesizes a cabinet impulse response from a cabinet model, so a cab
///        IR is available without shipping — or sourcing — a recording.
///
/// A commercial cab IR is a microphone recording of a specific cabinet, which
/// makes it data with its own copyright, its own file size and its own claim on
/// whichever cabinet it captured. This library ships models rather than
/// recordings, so the IR is generated instead.
///
/// What the generator adds over the analytic cab chain is the one thing a
/// cascade of biquads structurally cannot produce: the cabinet's GEOMETRY. A
/// real cabinet has several drivers on one baffle, and a microphone in front of
/// one of them also hears the others, later (path length), quieter (spherical
/// spreading) and darker (each neighbour is seen far off its own axis). That
/// summation is a comb filter whose spacing follows the mic distance, and it is
/// the reason backing a mic off a 4x12 changes its character rather than just
/// its level.
///
/// The split of responsibility is deliberate and is what keeps the model free of
/// invented tone:
///
///   - The MIKED driver's response is exactly `design_cab_stage()` — the same
///     calibrated cabinet and capsule voicing the realtime chain uses. The
///     generator restates none of it.
///   - Each NEIGHBOUR is that same response, attenuated by 1/r, delayed by the
///     path difference, and rolled off by the directivity of a rigid circular
///     piston at that neighbour's geometric angle.
///
/// One mechanism per effect, so nothing is counted twice: with the neighbours
/// switched off the generated IR reproduces the analytic cab, which is asserted
/// as a test rather than asserted in prose.
///
/// What the geometry produces, measured rather than asserted: a low-frequency
/// lift that grows with mic distance (+1 dB at 2.5 cm, +5 dB at 15 cm, +7 dB at
/// 60 cm), because drivers less than a quarter-wavelength apart couple and sum
/// coherently — the documented reason a big multi-driver cabinet sounds darker
/// than one of its own drivers — over a comb whose first notch walks down from
/// the low mids as the mic backs off.
///
/// Honest limits. The cabinet dimensions and cone areas below are the standard
/// sizes of the cabinets being modelled; the piston directivity is textbook. The
/// breakup frequency is not a published quantity — it is the one voicing number
/// here, and it only sets how fast a neighbour's directivity tightens above the
/// frequency where a cone stops moving as a rigid piston.
///
/// The drivers are treated as identical and perfectly coherent, which is what
/// makes the interference exact — and it is exact in the wrong direction at
/// distance: the model nulls harder than a real cabinet, whose drivers differ by
/// a few percent in sensitivity and resonance and whose nulls fill in
/// accordingly. Under a close mic the miked driver dominates and the two agree
/// (worst-case 1.4 dB at 2.5 cm, 5 dB at 8 cm); by 30 cm the model predicts a
/// notch some 24 dB deep that no cabinet measures. Close-mic distances are where
/// this is trustworthy, which is also where cabinets are actually miked; the
/// escape hatch for anything else is a single driver.
///
/// Room reflections are deliberately absent: this models the microphone on the
/// cabinet, not the room around it, which belongs in a reverb insert. There is
/// no floor and no back wall, so the comb here is the cabinet's own.

#include <vector>

#include "mastering/saturation/cab_voicing.h"

namespace sonare::mastering::saturation {

/// The physical cabinet behind a `CabModel`: driver size, layout and spacing.
/// These are the dimensions of the cabinet being modelled, not tuning knobs.
struct CabGeometry {
  /// Effective radiating area of one cone (m^2). The radiating radius follows
  /// from it, which is the quantity the directivity actually needs.
  float cone_area_m2;
  /// Where the cone stops behaving as a rigid piston. Above this the radiating
  /// area shrinks with frequency, so a driver beams less hard than piston theory
  /// predicts. The one number here that is a voicing choice rather than a
  /// measurement.
  float breakup_hz;
  int columns;      ///< Drivers across the baffle.
  int rows;         ///< Drivers up the baffle.
  float pitch_x_m;  ///< Horizontal centre-to-centre spacing.
  float pitch_y_m;  ///< Vertical centre-to-centre spacing.
};

CabGeometry cab_geometry(CabModel model) noexcept;

/// @brief The argument at which a rigid circular piston's directivity is 3 dB
///        down, i.e. the root of `2*J1(x)/x == 1/sqrt(2)`.
/// @details Solved rather than tabulated, so the constant cannot drift from the
///          function it is supposed to describe.
float piston_minus3db_argument() noexcept;

/// What to generate.
struct CabIrSpec {
  /// Cabinet voicing and geometry.
  CabModel cab_model = CabModel::kGuitar4x12;
  /// Capsule in front of it. `kNone` gives the cabinet's own baked-in close-mic
  /// voicing, exactly as in the analytic chain.
  MicModel mic_model = MicModel::kNone;
  /// Position across the miked cone in [0,1] (0 = on-axis at the dust cap).
  float mic_axis = 0.0f;
  /// Mic distance from the grille in cm. This is the control the generated IR
  /// responds to most strongly, because it sets the neighbour path differences
  /// and therefore the comb spacing.
  float mic_distance_cm = kMicReferenceDistanceCm;
  /// Presence peak gain (dB) on the cabinet, matching `AmpSimConfig::presence_db`.
  float presence_db = 0.0f;
  /// Sum the cabinet's other drivers. False leaves a single driver, which is the
  /// analytic cab's own response and therefore the model's identity case.
  bool multi_driver = true;
  /// IR length in ms. Clamped to the convolution budget (`kMaxCabIrMs`).
  float length_ms = kMaxCabIrMs;
};

/// @brief Generates a cabinet impulse response at @p sample_rate.
/// @param spec Cabinet, capsule and mic placement.
/// @param sample_rate Rate to generate at. Generate at the processor's own rate
///        and no resampling is needed; `AmpSim` does exactly that.
/// @return The IR, ready for `AmpSim::load_cab_ir()`.
/// @throws SonareException on a non-positive sample rate.
/// @details Level. The cabinet is matched to the single driver's ENERGY, so
///          turning a generated cabinet on is a change of character rather than
///          of level. Matching the LOW END instead would have been the obvious
///          choice and is the wrong one: the low-frequency coupling gain is a
///          real, measurable property of a multi-driver cabinet, and dividing it
///          out re-expresses it as a midrange scoop that no cabinet has.
///
///          Bandwidth. Truncating to the convolution budget costs nothing
///          measurable — a single-driver IR reproduces the analytic chain to
///          better than 0.001 dB from 40 Hz to 5 kHz, because the design's own
///          ring is well inside the budget. The truncation is faded rather than
///          cut all the same.
///
///          Control thread only (allocates).
std::vector<float> generate_cab_ir(const CabIrSpec& spec, double sample_rate);

}  // namespace sonare::mastering::saturation
