/// @file no_alloc_test_helpers.h
/// @brief Shared helpers for no-allocation realtime tests.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include "automation/automation_engine.h"
#include "engine/capture.h"
#include "engine/clip_player.h"
#ifdef SONARE_WITH_GRAPH
#include "engine/graph_runtime.h"
#include "graph/graph.h"
#endif
#include "engine/meter_telemetry.h"
#include "engine/metronome.h"
#include "engine/mixing_runtime.h"
#include "engine/monitor_runtime.h"
#include "engine/realtime_engine.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/passive_tube.h"
#include "mastering/eq/spectrum_registry.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mixing/bus.h"
#include "mixing/channel_strip.h"
#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_ACOUSTIC_SIM)
#include "acoustic/material.h"
#include "effects/acoustic/room_morph.h"
#include "effects/reverb/room_reverb.h"
#endif
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using sonare::test::allocation_should_fail;
using sonare::test::AllocationGuard;
using sonare::test::note_allocation;
using sonare::test::note_live_allocation;
using sonare::test::note_live_bytes;
using sonare::test::note_live_release;

// Every block carries its requested size just in front of it, so a release subtracts what was
// requested; the allocator's own block can be larger (macOS reuses a cached larger large region).
inline constexpr std::size_t kSizeHeader = alignof(std::max_align_t);

[[maybe_unused]] std::size_t header_bytes(std::size_t alignment) noexcept {
  return std::max(alignment, kSizeHeader);
}

[[maybe_unused]] void* stamp_size(void* base, std::size_t header, std::size_t size) noexcept {
  auto* user = static_cast<unsigned char*>(base) + header;
  std::memcpy(user - sizeof(size), &size, sizeof(size));
  note_live_allocation();
  note_live_bytes(static_cast<long long>(size));
  return user;
}

// Notes the release of @p ptr and returns the block the allocator handed out.
[[maybe_unused]] void* unstamp_size(void* ptr, std::size_t header) noexcept {
  note_live_release(ptr);
  auto* user = static_cast<unsigned char*>(ptr);
  std::size_t size = 0;
  std::memcpy(&size, user - sizeof(size), sizeof(size));
  note_live_bytes(-static_cast<long long>(size));
  return user - header;
}

[[maybe_unused]] void* allocate_bytes(std::size_t size) {
  note_allocation();
  if (allocation_should_fail(size) || size > SIZE_MAX - kSizeHeader) {
    throw std::bad_alloc();
  }
  if (void* base = std::malloc(kSizeHeader + size)) return stamp_size(base, kSizeHeader, size);
  throw std::bad_alloc();
}

[[maybe_unused]] void* allocate_aligned_bytes(std::size_t size, std::size_t alignment) {
  note_allocation();
  const std::size_t header = header_bytes(alignment);
  if (allocation_should_fail(size) || size > SIZE_MAX - header) {
    throw std::bad_alloc();
  }
  void* base = nullptr;
#if defined(_WIN32)
  base = _aligned_malloc(header + size, alignment);
#else
  if (posix_memalign(&base, alignment, header + size) != 0) base = nullptr;
#endif
  if (base != nullptr) return stamp_size(base, header, size);
  throw std::bad_alloc();
}

[[maybe_unused]] void release_bytes(void* ptr) noexcept {
  if (ptr != nullptr) std::free(unstamp_size(ptr, kSizeHeader));
}

// Aligned allocations must be released with the matching deallocator: on Windows
// _aligned_malloc memory requires _aligned_free, whereas posix_memalign memory is
// freed with std::free.
[[maybe_unused]] void aligned_free(void* ptr, std::size_t alignment) noexcept {
  if (ptr == nullptr) return;
  void* base = unstamp_size(ptr, header_bytes(alignment));
#if defined(_WIN32)
  _aligned_free(base);
#else
  std::free(base);
#endif
}

class ScaleProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit ScaleProcessor(float scale) : scale_(scale) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= scale_;
      }
    }
  }
  void reset() override {}
  bool set_parameter_impl(unsigned int param_id, float value) override {
    if (param_id > 64u) return false;
    scale_ = value;
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id <= 64u;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> descriptors;
    descriptors.reserve(65);
    for (unsigned int id = 0; id <= 64u; ++id) {
      descriptors.push_back({"scale" + std::to_string(id), id});
    }
    return descriptors;
  }

 private:
  float scale_ = 1.0f;
};

class ParameterCaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter_impl(unsigned int param_id, float value) override {
    last_param = param_id;
    last_value = value;
    return true;
  }
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id <= 64u;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> descriptors;
    descriptors.reserve(65);
    for (unsigned int id = 0; id <= 64u; ++id) {
      descriptors.push_back({"parameter" + std::to_string(id), id});
    }
    return descriptors;
  }

  unsigned int last_param = 0;
  float last_value = 0.0f;
};

}  // namespace
