#pragma once

/// @file spectrum_registry.h
/// @brief Fixed-capacity process-local spectrum registry for EQ collision hints.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "mastering/eq/spectrum_engine.h"

namespace sonare::mastering::eq {

struct SpectrumProfile {
  uint64_t instance_id = 0;
  std::array<float, kSpectrumProfileBands> band_db{};
  uint64_t seq = 0;
  bool active = false;
};

struct SpectrumCollisionBand {
  size_t band = 0;
  float score_db = 0.0f;
};

struct SpectrumCollisionReport {
  std::array<SpectrumCollisionBand, kSpectrumProfileBands> bands{};
  size_t count = 0;
};

class SpectrumRegistry {
 public:
  static SpectrumRegistry& instance() noexcept;

  void publish(const SpectrumProfile& profile) noexcept;
  void remove(uint64_t instance_id) noexcept;
  bool read(uint64_t instance_id, SpectrumProfile& out) const noexcept;
  SpectrumCollisionReport collisions(uint64_t a, uint64_t b,
                                     float threshold_db = -60.0f) const noexcept;
  void reset() noexcept;

 private:
  static constexpr size_t kMaxProfiles = 64;
  struct ProfileSlot {
    std::atomic<uint32_t> guard{0};
    std::atomic<uint64_t> instance_id{0};
    std::array<std::atomic<float>, kSpectrumProfileBands> band_db{};
    std::atomic<uint64_t> seq{0};
    std::atomic<bool> active{false};
  };

  static bool read_slot(const ProfileSlot& slot, SpectrumProfile& out) noexcept;
  static void write_slot(ProfileSlot& slot, const SpectrumProfile& profile) noexcept;

  std::array<ProfileSlot, kMaxProfiles> profiles_{};
};

}  // namespace sonare::mastering::eq
