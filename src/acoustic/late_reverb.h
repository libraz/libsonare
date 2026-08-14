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

#include <cstddef>
#include <vector>

#include "acoustic/room_model.h"
#include "core/audio.h"
#include "util/resource_limits.h"

namespace sonare::acoustic {

/// @brief Metric Sabine/Eyring proportionality constant 24 ln(10) / c with
///        c ~= 343 m/s (textbook value). Shared by the forward RT60 model
///        (`sabine_rt60`/`eyring_rt60`) and the room estimator's geometry
///        inversion so the estimate inverts the synthesis exactly.
inline constexpr float kSabineCoeff = 0.161f;

/// @brief Historical upper bound for auto-sized acoustic buffers.
///
/// The effective RIR/late-tail cap is lower and is derived from the shared
/// four-buffer working-set budget in `resource_limits.h`. Keep this ceiling as
/// an upper bound for compatibility with the early image-source path.
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

/// @brief Atmospheric conditions for the optional air-absorption term.
///
/// Defaults are the ISO reference climate (20 degC, 50 % relative humidity) at
/// sea-level pressure. Only used when explicitly passed to
/// `shoebox_reverb_time`; the geometry-only path ignores air absorption.
struct AirAbsorption {
  float temperature_c = 20.0f;     ///< air temperature in degrees Celsius
  float humidity_percent = 50.0f;  ///< relative humidity in percent [0, 100]
};

/// @brief Pure-tone atmospheric absorption coefficient (energy, nepers/m).
///
/// ISO 9613-1 model at sea-level ambient pressure. Returns the energy
/// attenuation exponent m such that intensity decays as exp(-m * distance);
/// this is the coefficient used by the Sabine/Eyring `4 m V` air term. Grows
/// steeply with frequency, so it mainly shortens the high-band reverberation
/// time of large rooms. Returns 0 for a non-positive or non-finite frequency.
float air_absorption_m_per_meter(float freq_hz, float temperature_c,
                                 float humidity_percent) noexcept;

/// @brief Nominal octave-band centre frequency (Hz) for band index @p band.
///
/// Matches the analyzer's split (125 Hz for band 0, rising by octaves). Shared
/// with the early-reflection colourer so early reflections and the late tail
/// place identical per-band shaping on the same octave grid.
float octave_center_hz(int band) noexcept;

/// @brief Zero-phase octave bandpass (forward + backward RBJ biquad at
///        Q = sqrt(2)), applied to @p x in place.
///
/// The same minimal forward/backward biquad the late tail uses to shape each
/// octave band, exposed so the early-reflection colourer can isolate a band's
/// material-dependent deviation onto the identical octave grid.
void octave_bandpass_zero_phase(std::vector<float>& x, float center_hz, int sample_rate);

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
/// When @p air is non-null, the per-band absorption gains the classic `4 m V`
/// atmospheric term (m from `air_absorption_m_per_meter`), which curbs the
/// high-frequency RT60 over-prediction of large rooms. Passing nullptr (the
/// default) reproduces the geometry-only result exactly.
ReverbTime shoebox_reverb_time(const ShoeboxRoom& room, ReverbModel model = ReverbModel::Eyring,
                               const AirAbsorption* air = nullptr);

/// @brief Configuration for late-tail synthesis.
struct LateReverbConfig {
  unsigned seed = 1u;     ///< deterministic noise seed (never platform/Math.random)
  int max_samples = 0;    ///< hard length cap in samples; 0 = size from the longest band
  float headroom = 1.0f;  ///< extra tail length as a multiple of the longest RT60 past -60 dB
};

/// @brief Allocation-free result of late-tail length resolution.
struct LateTailResolution {
  std::size_t samples = 0;
  bool resource_clamped = false;  ///< auto length exceeded the shared RIR budget
};

/// @brief Resolve a late-tail length and report resource-budget clipping.
///
/// `resource_clamped` describes the auto-sized length before an explicit
/// `max_samples` cap is applied. It remains true when an explicit cap makes the
/// returned sample count smaller, allowing RIR diagnostics to distinguish the
/// shared resource budget from a caller-requested upper bound.
LateTailResolution resolve_late_tail(const ReverbTime& rt, int sample_rate,
                                     const LateReverbConfig& config = {}) noexcept;

/// @brief Resolve the storage length for a late-reverberation tail.
///
/// The result is allocation-free and applies the same policy used by
/// `synthesize_late_tail`: invalid sample rates and rooms with no representable
/// positive-decay band return zero; bands at/above Nyquist do not affect the
/// length; RT60 is capped at 60 seconds for sizing; and a positive explicit
/// `max_samples` is an upper bound. The shared acoustic working-set limit is
/// applied after the historical `kMaxAutoSamples` ceiling, so even hostile
/// sample-rate/RT60/headroom combinations remain representable as `int`.
std::size_t resolve_late_tail_samples(const ReverbTime& rt, int sample_rate,
                                      const LateReverbConfig& config = {}) noexcept;

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
