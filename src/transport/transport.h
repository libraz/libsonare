#pragma once

/// @file transport.h
/// @brief Sample-accurate playback clock for render and musical timelines.

#include <array>
#include <atomic>
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
  void clear() noexcept {
    size_ = 0;
    overflowed_ = false;
  }
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
  // Consistent snapshot of the loop region. set_loop (control thread) publishes
  // all three fields atomically via the seqlock guard below; the audio thread
  // (advance / collect_loop_boundaries / snapshot) reads a torn-free copy.
  struct LoopState {
    double start_ppq = 0.0;
    double end_ppq = 0.0;
    bool enabled = false;
  };

  // Seqlock: an odd guard means a write is in progress. The audio-thread reader
  // does a single non-spinning attempt (read_loop_state); on a detected
  // conflict (writer mid-update or torn read) it returns the last consistent
  // value it cached instead of spinning up to a scheduler tick, which would
  // risk an xrun if the control thread were preempted mid-write. The control
  // thread alone calls write_loop_state. Mirrors mixing/meter.cpp's try-read.
  LoopState read_loop_state() const noexcept;
  void write_loop_state(const LoopState& state) noexcept;

  const TempoMap* tempo_map_ = nullptr;
  double sample_rate_ = 48000.0;
  bool playing_ = false;
  int64_t render_frame_ = 0;
  int64_t sample_position_ = 0;
  LoopState loop_state_{};
  mutable std::atomic<uint32_t> loop_guard_{0};
  // Last torn-free LoopState observed by read_loop_state(). Touched only on the
  // audio-thread read path, so no synchronization is needed; serves as the
  // fallback when a single try-read races a control-thread write.
  mutable LoopState cached_loop_state_{};
};

}  // namespace sonare::transport
