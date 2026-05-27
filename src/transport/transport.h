#pragma once

/// @file transport.h
/// @brief Sample-accurate playback clock for render and musical timelines.

#include <array>
#include <cstdint>

#include "transport/marker.h"
#include "transport/tempo_map.h"
#include "transport/transport_state.h"

namespace sonare::transport {

struct Boundary {
  int offset = 0;
  int64_t render_frame = 0;
  int64_t timeline_sample = 0;
};

class BoundaryList {
 public:
  static constexpr size_t kCapacity = 16;

  bool add(Boundary boundary) noexcept;
  size_t size() const noexcept { return size_; }
  const Boundary& operator[](size_t index) const noexcept { return items_[index]; }
  void clear() noexcept { size_ = 0; }
  bool overflowed() const noexcept { return overflowed_; }

 private:
  std::array<Boundary, kCapacity> items_{};
  size_t size_ = 0;
  bool overflowed_ = false;
};

class Transport {
 public:
  void prepare(double sample_rate, const TempoMap* tempo_map);
  TransportState snapshot() const noexcept;
  void advance(int num_frames) noexcept;

  void play() noexcept { playing_ = true; }
  void stop() noexcept { playing_ = false; }
  void seek_sample(int64_t sample) noexcept;
  void seek_ppq(double ppq) noexcept;
  void set_loop(double start_ppq, double end_ppq, bool enabled) noexcept;
  bool seek_marker(uint32_t marker_id, const MarkerMap& markers) noexcept;
  bool set_loop_from_markers(uint32_t start_marker_id, uint32_t end_marker_id,
                             const MarkerMap& markers) noexcept;
  bool collect_loop_boundaries(int num_frames, BoundaryList* out) const noexcept;

  int64_t render_frame() const noexcept { return render_frame_; }
  int64_t sample_position() const noexcept { return sample_position_; }

 private:
  const TempoMap* tempo_map_ = nullptr;
  double sample_rate_ = 48000.0;
  bool playing_ = false;
  bool looping_ = false;
  int64_t render_frame_ = 0;
  int64_t sample_position_ = 0;
  double loop_start_ppq_ = 0.0;
  double loop_end_ppq_ = 0.0;
};

}  // namespace sonare::transport
