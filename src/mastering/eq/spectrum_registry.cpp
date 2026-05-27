#include "mastering/eq/spectrum_registry.h"

#include <algorithm>
#include <atomic>

namespace sonare::mastering::eq {

void SpectrumRegistry::write_slot(ProfileSlot& slot, const SpectrumProfile& profile) noexcept {
  const uint32_t guard = slot.guard.load(std::memory_order_relaxed);
  slot.guard.store(guard + 1U, std::memory_order_release);
  // Publish the odd guard before mutating payload fields so readers observe it.
  std::atomic_thread_fence(std::memory_order_release);
  slot.instance_id.store(profile.instance_id, std::memory_order_relaxed);
  for (size_t band = 0; band < kSpectrumProfileBands; ++band) {
    slot.band_db[band].store(profile.band_db[band], std::memory_order_relaxed);
  }
  slot.seq.store(profile.seq, std::memory_order_relaxed);
  slot.active.store(profile.active, std::memory_order_relaxed);
  slot.guard.store(guard + 2U, std::memory_order_release);
}

SpectrumRegistry& SpectrumRegistry::instance() noexcept {
  static SpectrumRegistry registry;
  return registry;
}

bool SpectrumRegistry::read_slot(const ProfileSlot& slot, SpectrumProfile& out) noexcept {
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint32_t before = slot.guard.load(std::memory_order_acquire);
    // Spin while a write is in progress without consuming the retry budget.
    while ((before & 1U) != 0U) {
      before = slot.guard.load(std::memory_order_acquire);
    }
    SpectrumProfile copy;
    copy.instance_id = slot.instance_id.load(std::memory_order_relaxed);
    for (size_t band = 0; band < kSpectrumProfileBands; ++band) {
      copy.band_db[band] = slot.band_db[band].load(std::memory_order_relaxed);
    }
    copy.seq = slot.seq.load(std::memory_order_relaxed);
    copy.active = slot.active.load(std::memory_order_relaxed);
    // Ensure the payload reads complete before observing the trailing guard.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t after = slot.guard.load(std::memory_order_acquire);
    if (before == after && (after & 1U) == 0U) {
      out = copy;
      return true;
    }
  }
  return false;
}

void SpectrumRegistry::publish(const SpectrumProfile& profile) noexcept {
  if (profile.instance_id == 0) {
    return;
  }
  size_t target = profiles_.size();
  for (size_t i = 0; i < profiles_.size(); ++i) {
    SpectrumProfile current;
    const bool readable = read_slot(profiles_[i], current);
    if (readable && current.active && current.instance_id == profile.instance_id) {
      target = i;
      break;
    }
    if (readable && !current.active && target == profiles_.size()) {
      target = i;
    }
  }
  if (target < profiles_.size()) {
    SpectrumProfile stored = profile;
    stored.active = true;
    write_slot(profiles_[target], stored);
  }
}

void SpectrumRegistry::remove(uint64_t instance_id) noexcept {
  for (size_t i = 0; i < profiles_.size(); ++i) {
    SpectrumProfile current;
    if (read_slot(profiles_[i], current) && current.active && current.instance_id == instance_id) {
      write_slot(profiles_[i], {});
      return;
    }
  }
}

bool SpectrumRegistry::read(uint64_t instance_id, SpectrumProfile& out) const noexcept {
  for (size_t i = 0; i < profiles_.size(); ++i) {
    SpectrumProfile copy;
    if (read_slot(profiles_[i], copy) && copy.active && copy.instance_id == instance_id) {
      out = copy;
      return true;
    }
  }
  return false;
}

SpectrumCollisionReport SpectrumRegistry::collisions(uint64_t a, uint64_t b,
                                                     float threshold_db) const noexcept {
  SpectrumCollisionReport report{};
  SpectrumProfile pa;
  SpectrumProfile pb;
  if (!read(a, pa) || !read(b, pb)) {
    return report;
  }
  for (size_t i = 0; i < kSpectrumProfileBands; ++i) {
    if (pa.band_db[i] <= threshold_db) {
      continue;
    }
    float peer_db = threshold_db;
    const size_t begin = i == 0 ? 0 : i - 1;
    const size_t end = std::min(kSpectrumProfileBands - 1, i + 1);
    for (size_t j = begin; j <= end; ++j) {
      peer_db = std::max(peer_db, pb.band_db[j]);
    }
    if (peer_db > threshold_db) {
      report.bands[report.count++] = {i, std::min(pa.band_db[i], peer_db)};
    }
  }
  return report;
}

void SpectrumRegistry::reset() noexcept {
  for (size_t i = 0; i < profiles_.size(); ++i) {
    write_slot(profiles_[i], {});
  }
}

}  // namespace sonare::mastering::eq
