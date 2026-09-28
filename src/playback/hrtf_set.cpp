#include "playback/hrtf_set.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "core/resample.h"
#include "playback/default_hrtf.h"

namespace sonare::playback {

HrtfSet::HrtfSet(ShrfData data) : data_(std::move(data)) {}

HrtfSet HrtfSet::from_memory(const uint8_t* data, size_t size) {
  return HrtfSet(parse_shrf(data, size));
}

#ifndef __EMSCRIPTEN__
HrtfSet HrtfSet::builtin_default() { return from_memory(kDefaultHrtf, kDefaultHrtfSize); }
#endif

HrtfSet HrtfSet::resampled(int sample_rate) const {
  if (sample_rate == static_cast<int>(data_.header.sample_rate)) return *this;

  const int src_sr = static_cast<int>(data_.header.sample_rate);
  const double ratio = static_cast<double>(sample_rate) / static_cast<double>(src_sr);
  const size_t direction_count =
      static_cast<size_t>(data_.header.n_el) * static_cast<size_t>(data_.header.n_az);
  const int old_taps = data_.header.taps;

  ShrfData result;
  result.header = data_.header;
  result.header.sample_rate = static_cast<uint32_t>(sample_rate);
  // The HRIRs are already dequantized to float, so a resampled copy carries no scale.
  result.header.quant = ShrfQuant::Float32;
  result.header.scale = 1.0f;

  result.itd.resize(data_.itd.size());
  for (size_t i = 0; i < data_.itd.size(); ++i) {
    result.itd[i] = static_cast<float>(static_cast<double>(data_.itd[i]) * ratio);
  }

  int new_taps = 0;
  for (size_t direction = 0; direction < direction_count; ++direction) {
    for (int ear = 0; ear < 2; ++ear) {
      const size_t old_offset =
          (direction * 2 + static_cast<size_t>(ear)) * static_cast<size_t>(old_taps);
      const std::vector<float> channel =
          resample(&data_.hrir[old_offset], static_cast<size_t>(old_taps), src_sr, sample_rate);
      if (new_taps == 0) {
        new_taps = static_cast<int>(channel.size());
        result.header.taps = new_taps;
        result.hrir.assign(direction_count * 2 * static_cast<size_t>(new_taps), 0.0f);
      }
      const size_t new_offset =
          (direction * 2 + static_cast<size_t>(ear)) * static_cast<size_t>(new_taps);
      std::copy(channel.begin(), channel.end(),
                result.hrir.begin() + static_cast<std::ptrdiff_t>(new_offset));
    }
  }

  return HrtfSet(std::move(result));
}

int HrtfSet::sample_rate() const noexcept { return static_cast<int>(data_.header.sample_rate); }
int HrtfSet::taps() const noexcept { return data_.header.taps; }
int HrtfSet::azimuth_count() const noexcept { return data_.header.n_az; }
int HrtfSet::elevation_count() const noexcept { return data_.header.n_el; }
float HrtfSet::azimuth_step_deg() const noexcept { return data_.header.az_step_deg; }
float HrtfSet::elevation_min_deg() const noexcept { return data_.header.el_min_deg; }
float HrtfSet::elevation_step_deg() const noexcept { return data_.header.el_step_deg; }

const float* HrtfSet::hrir(int elevation_index, int azimuth_index, int ear) const noexcept {
  const size_t direction =
      static_cast<size_t>(elevation_index) * static_cast<size_t>(data_.header.n_az) +
      static_cast<size_t>(azimuth_index);
  const size_t offset =
      (direction * 2 + static_cast<size_t>(ear)) * static_cast<size_t>(data_.header.taps);
  return &data_.hrir[offset];
}

float HrtfSet::itd_samples(int elevation_index, int azimuth_index) const noexcept {
  const size_t index =
      static_cast<size_t>(elevation_index) * static_cast<size_t>(data_.header.n_az) +
      static_cast<size_t>(azimuth_index);
  return data_.itd[index];
}

void HrtfSet::interpolate(float azimuth_deg, float elevation_deg, float* left, float* right,
                          float* itd_samples) const noexcept {
  const int n_az = data_.header.n_az;
  const int n_el = data_.header.n_el;
  const int taps = data_.header.taps;
  const float az_step = data_.header.az_step_deg;

  // Azimuth wraps around the full circle covered by the n_az columns.
  float wrapped_az = std::fmod(azimuth_deg, 360.0f);
  if (wrapped_az < 0.0f) wrapped_az += 360.0f;
  const float az_units = wrapped_az / az_step;
  const float az_floor = std::floor(az_units);
  int az_low = static_cast<int>(az_floor) % n_az;
  if (az_low < 0) az_low += n_az;
  const int az_high = (az_low + 1) % n_az;
  const float az_frac = az_units - az_floor;

  // Elevation outside the grid clamps to the nearest row instead of extrapolating.
  const float el_min = data_.header.el_min_deg;
  const float el_step = data_.header.el_step_deg;
  const float el_max = el_min + el_step * static_cast<float>(n_el - 1);
  int el_low = 0;
  int el_high = 0;
  float el_frac = 0.0f;
  if (n_el > 1 && elevation_deg > el_min && elevation_deg < el_max) {
    const float el_units = (elevation_deg - el_min) / el_step;
    const float el_floor = std::floor(el_units);
    el_low = static_cast<int>(el_floor);
    el_high = el_low + 1;
    el_frac = el_units - el_floor;
  } else if (elevation_deg >= el_max) {
    el_low = el_high = n_el - 1;
  }

  const float w00 = (1.0f - az_frac) * (1.0f - el_frac);
  const float w10 = az_frac * (1.0f - el_frac);
  const float w01 = (1.0f - az_frac) * el_frac;
  const float w11 = az_frac * el_frac;

  const float* h00l = hrir(el_low, az_low, 0);
  const float* h00r = hrir(el_low, az_low, 1);
  const float* h10l = hrir(el_low, az_high, 0);
  const float* h10r = hrir(el_low, az_high, 1);
  const float* h01l = hrir(el_high, az_low, 0);
  const float* h01r = hrir(el_high, az_low, 1);
  const float* h11l = hrir(el_high, az_high, 0);
  const float* h11r = hrir(el_high, az_high, 1);

  for (int t = 0; t < taps; ++t) {
    left[t] = w00 * h00l[t] + w10 * h10l[t] + w01 * h01l[t] + w11 * h11l[t];
    right[t] = w00 * h00r[t] + w10 * h10r[t] + w01 * h01r[t] + w11 * h11r[t];
  }

  if (itd_samples != nullptr) {
    *itd_samples =
        w00 * this->itd_samples(el_low, az_low) + w10 * this->itd_samples(el_low, az_high) +
        w01 * this->itd_samples(el_high, az_low) + w11 * this->itd_samples(el_high, az_high);
  }
}

}  // namespace sonare::playback
