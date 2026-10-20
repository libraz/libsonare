#pragma once

/// @file metronome.h
/// @brief Tempo-map driven metronome click generator and count-in helper.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "transport/tempo_map.h"
#include "util/constants.h"

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

/// Public/configuration limits. One second is already much longer than a
/// musical click; the sample limit also bounds callers that configure the
/// metronome before a sample rate is prepared.
inline constexpr double kMaxMetronomeClickSeconds = 1.0;
inline constexpr int kMaxMetronomeClickSamples = 384000;

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
  void set_tempo_map(const transport::TempoMap* tempo_map) noexcept { tempo_map_ = tempo_map; }
  void set_config(MetronomeConfig config) noexcept;
  const MetronomeConfig& config() const noexcept { return config_; }

  // Collects beat events whose click could sound inside this block. Beats are
  // included from `block_start_sample - lookback_samples` up to the block end so
  // that a click which began in a previous sub-block still has its tail rendered
  // here (such events have a negative offset). lookback_samples == 0 reproduces
  // the plain in-block behavior.
  void collect_events(int64_t block_start_sample, int num_frames, MetronomeEventList* out,
                      int lookback_samples = 0) const noexcept;
  /// Renders the click and returns false when the fixed event list overflowed. A
  /// block that does not continue the previous one (a start, seek or loop wrap)
  /// begins a new segment, and only a click started inside the current segment
  /// sounds, so a jump never lands in the middle of an earlier beat's decay.
  bool process(float* const* channels, int num_channels, int num_frames,
               int64_t block_start_sample) noexcept;

  /// Sample at which a count-in of @p bars bars ends, measured on this metronome's tempo map.
  int64_t count_in_end_sample(int64_t start_sample, int bars) const noexcept;

  /// Count-in end on @p map: the bar containing @p start_sample is counted from its own
  /// start, so a mid-bar start gets a short first bar. Every counted bar has the length of the
  /// time signature in effect at @p start_sample; later signature changes do not alter it, and
  /// tempo changes apply through the map. Returns @p start_sample when @p bars <= 0.
  static int64_t count_in_end_sample(const transport::TempoMap& map, int64_t start_sample,
                                     int bars) noexcept;

 private:
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  const transport::TempoMap* tempo_map_ = nullptr;
  MetronomeConfig config_{};
  // Start of the continuous run the last block belonged to, and where it ended.
  int64_t segment_start_sample_ = 0;
  int64_t next_block_start_sample_ = std::numeric_limits<int64_t>::min();
};

}  // namespace sonare::engine
