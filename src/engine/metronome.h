#pragma once

/// @file metronome.h
/// @brief Tempo-map driven metronome click generator and count-in helper.

#include <array>
#include <cstddef>
#include <cstdint>

#include "transport/tempo_map.h"

namespace sonare::engine {

struct MetronomeConfig {
  bool enabled = false;
  float beat_gain = 0.35f;
  float accent_gain = 0.7f;
  // Optional explicit click length in samples; when > 0 it takes precedence
  // over click_seconds. 0 means "derive from click_seconds and the sample
  // rate" (sample-rate independent).
  int click_samples = 0;
  // Click duration in seconds. The default (2 ms) resolves to the legacy 96
  // samples at 48 kHz when click_samples is 0.
  double click_seconds = 0.002;
};

struct MetronomeEvent {
  int offset = 0;
  bool accent = false;
  int64_t timeline_sample = 0;
};

struct MetronomeEventList {
  static constexpr size_t kCapacity = 128;
  std::array<MetronomeEvent, kCapacity> events{};
  size_t size = 0;
  bool overflowed = false;

  void clear() noexcept;
  bool add(MetronomeEvent event) noexcept;
};

class Metronome {
 public:
  void prepare(double sample_rate, const transport::TempoMap* tempo_map) noexcept;
  void set_config(MetronomeConfig config) noexcept { config_ = config; }
  const MetronomeConfig& config() const noexcept { return config_; }

  void collect_events(int64_t block_start_sample, int num_frames,
                      MetronomeEventList* out) const noexcept;
  void process(float* const* channels, int num_channels, int num_frames,
               int64_t block_start_sample) const noexcept;

  int64_t count_in_end_sample(int64_t start_sample, int bars) const noexcept;

 private:
  double sample_rate_ = 48000.0;
  const transport::TempoMap* tempo_map_ = nullptr;
  MetronomeConfig config_{};
};

}  // namespace sonare::engine
