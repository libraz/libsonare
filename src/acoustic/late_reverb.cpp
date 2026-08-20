#include "acoustic/late_reverb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"

namespace sonare::acoustic {

namespace {

using sonare::constants::kSqrt2;
using sonare::constants::kTwoPiD;

// kSabineCoeff and kMaxAutoSamples are shared via late_reverb.h.

// -60 dB of energy: env(RT60) = 10^-3 in amplitude, i.e. exp(-ln(1000) * t/RT60).
constexpr double kLn1000 = 6.90775527898213705;

// Upper bound (seconds) on the reverberation time used to size the auto tail. No
// real room sustains longer; clamping here keeps a near-rigid room's effectively
// unbounded RT60 from driving a multi-gigabyte tail allocation (a hard,
// uncatchable abort under the WASM allocator).
constexpr float kMaxRt60Seconds = 60.0f;

// Deterministic, platform-independent PRNG (SplitMix64) so synthesized tails are
// bit-reproducible from the seed alone, never relying on std distribution
// implementations or any platform RNG.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Uniform double in [0, 1) from the top 53 bits.
  double uniform() noexcept {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }

 private:
  std::uint64_t state_;
};

// Standard-normal sample via the Box–Muller transform (self-contained so the
// result is identical across compilers given identical float behaviour).
float gaussian(SplitMix64& rng) noexcept {
  double u1 = rng.uniform();
  const double u2 = rng.uniform();
  if (u1 < 1e-12) u1 = 1e-12;  // guard log(0)
  return static_cast<float>(std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPiD * u2));
}

}  // namespace

// Octave-band centres matching the analyzer's split (kDefaultOctaveBands = 6:
// 125 .. 4000 Hz); higher band counts continue up by octaves.
float octave_center_hz(int band) noexcept {
  return 125.0f * std::pow(2.0f, static_cast<float>(band));
}

// Zero-phase octave bandpass (forward + backward biquad pass), matching the
// analyzer's RBJ bandpass at Q = sqrt(2) so a tail's measured per-band RT60
// tracks the design value.
//
// This is a deliberately minimal forward/backward biquad and is not routed
// through apply_biquad_filtfilt (filters/iir.cpp): that helper adds lfilter_zi
// edge-condition seeding, whereas here the input is white noise that is later
// per-band RMS-normalized and cross-faded, so zero initial conditions are fine
// and the seeded-vs-zero difference in the late tail is inaudible. Kept separate
// rather than sharing the filtfilt path (its to_filter_coeffs is file-local).
void octave_bandpass_zero_phase(std::vector<float>& x, float center_hz, int sample_rate) {
  const float w0 = static_cast<float>(kTwoPiD) * center_hz / static_cast<float>(sample_rate);
  const rt::BiquadCoeffs coeffs = rt::rbj_bandpass(w0, kSqrt2);

  rt::BiquadState state;
  state.set(coeffs);
  for (float& s : x) s = state.process(s);

  std::reverse(x.begin(), x.end());
  state.reset();
  for (float& s : x) s = state.process(s);
  std::reverse(x.begin(), x.end());
}

float sabine_rt60(float volume, float absorption_area) noexcept {
  if (volume <= 0.0f || absorption_area <= 0.0f) return 0.0f;
  return kSabineCoeff * volume / absorption_area;
}

float eyring_rt60(float volume, float surface_area, float mean_absorption) noexcept {
  if (volume <= 0.0f || surface_area <= 0.0f || mean_absorption <= 0.0f) return 0.0f;
  const float alpha = std::min(mean_absorption, 0.999f);
  const float denom = -surface_area * std::log(1.0f - alpha);
  if (denom <= 0.0f) return 0.0f;
  return kSabineCoeff * volume / denom;
}

float air_absorption_m_per_meter(float freq_hz, float temperature_c,
                                 float humidity_percent) noexcept {
  if (!std::isfinite(freq_hz) || freq_hz <= 0.0f) return 0.0f;
  // Absolute zero is a hard physical floor (the Kelvin conversion below would
  // go non-positive and feed pow()/log() a domain error); treat a caller-
  // supplied nonphysical temperature the same as the frequency guard above.
  if (!std::isfinite(temperature_c) || temperature_c <= kAbsoluteZeroCelsius) return 0.0f;

  // ISO 9613-1 pure-tone atmospheric absorption at sea-level pressure
  // (pa == reference pressure, so the pressure ratios drop out). Computed in
  // double for the exponentials, returned as the energy attenuation exponent m
  // (nepers/m) used by the Sabine/Eyring 4 m V air term.
  const double f = static_cast<double>(freq_hz);
  const double t = static_cast<double>(temperature_c) + 273.15;  // Kelvin
  const double hr = std::clamp(static_cast<double>(humidity_percent), 0.0, 100.0);
  constexpr double kT0 = 293.15;   // reference air temperature (20 degC)
  constexpr double kT01 = 273.16;  // triple-point isotherm

  // Molar concentration of water vapour (%), with pa == pr.
  const double psat_ratio = std::pow(10.0, -6.8346 * std::pow(kT01 / t, 1.261) + 4.6151);
  const double h = hr * psat_ratio;

  // Oxygen and nitrogen relaxation frequencies (Hz).
  const double fr_o = 24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h);
  const double fr_n = std::pow(t / kT0, -0.5) *
                      (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(t / kT0, -1.0 / 3.0) - 1.0)));
  if (!(fr_o > 0.0) || !(fr_n > 0.0)) return 0.0f;

  const double f2 = f * f;
  // Absorption in dB/m (the 8.686 = 20 log10(e) pressure-level factor).
  const double alpha_db =
      8.686 * f2 *
      (1.84e-11 * std::pow(t / kT0, 0.5) +
       std::pow(t / kT0, -2.5) * (0.01275 * std::exp(-2239.1 / t) / (fr_o + f2 / fr_o) +
                                  0.1068 * std::exp(-3352.0 / t) / (fr_n + f2 / fr_n)));
  if (!std::isfinite(alpha_db) || alpha_db <= 0.0) return 0.0f;

  // Convert pressure-level dB/m to the energy attenuation exponent (nepers/m):
  // I/I0 = 10^(-alpha_db * d / 10) = exp(-(alpha_db * ln10 / 10) * d).
  constexpr double kDbToNeperEnergy = 2.302585092994046 / 10.0;  // ln(10) / 10
  return static_cast<float>(alpha_db * kDbToNeperEnergy);
}

ReverbTime shoebox_reverb_time(const ShoeboxRoom& room, ReverbModel model,
                               const AirAbsorption* air) {
  const RoomDimensions& d = room.dims;
  // Per-wall areas, indexed by ShoeboxWall.
  const std::array<float, kShoeboxWallCount> wall_area{{
      d.width * d.height,   // kWallXMin
      d.width * d.height,   // kWallXMax
      d.length * d.height,  // kWallYMin
      d.length * d.height,  // kWallYMax
      d.length * d.width,   // kWallZMin
      d.length * d.width,   // kWallZMax
  }};

  // Shared octave-band reconciliation (MAX non-empty count, repeat-last padding)
  // so this RT60 path and the image-source specular path agree on band layout.
  size_t n_bands = reconcile_band_count(room.walls);
  // An all-rigid/empty room collapses to a single band above; keep the historical
  // default band count so the synthesized tail spans the full octave split.
  bool any_material = false;
  for (const Material& w : room.walls) any_material = any_material || !w.absorption.empty();
  if (!any_material) n_bands = static_cast<size_t>(kDefaultOctaveBands);

  const float volume = shoebox_volume(room);
  const float surface = shoebox_surface_area(room);

  ReverbTime rt;
  rt.rt60_bands.resize(n_bands, 0.0f);
  for (size_t b = 0; b < n_bands; ++b) {
    float absorption_area = 0.0f;
    for (size_t w = 0; w < kShoeboxWallCount; ++w) {
      // Shared repeat-last padding (rigid/empty material reads as α=0).
      const float alpha = material_alpha_at(room.walls[w], b);
      absorption_area += wall_area[w] * alpha;
    }
    if (air == nullptr) {
      // Geometry-only path: byte-identical to the pre-air-absorption result.
      if (model == ReverbModel::Sabine) {
        rt.rt60_bands[b] = sabine_rt60(volume, absorption_area);
      } else {
        const float mean_alpha = surface > 0.0f ? absorption_area / surface : 0.0f;
        rt.rt60_bands[b] = eyring_rt60(volume, surface, mean_alpha);
      }
      continue;
    }

    // Add the classic 4 m V atmospheric absorption term to the denominator so
    // high bands in large rooms stop over-predicting RT60.
    const float m = air_absorption_m_per_meter(octave_center_hz(static_cast<int>(b)),
                                               air->temperature_c, air->humidity_percent);
    const float air_term = 4.0f * m * volume;
    if (model == ReverbModel::Sabine) {
      rt.rt60_bands[b] = sabine_rt60(volume, absorption_area + air_term);
    } else {
      const float mean_alpha = surface > 0.0f ? absorption_area / surface : 0.0f;
      const float alpha = std::min(mean_alpha, 0.999f);
      const float denom = -surface * std::log(1.0f - alpha) + air_term;
      rt.rt60_bands[b] =
          (volume > 0.0f && surface > 0.0f && denom > 0.0f) ? kSabineCoeff * volume / denom : 0.0f;
    }
  }
  return rt;
}

Audio synthesize_late_tail(const ReverbTime& rt, int sample_rate, const LateReverbConfig& config) {
  const LateTailResolution resolution = resolve_late_tail(rt, sample_rate, config);
  if (resolution.samples == 0u) {
    return Audio::from_vector(std::vector<float>{}, sample_rate);
  }

  const float sr = static_cast<float>(sample_rate);
  const float nyquist = sr * 0.5f;
  const int length = static_cast<int>(resolution.samples);

  std::vector<float> out(static_cast<size_t>(length), 0.0f);
  std::vector<float> band(static_cast<size_t>(length));

  for (size_t b = 0; b < rt.rt60_bands.size(); ++b) {
    const float rt60 = rt.rt60_bands[b];
    if (!(rt60 > 0.0f)) continue;
    const float center = octave_center_hz(static_cast<int>(b));
    if (center * kSqrt2 >= nyquist) continue;  // band above the representable range

    // Decorrelated, reproducible noise stream per band.
    SplitMix64 rng(static_cast<std::uint64_t>(config.seed) +
                   0x9E3779B9ull * (static_cast<std::uint64_t>(b) + 1ull));
    for (int i = 0; i < length; ++i) band[static_cast<size_t>(i)] = gaussian(rng);

    octave_bandpass_zero_phase(band, center, sample_rate);

    // Normalize each band to unit RMS so its starting level is independent of
    // the bandpass bandwidth (which grows with centre frequency at fixed Q).
    // Without this, higher bands contribute disproportionate energy purely as a
    // filter-bandwidth artifact, tilting the tail spectrum away from the
    // material-derived per-band decay. The decay envelope (set by RT60) then
    // governs each band's relative weight.
    const float band_rms = sonare::rms(band.data(), band.size());
    if (band_rms > 1e-12f) {
      const float norm = 1.0f / band_rms;
      for (float& s : band) s *= norm;
    }

    const double decay_rate = kLn1000 / static_cast<double>(rt60);
    for (int i = 0; i < length; ++i) {
      const double t = static_cast<double>(i) / sr;
      const float env = static_cast<float>(std::exp(-decay_rate * t));
      out[static_cast<size_t>(i)] += band[static_cast<size_t>(i)] * env;
    }
  }

  return Audio::from_vector(std::move(out), sample_rate);
}

LateTailResolution resolve_late_tail(const ReverbTime& rt, int sample_rate,
                                     const LateReverbConfig& config) noexcept {
  LateTailResolution resolution;
  if (sample_rate <= 0) return resolution;

  const float sr = static_cast<float>(sample_rate);
  const float nyquist = sr * 0.5f;
  float longest = 0.0f;
  for (size_t b = 0; b < rt.rt60_bands.size(); ++b) {
    const float center = octave_center_hz(static_cast<int>(b));
    if (center * kSqrt2 >= nyquist) continue;
    const float rt60 = rt.rt60_bands[b];
    // NaN is not a finite decay; positive infinity retains the historical
    // 60-second sizing clamp without ever entering an unbounded cast.
    if (!(rt60 > 0.0f)) continue;
    longest = std::max(longest, std::min(rt60, kMaxRt60Seconds));
  }
  if (!(longest > 0.0f)) return resolution;

  constexpr std::size_t kMaxSamples =
      std::min(static_cast<std::size_t>(kMaxAutoSamples), resource::kMaxAcousticRirSamples);
  static_assert(kMaxSamples <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
                "resolved acoustic tail length must fit the synthesis loop index");

  // Keep the historical double expression for ordinary finite values so the
  // normal <=cap tail length (and therefore its seeded samples) stays
  // bit-stable. The largest finite float headroom and int sample rate still
  // fit comfortably in double; positive infinity is handled as saturation.
  const float non_negative_headroom = config.headroom > 0.0f ? config.headroom : 0.0f;
  const double raw = std::isinf(non_negative_headroom)
                         ? std::numeric_limits<double>::infinity()
                         : std::ceil(static_cast<double>(longest) *
                                     (1.0 + static_cast<double>(non_negative_headroom)) *
                                     static_cast<double>(sample_rate));
  // Clamp before narrowing so even a saturated/non-finite product never reaches
  // an undefined integral cast.
  const double bounded = std::min(raw, static_cast<double>(kMaxSamples));
  resolution.resource_clamped = raw > static_cast<double>(kMaxSamples);
  std::size_t length = bounded > 0.0 ? static_cast<std::size_t>(bounded) : 0u;
  if (config.max_samples > 0) {
    length = std::min(length, static_cast<std::size_t>(config.max_samples));
  }
  resolution.samples = std::max<std::size_t>(1u, length);
  return resolution;
}

std::size_t resolve_late_tail_samples(const ReverbTime& rt, int sample_rate,
                                      const LateReverbConfig& config) noexcept {
  return resolve_late_tail(rt, sample_rate, config).samples;
}

}  // namespace sonare::acoustic
