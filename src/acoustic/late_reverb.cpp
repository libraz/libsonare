#include "acoustic/late_reverb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

// Octave-band centres matching the analyzer's split (kDefaultOctaveBands = 6:
// 125 .. 4000 Hz); higher band counts continue up by octaves.
float octave_center_hz(int band) noexcept {
  return 125.0f * std::pow(2.0f, static_cast<float>(band));
}

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

// Zero-phase octave bandpass (forward + backward biquad pass), matching the
// analyzer's RBJ bandpass at Q = sqrt(2) so a tail's measured per-band RT60
// tracks the design value.
void bandpass_zero_phase(std::vector<float>& x, float center_hz, int sample_rate) {
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

}  // namespace

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

ReverbTime shoebox_reverb_time(const ShoeboxRoom& room, ReverbModel model) {
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
    if (model == ReverbModel::Sabine) {
      rt.rt60_bands[b] = sabine_rt60(volume, absorption_area);
    } else {
      const float mean_alpha = surface > 0.0f ? absorption_area / surface : 0.0f;
      rt.rt60_bands[b] = eyring_rt60(volume, surface, mean_alpha);
    }
  }
  return rt;
}

Audio synthesize_late_tail(const ReverbTime& rt, int sample_rate, const LateReverbConfig& config) {
  const float sr = static_cast<float>(sample_rate);
  if (sample_rate <= 0) {
    return Audio::from_vector(std::vector<float>{}, sample_rate);
  }

  const float nyquist = sr * 0.5f;
  float longest = 0.0f;
  for (size_t b = 0; b < rt.rt60_bands.size(); ++b) {
    const float center = octave_center_hz(static_cast<int>(b));
    if (center * kSqrt2 >= nyquist) continue;
    longest = std::max(longest, rt.rt60_bands[b]);
  }
  if (longest <= 0.0f) {
    return Audio::from_vector(std::vector<float>{}, sample_rate);
  }

  const float headroom = std::max(0.0f, config.headroom);
  // Compute in double and clamp to the safety ceiling so an unbounded RT60 (a
  // near-rigid room) cannot overflow the int length / request a huge allocation.
  const double raw =
      std::ceil(static_cast<double>(longest) * (1.0 + headroom) * static_cast<double>(sample_rate));
  int length = static_cast<int>(std::min(raw, static_cast<double>(kMaxAutoSamples)));
  if (config.max_samples > 0) length = std::min(length, config.max_samples);
  if (length < 1) length = 1;

  std::vector<float> out(static_cast<size_t>(length), 0.0f);
  std::vector<float> band(static_cast<size_t>(length));

  for (size_t b = 0; b < rt.rt60_bands.size(); ++b) {
    const float rt60 = rt.rt60_bands[b];
    if (rt60 <= 0.0f) continue;
    const float center = octave_center_hz(static_cast<int>(b));
    if (center * kSqrt2 >= nyquist) continue;  // band above the representable range

    // Decorrelated, reproducible noise stream per band.
    SplitMix64 rng(static_cast<std::uint64_t>(config.seed) +
                   0x9E3779B9ull * (static_cast<std::uint64_t>(b) + 1ull));
    for (int i = 0; i < length; ++i) band[static_cast<size_t>(i)] = gaussian(rng);

    bandpass_zero_phase(band, center, sample_rate);

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

}  // namespace sonare::acoustic
