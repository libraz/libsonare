#pragma once

/// @file transport.h
/// @brief Sample-accurate playback clock for render and musical timelines.

#include <array>
#include <atomic>
#include <cstdint>

#include "rt/seqlock_cell.h"
#include "transport/marker.h"
#include "transport/tempo_map.h"
#include "transport/transport_state.h"
#include "util/constants.h"

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
  // Whether the transport is currently rolling. Read on the audio thread per
  // sub-block to gate sequenced playback (a stopped transport produces no clip /
  // MIDI output and re-dispatches nothing while the playhead is frozen).
  bool playing() const noexcept { return playing_; }

  // Number of process blocks in which collect_loop_boundaries() dropped one or
  // more loop wraps because the BoundaryList filled to kCapacity. Mirrors
  // AutomationEngine::boundary_overflow_count() so a too-short loop relative to
  // the block size (more than kCapacity wraps per block) is observable rather
  // than silently truncated. Relaxed is sufficient: it is a diagnostic stamp.
  uint32_t loop_overflow_count() const noexcept {
    return loop_overflow_count_.load(std::memory_order_relaxed);
  }

 private:
  // Consistent snapshot of the loop region. set_loop (control thread) publishes
  // all three fields atomically via the seqlock cell below; the audio thread
  // (advance / collect_loop_boundaries / snapshot) reads a torn-free copy via
  // the cell's single non-spinning try_load() (RT-safe; on a detected conflict
  // it returns the last cached value instead of spinning up to a scheduler
  // tick, which would risk an xrun if the control thread were preempted
  // mid-write). The control thread alone calls store().
  struct LoopState {
    double start_ppq = 0.0;
    double end_ppq = 0.0;
    bool enabled = false;
  };

  const TempoMap* tempo_map_ = nullptr;
  double sample_rate_ = constants::kDefaultDawSampleRate;
  bool playing_ = false;
  int64_t render_frame_ = 0;
  int64_t sample_position_ = 0;
  rt::SeqlockCell<LoopState> loop_state_{};
  // Diagnostic overflow counter (see loop_overflow_count()). Incremented on the
  // audio thread from the const collect_loop_boundaries(), hence mutable atomic.
  mutable std::atomic<uint32_t> loop_overflow_count_{0};
};

}  // namespace sonare::transport
