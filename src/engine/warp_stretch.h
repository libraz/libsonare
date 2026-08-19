#pragma once

/// @file warp_stretch.h
/// @brief Wait-free WSOLA time stretcher used by the realtime clip player.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::engine {

/// @brief Pulls one source sample for a channel. Returns 0 outside the source.
/// @details A plain function pointer plus context rather than std::function so
///          the audio thread never touches a type-erased allocation.
using WarpSourceReader = float (*)(void* context, int channel, int64_t sample) noexcept;

/// @brief Maps a clip-local output position to the source position the warp map
///        assigns to it. Fractional, because the warp map is piecewise linear.
using WarpPositionMapper = double (*)(void* context, int64_t clip_local_output) noexcept;

/// @brief One clip's WSOLA state: pitch-preserving playback of a warp map.
///
/// @details The clip player resolves a source position per output sample and
/// reads it directly, which resamples and therefore moves the pitch with the
/// rate (@c WarpMode::kRepitch). This class keeps the same position mapping but
/// synthesizes the output by overlap-adding source segments at a FIXED
/// synthesis hop, so the local period — and with it the pitch — is preserved
/// while the playback rate follows the map.
///
/// Segment placement is WSOLA: each frame is searched within a window around
/// the mapped position for the offset whose start best continues the previously
/// emitted frame, which keeps waveform periods aligned across the overlap and
/// avoids the phase cancellation plain OLA produces.
///
/// All buffers are sized in @ref prepare and never resized afterwards, and the
/// per-block path performs no allocation, locking, or system call.
class WarpStretchVoice {
 public:
  /// Analysis / synthesis window length in samples. Long enough to hold two
  /// periods of a low male voice at 48 kHz, short enough to keep the similarity
  /// search inside a few thousand multiply-accumulates per frame.
  static constexpr int kFrameSize = 1024;
  /// Synthesis hop. Half the frame, so a periodic Hann window sums to exactly
  /// one across the overlap and no output normalization pass is needed.
  static constexpr int kSynthesisHop = kFrameSize / 2;
  /// Similarity search radius around the mapped position, in samples. Half a
  /// frame is enough to realign any period the window can resolve.
  static constexpr int kSearchRadius = kFrameSize / 2;
  /// Stride of the coarse search pass. The refine pass then scans +/- this
  /// around the coarse winner, so the result matches an exhaustive scan
  /// whenever the correlation is locally unimodal (it is, around a period peak)
  /// at a quarter of the cost.
  static constexpr int kCoarseStride = 4;
  static constexpr int kMaxChannels = 2;

  /// @brief Allocates every buffer. Control thread only.
  void prepare(int max_block_size, int channels);
  /// @brief Drops the accumulated overlap and forgets the stream position.
  void reset() noexcept;

  uint32_t clip_id() const noexcept { return clip_id_; }
  bool active() const noexcept { return active_; }
  /// Blocks since this voice last produced output, used to pick an eviction
  /// candidate when every voice is taken.
  uint32_t idle_blocks() const noexcept { return idle_blocks_; }
  void mark_idle() noexcept;

  /// @brief Renders @p count output samples starting at clip-local position
  ///        @p output_start into @p out.
  /// @details Restarts the stream when @p clip_id or @p output_start does not
  ///          continue the previous call, which is what makes a seek, a loop
  ///          wrap, or a re-used voice sound like a fresh start rather than a
  ///          smear of the previous position.
  /// @param out Per-channel destination pointers; entries may be null.
  /// @param channels Number of destination channels (<= @ref kMaxChannels used
  ///        for the internal state; extra channels repeat the last one).
  /// @return false when the voice has not been prepared, so the caller can fall
  ///         back to the resampling path instead of emitting silence.
  bool render(uint32_t clip_id, int64_t output_start, int count, float** out, int channels,
              WarpSourceReader reader, WarpPositionMapper mapper, void* context) noexcept;

 private:
  void synthesize_frame(int offset, WarpSourceReader reader, WarpPositionMapper mapper,
                        void* context) noexcept;
  int best_offset(int64_t desired, WarpSourceReader reader, void* context) noexcept;

  std::vector<float> window_;                             // kFrameSize
  std::array<std::vector<float>, kMaxChannels> overlap_;  // accumulator
  std::vector<float> match_;                              // kSynthesisHop template
  std::vector<float> search_;                             // search scratch (channel 0)
  int capacity_ = 0;
  int channels_ = 0;
  uint32_t clip_id_ = 0;
  bool active_ = false;
  bool have_previous_ = false;
  uint32_t idle_blocks_ = 0;
  int64_t next_output_ = 0;    // clip-local output position of overlap_[*][0]
  int next_frame_offset_ = 0;  // where the next frame lands inside overlap_
  int filled_ = 0;             // valid samples in overlap_
};

}  // namespace sonare::engine
