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

}  // namespace sonare::acoustic
