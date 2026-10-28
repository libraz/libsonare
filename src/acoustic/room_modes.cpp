#include "acoustic/room_modes.h"

#include <algorithm>
#include <cmath>

#include "acoustic/image_source.h"
#include "filters/iir.h"
#include "util/constants.h"

namespace sonare::acoustic {

namespace {

using sonare::constants::kLn1000D;
using sonare::constants::kLn10D;
using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

// Schroeder's constant: f_s = 2000 sqrt(T60 / V) with T60 in s and V in m^3.
constexpr double kSchroederCoeff = 2000.0;

// Butterworth order of the modal/diffuse crossover; run zero-phase its gain at twice the
// crossover is 1/257 (-48 dB), which is where mode enumeration stops.
constexpr int kModalSplitOrder = 4;
constexpr double kModalCeilingRatio = 2.0;

// Mode budget: small rooms with long decays put thousands of modes under the ceiling, and
// past this the crossover is lowered rather than the cost grown.
constexpr std::size_t kMaxRoomModes = 1024;

// Rendering budget in mode-samples (~1 s of single-core work); a near-rigid room's minute-long
// tail lowers the crossover instead of stalling prepare().
constexpr double kMaxModalWork = 1073741824.0;

// A mode is rendered until its envelope reaches -140 dB, under float resolution of its peak.
constexpr double kModeTailLn = 7.0 * kLn10D;

// Crossover refinement passes when the mode count or the work exceeds its budget.
constexpr float kCrossoverStep = 0.9f;
constexpr int kMaxCrossoverSteps = 64;

// Leading terms of the shoebox mode-count asymptote below f: volume, surface and edge terms.
double estimated_mode_count(const RoomDimensions& d, double f) noexcept {
  const double c = kSoundSpeed;
  const double v = static_cast<double>(d.length) * d.width * d.height;
  const double s =
      2.0 * (static_cast<double>(d.length) * d.width + static_cast<double>(d.length) * d.height +
             static_cast<double>(d.width) * d.height);
  const double edges = 4.0 * (static_cast<double>(d.length) + d.width + d.height);
  return 4.0 * kPiD * v * f * f * f / (3.0 * c * c * c) + kPiD * s * f * f / (4.0 * c * c) +
         edges * f / (8.0 * c);
}

double mode_frequency(const RoomDimensions& d, int nx, int ny, int nz) noexcept {
  const double x = nx / static_cast<double>(d.length);
  const double y = ny / static_cast<double>(d.width);
  const double z = nz / static_cast<double>(d.height);
  return 0.5 * kSoundSpeed * std::sqrt(x * x + y * y + z * z);
}

// Samples a mode is rendered for: until -140 dB, never past the tail.
std::size_t mode_samples(const RoomMode& mode, std::size_t length, int sample_rate) noexcept {
  const double n = kModeTailLn / static_cast<double>(mode.decay_per_s) * sample_rate;
  return n < static_cast<double>(length) ? static_cast<std::size_t>(n) + 1 : length;
}

// Spatial mean of a mode's squared shape: 1/2 per non-zero index.
double shape_mean_square(const RoomMode& mode) noexcept {
  return (mode.nx == 0 ? 1.0 : 0.5) * (mode.ny == 0 ? 1.0 : 0.5) * (mode.nz == 0 ? 1.0 : 0.5);
}

double mode_shape(const RoomMode& mode, const RoomDimensions& d, const Vec3& p) noexcept {
  return std::cos(mode.nx * kPiD * p.x / d.length) * std::cos(mode.ny * kPiD * p.y / d.width) *
         std::cos(mode.nz * kPiD * p.z / d.height);
}

}  // namespace

float schroeder_frequency_hz(float rt60_s, float volume_m3) noexcept {
  if (!(rt60_s > 0.0f) || !(volume_m3 > 0.0f) || !std::isfinite(rt60_s)) return 0.0f;
  return static_cast<float>(kSchroederCoeff * std::sqrt(static_cast<double>(rt60_s) / volume_m3));
}

ModalPlan plan_room_modes(const ShoeboxRoom& room, const ReverbTime& rt, int sample_rate,
                          std::size_t length) {
  ModalPlan plan;
  const RoomDimensions& d = room.dims;
  const float volume = shoebox_volume(room);
  if (sample_rate <= 0 || length == 0 || !(volume > 0.0f) || rt.rt60_bands.empty()) return plan;

  // One fixed-point step: Schroeder at the lowest band's RT60, then at the RT60 there.
  float crossover = schroeder_frequency_hz(rt60_at_hz(rt, octave_center_hz(0)), volume);
  crossover = schroeder_frequency_hz(rt60_at_hz(rt, crossover), volume);
  // The enumeration ceiling must stay below Nyquist.
  crossover = std::min(crossover, static_cast<float>(sample_rate) * 0.25f);
  for (int step = 0;
       step < kMaxCrossoverSteps &&
       estimated_mode_count(d, kModalCeilingRatio * crossover) > static_cast<double>(kMaxRoomModes);
       ++step) {
    crossover *= kCrossoverStep;
  }
  if (!(crossover > 0.0f)) return plan;

  for (int step = 0; step < kMaxCrossoverSteps; ++step) {
    const double ceiling = kModalCeilingRatio * crossover;
    const int max_x = static_cast<int>(2.0 * d.length * ceiling / kSoundSpeed);
    const int max_y = static_cast<int>(2.0 * d.width * ceiling / kSoundSpeed);
    const int max_z = static_cast<int>(2.0 * d.height * ceiling / kSoundSpeed);
    std::vector<RoomMode> modes;
    for (int nx = 0; nx <= max_x; ++nx) {
      for (int ny = 0; ny <= max_y; ++ny) {
        for (int nz = 0; nz <= max_z; ++nz) {
          if (nx == 0 && ny == 0 && nz == 0) continue;
          const double f = mode_frequency(d, nx, ny, nz);
          if (f >= ceiling) continue;
          const float mode_rt60 = rt60_at_hz(rt, static_cast<float>(f));
          if (!(mode_rt60 > 0.0f) || !std::isfinite(mode_rt60)) return plan;
          modes.push_back(
              {nx, ny, nz, static_cast<float>(f), static_cast<float>(kLn1000D / mode_rt60)});
        }
      }
    }
    double work = 0.0;
    for (const RoomMode& mode : modes) {
      work += static_cast<double>(mode_samples(mode, length, sample_rate));
    }
    if (modes.size() <= kMaxRoomModes && work <= kMaxModalWork) {
      const bool any_below = std::any_of(modes.begin(), modes.end(),
                                         [&](const RoomMode& m) { return m.freq_hz < crossover; });
      if (!any_below) return plan;
      std::sort(modes.begin(), modes.end(),
                [](const RoomMode& a, const RoomMode& b) { return a.freq_hz < b.freq_hz; });
      plan.crossover_hz = crossover;
      plan.modes = std::move(modes);
      return plan;
    }
    crossover *= kCrossoverStep;
  }
  return plan;
}

double remove_modal_band(std::vector<float>& late, const ModalPlan& plan, int sample_rate) {
  if (plan.modes.empty() || late.empty()) return 0.0;
  std::vector<float> low = late;
  butterworth_zero_phase(low, plan.crossover_hz, sample_rate, kModalSplitOrder, false);
  double energy = 0.0;
  for (std::size_t i = 0; i < late.size(); ++i) {
    energy += static_cast<double>(low[i]) * low[i];
    late[i] -= low[i];
  }
  return energy;
}

float modal_level(const ModalPlan& plan, double band_energy, std::size_t length, int sample_rate) {
  if (plan.modes.empty() || !(band_energy > 0.0)) return 0.0f;
  // Each mode is a sin(w t) e^(-delta t) / w term; its expected energy over positions follows
  // from the shapes' mean square, which cancels against the normalization.
  double reference = 0.0;
  for (const RoomMode& mode : plan.modes) {
    const double w = kTwoPiD * mode.freq_hz;
    const double delta = mode.decay_per_s;
    const double duration =
        static_cast<double>(mode_samples(mode, length, sample_rate)) / sample_rate;
    const double integral =
        (1.0 - std::exp(-2.0 * delta * duration)) / (4.0 * delta * (delta * delta + w * w));
    const double gain = butterworth_zero_phase_gain(mode.freq_hz, plan.crossover_hz, sample_rate,
                                                    kModalSplitOrder, false);
    reference += gain * gain * integral;
  }
  reference *= sample_rate;
  return reference > 0.0 ? static_cast<float>(std::sqrt(band_energy / reference)) : 0.0f;
}

void add_room_modes(std::vector<float>& late, const ModalPlan& plan, const ShoeboxRoom& room,
                    const SourceListener& placement, float level, int sample_rate) {
  if (plan.modes.empty() || late.empty() || !(level > 0.0f)) return;
  const double sr = sample_rate;
  for (const RoomMode& mode : plan.modes) {
    const double w = kTwoPiD * mode.freq_hz;
    const double gain = butterworth_zero_phase_gain(mode.freq_hz, plan.crossover_hz, sample_rate,
                                                    kModalSplitOrder, false);
    const double amplitude = level * gain * mode_shape(mode, room.dims, placement.source) *
                             mode_shape(mode, room.dims, placement.listener) /
                             (shape_mean_square(mode) * w);
    // e^((-delta + j w) t) by complex rotation, so each sample costs a multiply, not a libm call.
    const double r = std::exp(-static_cast<double>(mode.decay_per_s) / sr);
    const double rot_re = r * std::cos(w / sr);
    const double rot_im = r * std::sin(w / sr);
    double re = 1.0;
    double im = 0.0;
    const std::size_t n = mode_samples(mode, late.size(), sample_rate);
    for (std::size_t i = 0; i < n; ++i) {
      late[i] += static_cast<float>(amplitude * im);
      const double next_re = re * rot_re - im * rot_im;
      im = re * rot_im + im * rot_re;
      re = next_re;
    }
  }
}

}  // namespace sonare::acoustic
