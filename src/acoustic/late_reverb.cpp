#include "acoustic/late_reverb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "filters/iir.h"
#include "util/constants.h"

namespace sonare::acoustic {

namespace {

using sonare::constants::kSqrt2;
using sonare::constants::kTwoPiD;

// kSabineCoeff and kMaxAutoSamples are shared via late_reverb.h.

// Butterworth order of each octave crossover (96 dB/oct once run zero-phase). A single RBJ
// bandpass let a 6 s low band outlast a 0.2 s 4 kHz band's own decay after ~0.2 s.
constexpr int kOctaveSplitOrder = 8;

// Crossover between octave band @p band and the next one up.
float octave_upper_edge_hz(int band) noexcept { return octave_center_hz(band) * kSqrt2; }

// The late tail decays on a third-octave grid whose every third band is an octave centre.
constexpr int kThirdsPerOctave = 3;

// Crossover above third-octave band @p k (centre 125 * 2^(k/3)).
float third_octave_upper_edge_hz(int k) noexcept {
  return 125.0f * std::pow(2.0f, (static_cast<float>(k) + 0.5f) / kThirdsPerOctave);
}

// RT60 of third-octave band @p k, log-log interpolated between its octave neighbours; a band
// beside a non-positive (no tail) octave takes the nearer octave's value.
float third_octave_rt60(const std::vector<float>& octave_rt60, int k) noexcept {
  const int lower = k / kThirdsPerOctave;
  const int step = k % kThirdsPerOctave;
  const float lo = octave_rt60[static_cast<size_t>(lower)];
  if (step == 0) return lo;
  const float hi = octave_rt60[static_cast<size_t>(lower + 1)];
  if (!(lo > 0.0f) || !(hi > 0.0f) || !std::isfinite(lo) || !std::isfinite(hi)) {
    return 2 * step < kThirdsPerOctave ? lo : hi;
  }
  const float frac = static_cast<float>(step) / kThirdsPerOctave;
  return std::exp(std::log(lo) + frac * (std::log(hi) - std::log(lo)));
}

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

int octave_split_band_count(std::size_t bands, int sample_rate) noexcept {
  if (sample_rate <= 0) return 0;
  const float nyquist = static_cast<float>(sample_rate) * 0.5f;
  int count = 0;
  while (static_cast<std::size_t>(count) < bands && octave_center_hz(count) * kSqrt2 < nyquist) {
    ++count;
  }
  return count;
}

void octave_band_zero_phase(std::vector<float>& x, int band, int band_count, int sample_rate) {
  std::vector<float> low;
  // Peel off every band below this one; what remains is the complement above its lower edge.
  for (int b = 0; b < band; ++b) {
    low = x;
    butterworth_zero_phase(low, octave_upper_edge_hz(b), sample_rate, kOctaveSplitOrder, false);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] -= low[i];
  }
  if (band + 1 < band_count) {
    butterworth_zero_phase(x, octave_upper_edge_hz(band), sample_rate, kOctaveSplitOrder, false);
  }
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
  const int length = static_cast<int>(resolution.samples);

  // Complementary bands of one white stream: no band's decay leaks into another through a skirt.
  std::vector<float> residual(static_cast<size_t>(length));
  SplitMix64 rng(static_cast<std::uint64_t>(config.seed));
  for (float& s : residual) s = gaussian(rng);

  // Third-octave bands, RT60 interpolated log-log, so the decay does not step at an octave edge.
  std::vector<float> out(static_cast<size_t>(length), 0.0f);
  std::vector<float> band;
  const int octaves = octave_split_band_count(rt.rt60_bands.size(), sample_rate);
  const int thirds = octaves > 0 ? kThirdsPerOctave * (octaves - 1) + 1 : 0;
  for (int k = 0; k < thirds; ++k) {
    if (k + 1 == thirds) {
      band.swap(residual);
    } else {
      band = residual;
      butterworth_zero_phase(band, third_octave_upper_edge_hz(k), sample_rate, kOctaveSplitOrder,
                             false);
      for (std::size_t i = 0; i < band.size(); ++i) residual[i] -= band[i];
    }

    const float rt60 = third_octave_rt60(rt.rt60_bands, k);
    if (!(rt60 > 0.0f)) continue;
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
