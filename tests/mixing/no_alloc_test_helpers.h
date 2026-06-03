/// @file no_alloc_test_helpers.h
/// @brief Shared helpers for no-allocation realtime tests.

#pragma once

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
#include <new>
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
#include "mastering/eq/pultec.h"
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

using sonare::test::AllocationGuard;
using sonare::test::note_allocation;

[[maybe_unused]] void* allocate_bytes(std::size_t size) {
  note_allocation();
  if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

[[maybe_unused]] void* allocate_aligned_bytes(std::size_t size, std::size_t alignment) {
  note_allocation();
  void* ptr = nullptr;
  const std::size_t actual_size = size == 0 ? 1 : size;
#if defined(_WIN32)
  ptr = _aligned_malloc(actual_size, alignment);
  if (ptr != nullptr) {
    return ptr;
  }
#else
  if (posix_memalign(&ptr, alignment, actual_size) != 0) {
    ptr = nullptr;
  }
  if (ptr != nullptr) {
    return ptr;
  }
#endif
  throw std::bad_alloc();
}

// Aligned allocations must be released with the matching deallocator: on Windows
// _aligned_malloc memory requires _aligned_free, whereas posix_memalign memory is
// freed with std::free.
[[maybe_unused]] void aligned_free(void* ptr) noexcept {
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  std::free(ptr);
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

 private:
  float scale_ = 1.0f;
};

class ParameterCaptureProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter(unsigned int param_id, float value) override {
    last_param = param_id;
    last_value = value;
    return true;
  }

  unsigned int last_param = 0;
  float last_value = 0.0f;
};

}  // namespace
