#pragma once

/// @file late_reverb.h
/// @brief Statistical late-reverberation tail: per-octave-band reverberation
///        time from room geometry (Sabine/Eyring) and a deterministic
///        noise-shaped impulse-response tail whose per-band energy decays at
///        those reverberation times.
///
/// The early reflections come from the image-source method; this fills in the
/// dense, diffuse late field that the image-source method intentionally stops
/// enumerating. The tail is octave-band-filtered Gaussian noise (fixed seed,
/// never platform RNG) shaped by an exponential decay envelope, so its backward
/// Schroeder integral is monotonically non-increasing by construction and its
/// per-band decay is set directly by the design reverberation times.

#include <vector>

#include "acoustic/room_model.h"
#include "core/audio.h"

namespace sonare::acoustic {

/// @brief Metric Sabine/Eyring proportionality constant 24 ln(10) / c with
///        c ~= 343 m/s (textbook value). Shared by the forward RT60 model
///        (`sabine_rt60`/`eyring_rt60`) and the room estimator's geometry
///        inversion so the estimate inverts the synthesis exactly.
inline constexpr float kSabineCoeff = 0.161f;

/// @brief Safety ceiling for the auto-sized late-tail length in samples
///        (~22 min at 48 kHz). A near-rigid room yields an unbounded RT60,
///        which would otherwise overflow the length computation; the auto
///        length is clamped here so the result stays allocatable. Callers
///        needing a precise (possibly larger) length set
///        `LateReverbConfig::max_samples`.
inline constexpr int kMaxAutoSamples = 1 << 26;  // 67,108,864

/// @brief Sabine reverberation time (s): 0.161 * V / A.
///
/// @param volume          room volume V (m^3)
/// @param absorption_area total absorption A = sum over surfaces of (area * alpha) (m^2 sabins)
/// Returns 0 for a non-positive volume or absorption area (a perfectly
/// reflective room has unbounded RT60, reported as 0 so callers clamp/skip
/// explicitly rather than propagating infinity).
float sabine_rt60(float volume, float absorption_area) noexcept;

/// @brief Eyring reverberation time (s): 0.161 * V / (-S * ln(1 - mean_alpha)).
///
/// More accurate than Sabine when the mean absorption is high (alpha-bar
/// above ~0.2), where Sabine over-predicts RT60.
/// @param volume          room volume V (m^3)
/// @param surface_area    total interior surface area S (m^2)
/// @param mean_absorption area-weighted mean absorption alpha-bar; clamped to
///                        [0, 1) to keep the logarithm finite. Returns 0 for a
///                        non-positive mean absorption (see `sabine_rt60`).
float eyring_rt60(float volume, float surface_area, float mean_absorption) noexcept;

/// @brief Statistical-reverberation model selector.
enum class ReverbModel {
  Sabine,  ///< classic; accurate for low/moderate mean absorption
  Eyring,  ///< preferred for high mean absorption (alpha-bar above ~0.2)
};

/// @brief Per-octave-band reverberation time (seconds).
///
/// One entry per octave band, ordered like the materials and the analyzer's
/// band split (nominal 125 / 250 / 500 / 1k / 2k / 4k Hz for `kDefaultOctaveBands`).
/// A band value of 0 means "no finite decay" (perfectly reflective in that band)
/// and the synthesizer renders no tail for it.
struct ReverbTime {
  std::vector<float> rt60_bands;
};

/// @brief Per-band RT60 of a shoebox from its geometry and wall materials.
///
/// The absorption area of each band is the sum over the six walls of
/// (wall area * wall alpha at that band). The band count follows the longest
/// wall material (or `kDefaultOctaveBands` when every wall is rigid/empty);
/// bands past a material's length reuse its last coefficient. Rigid (empty)
/// walls contribute zero absorption, giving an RT60 of 0 for that band.
ReverbTime shoebox_reverb_time(const ShoeboxRoom& room, ReverbModel model = ReverbModel::Eyring);

/// @brief Configuration for late-tail synthesis.
struct LateReverbConfig {
  unsigned seed = 1u;     ///< deterministic noise seed (never platform/Math.random)
  int max_samples = 0;    ///< hard length cap in samples; 0 = size from the longest band
  float headroom = 1.0f;  ///< extra tail length as a multiple of the longest RT60 past -60 dB
};

/// @brief Synthesize the mono statistical late-reverberation tail.
///
/// For each band with RT60 > 0: deterministic white Gaussian noise (seeded from
/// @p config.seed mixed with the band index, so bands are decorrelated and
/// reproducible) is octave-band filtered and multiplied by the amplitude
/// envelope exp(-ln(1000) * t / RT60_band) (which is -60 dB of *energy* at
/// t = RT60_band), then the bands are summed. The result starts at t = 0 and is
/// left at its natural noise level; the RIR synthesizer scales it to meet the
/// early reflections at the early/late crossover. Returns empty audio when no
/// band has a finite decay.
Audio synthesize_late_tail(const ReverbTime& rt, int sample_rate,
                           const LateReverbConfig& config = {});

}  // namespace sonare::acoustic
