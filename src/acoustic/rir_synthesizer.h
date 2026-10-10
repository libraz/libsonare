#pragma once

/// @file rir_synthesizer.h
/// @brief Synthesize a full room impulse response: image-source early
///        reflections equal-power crossfaded onto the statistical
///        late-reverberation tail at the early/late crossover (mixing time).
///
/// This is the join point of the geometric model — discrete early reflections
/// (image-source method) up to the mixing time, then the dense diffuse tail
/// (per-band Sabine/Eyring decay) afterward — producing one mono RIR that the
/// convolution reverb path can apply. Offline / control-thread only.

#include <string>
#include <vector>

#include "acoustic/late_reverb.h"
#include "acoustic/room_model.h"
#include "core/audio.h"
#include "core/diagnostic.h"

namespace sonare::acoustic {

inline constexpr float kMaxRirSeconds = 600.0f;
inline constexpr float kMaxRirMixingTimeMs = 10000.0f;
inline constexpr float kMaxRirCrossfadeMs = 1000.0f;
/// Largest receiver-pair spacing (m) `validate_receiver_pair` accepts.
inline constexpr float kMaxReceiverSpacingM = 4.0f;

/// @brief Configuration for room-impulse-response synthesis.
struct RirSynthConfig {
  int ism_order = 3;  ///< image-source reflection order (early reflections)
  ReverbModel late_model = ReverbModel::Eyring;  ///< statistical tail RT60 model
  unsigned seed = 1u;                            ///< deterministic late-tail noise seed
  /// RIR length cap (s); 0 = auto from the longest RT60. An upper bound, with
  /// one floor: a cap shorter than the direct sound's own arrival would end the
  /// RIR before the first tap is rendered, so it is raised to fit the direct
  /// sound (see `synthesize_rir`) rather than yielding an all-zero response.
  float max_seconds = 0.0f;
  float mixing_time_ms = 0.0f;  ///< early/late crossover; 0 = auto (~sqrt(V) ms)
  float crossfade_ms = 5.0f;    ///< equal-power crossfade width around the mixing time
  /// Disabled by default so an existing caller's RIR is unchanged byte for
  /// byte. When enabled, the late tail's per-band RT60 gains the ISO 9613-1
  /// atmospheric-absorption term (see shoebox_reverb_time); `air` supplies the
  /// temperature/humidity and defaults to the ISO reference climate.
  bool air_absorption_enabled = false;
  AirAbsorption air{};
};

/// @brief Validate the non-geometry half of a RIR synthesis configuration.
///
/// Checks the image-source order, the timing values (`max_seconds`,
/// `mixing_time_ms`, `crossfade_ms`) and — only when `air_absorption_enabled` —
/// the atmospheric temperature/humidity, returning one Error Diagnostic per
/// failing group and an empty vector when the configuration is usable. The
/// sample rate is not part of the config and is checked by `synthesize_rir`.
///
/// `synthesize_rir` applies exactly these checks before it synthesizes anything,
/// so an engine that stores a `RirSynthConfig` can call this at construction and
/// reject the values up front rather than discovering the empty RIR at prepare()
/// time, where an empty IR degrades silently into dry passthrough.
std::vector<Diagnostic> validate_rir_synth_config(const RirSynthConfig& config);

/// @brief A synthesized RIR plus the diagnostics gathered producing it.
struct RirSynthResult {
  Audio rir;                            ///< mono synthesized room impulse response
  std::vector<Diagnostic> diagnostics;  ///< geometry validation + length-clamp telemetry
};

/// @brief The first Error diagnostic rendered as "code: message", empty when
///        the list carries no Error.
///
/// `synthesize_rir` reports a refused synthesis as an Error diagnostic plus an
/// empty RIR rather than by throwing, so a caller that cannot continue without a
/// RIR (a convolution engine, whose empty IR is inaudible) needs the reason to
/// put in its own exception instead of dropping the diagnostics on the floor.
std::string first_error_text(const std::vector<Diagnostic>& diagnostics);

/// @brief Synthesize a shoebox room impulse response (mono).
///
/// The geometry is validated first (see `validate_shoebox`); on any Error the
/// returned `rir` is empty and `diagnostics` carries the errors. Otherwise
/// Allen–Berkley image-source early reflections (to @p config.ism_order) are
/// equal-power crossfaded into the noise-shaped late tail (per-band RT60 from
/// the chosen model) at the mixing time. The late tail is level-matched to the
/// early reflections across the crossover so there is no energy discontinuity.
/// Output length follows the longest band RT60, clamped to @p config.max_seconds
/// (a Warning diagnostic is emitted when the clamp truncates the tail), and
/// floored so the direct sound always fits: a @p config.max_seconds shorter than
/// the source->listener flight time would otherwise end every buffer before the
/// first tap is rendered and return an all-zero RIR, which the convolution path
/// plays as digital silence. Such a cap is raised to the direct arrival plus the
/// fractional-delay kernel's half-width and an `acoustic.rir_length_floored`
/// Warning is emitted, so an accepted configuration always carries its direct
/// sound. When the room is effectively rigid (every band RT60 ~ 0) there is no
/// late tail, so the RIR is the early reflections alone (no crossfade-to-silence)
/// and an `acoustic.no_late_tail` Warning is emitted. The auto mixing time is
/// also pulled slightly earlier for rooms with high mean wall scattering.
RirSynthResult synthesize_rir(const ShoeboxRoom& room, const SourceListener& placement,
                              int sample_rate, const RirSynthConfig& config = {});

/// @brief Which receivers an engine synthesizes: the mono listener alone, or the mono
///        listener plus a spaced omnidirectional pair around it.
enum class ReceiverLayout { Mono, MonoAndPair };

/// @brief Positions of a spaced omnidirectional receiver pair.
struct ReceiverPair {
  Vec3 left;
  Vec3 right;
};

/// @brief Receiver pair centred on the listener, @p spacing_m apart.
///
/// The axis is horizontal and perpendicular to the horizontal listener->source direction f
/// (f = (1, 0, 0) when the source is within 1e-6 m horizontally): a = (-f.y, f.x, 0), the
/// listener's left in a z-up right-handed frame. left = listener + (s/2) a and
/// right = listener - (s/2) a, so both receivers are equidistant from the source.
ReceiverPair receiver_pair(const SourceListener& placement, float spacing_m) noexcept;

/// @brief Validate a receiver pair: `acoustic.receiver_spacing_out_of_range` when the spacing
///        is not finite or outside (0, 4] m, `acoustic.receiver_outside_room` naming the
///        receiver (left/right) and its coordinates when it leaves the room. Empty when usable.
std::vector<Diagnostic> validate_receiver_pair(const ShoeboxRoom& room,
                                               const SourceListener& placement, float spacing_m);

/// @brief A synthesized receiver-pair RIR plus the diagnostics gathered producing it.
struct RirPairResult {
  Audio left;
  Audio right;
  std::vector<Diagnostic> diagnostics;
};

/// @brief Synthesize the RIRs of a spaced omnidirectional receiver pair (see `receiver_pair`).
///
/// Validation follows `synthesize_rir` plus `validate_receiver_pair`; on any Error both RIRs
/// are empty. Each receiver gets its own image-source early reflections, mixing time and
/// crossfade; the late tails come from `synthesize_late_tail_pair`, so the left tail is the
/// mono stream and the right one carries the diffuse-field coherence per third-octave band.
/// One level-match scale, taken at the centre listener against the left tail, serves both
/// channels. Both RIRs are zero-padded to the longer length; diagnostics are the union of the
/// two receivers' lists, deduplicated by code in the left receiver's order.
RirPairResult synthesize_rir_pair(const ShoeboxRoom& room, const SourceListener& placement,
                                  float spacing_m, int sample_rate,
                                  const RirSynthConfig& config = {});

}  // namespace sonare::acoustic
