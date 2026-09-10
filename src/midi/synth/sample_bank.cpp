#include "midi/synth/sample_bank.h"

#include <algorithm>

namespace sonare::midi::synth {

bool SampleBank::add_sample(const float* data, size_t n_frames, const SampleDesc& desc,
                            uint32_t* out_index) {
  if (data == nullptr || n_frames == 0) return false;

  const uint32_t start = static_cast<uint32_t>(pool_.size());
  pool_.insert(pool_.end(), data, data + n_frames);

  Sample s;
  s.region.start = start;
  s.region.end = static_cast<uint32_t>(pool_.size());
  // Loops are clamped into the sample, and a loop mode that survives the clamp
  // as an empty span is dropped — SampleReader reads both taps unchecked.
  const uint32_t loop_start = start + std::min(desc.loop_start, static_cast<uint32_t>(n_frames));
  const uint32_t loop_end = start + std::min(desc.loop_end, static_cast<uint32_t>(n_frames));
  s.region.loop_start = loop_start;
  s.region.loop_end = std::max(loop_start, loop_end);
  s.region.loop_mode =
      (desc.loop_mode == 1 || desc.loop_mode == 3) && s.region.loop_end > s.region.loop_start
          ? desc.loop_mode
          : 0;
  s.root_key = static_cast<uint8_t>(std::min<int>(desc.root_key, 127));
  s.fine_tune_cents = desc.fine_tune_cents;
  s.source_rate = desc.source_rate > 0.0 ? desc.source_rate : 0.0;
  samples_.push_back(s);

  if (out_index != nullptr) *out_index = static_cast<uint32_t>(samples_.size() - 1);
  return true;
}

bool SampleBank::add_zone(uint32_t set, const SampleZoneDesc& zone) {
  if (zone.sample_index >= samples_.size()) return false;
  if (zone.key_lo > zone.key_hi || zone.vel_lo > zone.vel_hi) return false;

  if (set >= sets_.size()) sets_.resize(static_cast<size_t>(set) + 1);

  const Sample& s = samples_[zone.sample_index];
  SampleZone z;
  z.region = s.region;
  z.key_lo = zone.key_lo;
  z.key_hi = zone.key_hi;
  z.vel_lo = zone.vel_lo;
  z.vel_hi = zone.vel_hi;
  z.root_key = s.root_key;
  z.tune_cents = s.fine_tune_cents + zone.tune_cents;
  z.gain = zone.gain;
  z.pan_units = zone.pan_units;
  z.source_rate = s.source_rate;
  sets_[set].push_back(z);
  return true;
}

const SampleZone* SampleBank::find(int32_t set, uint8_t key, uint8_t velocity) const noexcept {
  if (set < 0 || static_cast<size_t>(set) >= sets_.size()) return nullptr;
  for (const SampleZone& z : sets_[static_cast<size_t>(set)]) {
    if (key >= z.key_lo && key <= z.key_hi && velocity >= z.vel_lo && velocity <= z.vel_hi) {
      return &z;
    }
  }
  return nullptr;
}

SampleZoneMix SampleBank::find_mix(int32_t set, uint8_t key, uint8_t velocity) const noexcept {
  SampleZoneMix out;
  if (set < 0 || static_cast<size_t>(set) >= sets_.size()) return out;
  const SampleZone* first = nullptr;
  const SampleZone* second = nullptr;
  for (const SampleZone& z : sets_[static_cast<size_t>(set)]) {
    if (key < z.key_lo || key > z.key_hi || velocity < z.vel_lo || velocity > z.vel_hi) continue;
    if (first == nullptr) {
      first = &z;
    } else {
      second = &z;
      break;
    }
  }
  out.low = first;
  if (first == nullptr || second == nullptr) return out;

  // The zone with the higher floor is the one velocity rises INTO.
  const bool first_is_low = first->vel_lo <= second->vel_lo;
  out.low = first_is_low ? first : second;
  out.high = first_is_low ? second : first;
  const int lo = std::max(out.low->vel_lo, out.high->vel_lo);
  const int hi = std::min(out.low->vel_hi, out.high->vel_hi);
  // Linear rather than equal-power: velocity layers of one instrument are
  // correlated enough that an equal-power law bulges through the middle.
  // A one-value overlap has no interval to travel, so it sits at the midpoint.
  out.high_weight =
      hi > lo ? static_cast<float>(velocity - lo) / static_cast<float>(hi - lo) : 0.5f;
  return out;
}

}  // namespace sonare::midi::synth
