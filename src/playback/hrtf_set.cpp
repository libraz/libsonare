#include "playback/hrtf_set.h"

#include <utility>

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
  (void)sample_rate;
  return *this;
}

int HrtfSet::sample_rate() const noexcept { return 0; }
int HrtfSet::taps() const noexcept { return 0; }
int HrtfSet::azimuth_count() const noexcept { return 0; }
int HrtfSet::elevation_count() const noexcept { return 0; }
float HrtfSet::azimuth_step_deg() const noexcept { return 0.0f; }
float HrtfSet::elevation_min_deg() const noexcept { return 0.0f; }
float HrtfSet::elevation_step_deg() const noexcept { return 0.0f; }

const float* HrtfSet::hrir(int elevation_index, int azimuth_index, int ear) const noexcept {
  (void)elevation_index;
  (void)azimuth_index;
  (void)ear;
  return nullptr;
}

float HrtfSet::itd_samples(int elevation_index, int azimuth_index) const noexcept {
  (void)elevation_index;
  (void)azimuth_index;
  return 0.0f;
}

void HrtfSet::interpolate(float azimuth_deg, float elevation_deg, float* left, float* right,
                          float* itd_samples) const noexcept {
  (void)azimuth_deg;
  (void)elevation_deg;
  (void)left;
  (void)right;
  if (itd_samples != nullptr) *itd_samples = 0.0f;
}

}  // namespace sonare::playback
