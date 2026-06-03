#pragma once

/// @file boundary_splitter.h
/// @brief Fixed-capacity boundary merge for realtime render sub-blocks.

#include <array>
#include <cstddef>
#include <cstdint>

namespace sonare::engine {

enum class BoundarySource : uint32_t {
  kBlockStart = 1u << 0u,
  kBlockEnd = 1u << 1u,
  kLoop = 1u << 2u,
  kCommand = 1u << 3u,
  kAutomation = 1u << 4u,
  kClip = 1u << 5u,
  kMarker = 1u << 6u,
  kMidi = 1u << 7u,
};

constexpr uint32_t boundary_source_mask(BoundarySource source) noexcept {
  return static_cast<uint32_t>(source);
}

struct BoundaryPoint {
  int offset = 0;
  int64_t render_frame = 0;
  int64_t timeline_sample = 0;
  uint32_t sources = 0;
};

struct BoundaryBuildContext {
  int64_t block_render_frame = 0;
  int64_t block_timeline_sample = 0;
  int num_frames = 0;
  bool loop_wrap = false;
  // Sample offset of the FIRST loop wrap inside this block. Subsequent wraps
  // (short loop + large block) recur every loop_len_samples after this offset.
  int loop_wrap_offset = 0;
  int64_t loop_start_timeline_sample = 0;
  // Loop length in samples. When > 0 and loop_wrap is set, timeline_at_offset
  // folds any offset past the first wrap back into [loop_start, loop_start +
  // loop_len), so the over-wrapped tail of the block maps to the correct
  // timeline position even across multiple wraps. 0 means single-wrap behavior.
  int64_t loop_len_samples = 0;
};

class BoundaryList {
 public:
  static constexpr size_t kCapacity = 32;

  void clear() noexcept;
  bool add_offset(int offset, BoundarySource source, const BoundaryBuildContext& context) noexcept;
  bool add_point(BoundaryPoint point) noexcept;
  void sort_unique() noexcept;
  bool finalize(const BoundaryBuildContext& context) noexcept;

  size_t size() const noexcept { return size_; }
  const BoundaryPoint& operator[](size_t index) const noexcept { return points_[index]; }
  bool overflowed() const noexcept { return overflowed_; }
  uint32_t dropped_count() const noexcept { return dropped_count_; }

 private:
  static int64_t timeline_at_offset(int offset, const BoundaryBuildContext& context) noexcept;
  bool append(BoundaryPoint point) noexcept;
  bool ensure_block_start(const BoundaryBuildContext& context) noexcept;
  bool ensure_block_end(const BoundaryBuildContext& context) noexcept;

  std::array<BoundaryPoint, kCapacity> points_{};
  size_t size_ = 0;
  bool overflowed_ = false;
  uint32_t dropped_count_ = 0;
};

class BoundarySplitter {
 public:
  void begin(BoundaryBuildContext context) noexcept;
  bool add_loop(int offset) noexcept;
  bool add_command(int offset) noexcept;
  bool add_automation(int offset) noexcept;
  bool add_clip(int offset) noexcept;
  bool add_marker(int offset) noexcept;
  bool add_midi(int offset) noexcept;
  const BoundaryList& finish() noexcept;

 private:
  BoundaryBuildContext context_{};
  BoundaryList boundaries_{};
};

}  // namespace sonare::engine
