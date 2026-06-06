#pragma once

/// @file clip_player.h
/// @brief Timeline sample-accurate audio clip player.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "rt/processor_base.h"
#include "rt/rt_publisher.h"
#include "transport/tempo_map.h"

namespace sonare::engine {

/// Fade-curve law applied to clip fade-in / fade-out regions.
enum class FadeCurve {
  /// Linear-amplitude ramp (default; preserves existing golden output and
  /// dips ~-3 dB at the midpoint of equal-gain crossfades).
  Linear,
  /// Equal-power (constant-energy) ramp using a sine/cosine law; holds a
  /// constant -3 dB sum across symmetric crossfades.
  EqualPower,
  /// Slow start, fast finish (x^2).
  Exponential,
  /// Fast start, slow finish (sqrt(x)).
  Logarithmic,
};

struct ClipAudioBuffer {
  const float* const* channels = nullptr;
  int num_channels = 0;
  int64_t num_samples = 0;
};

struct ClipAudioStorage {
  std::vector<std::vector<float>> channels;
  std::vector<const float*> channel_ptrs;
};

/// Realtime clip source backed by externally supplied pages. Implementations
/// must be non-blocking and allocation-free on sample_at(); returning false is
/// a page miss and the player renders silence for that sample.
class ClipPagedAudioProvider {
 public:
  virtual ~ClipPagedAudioProvider() = default;
  virtual int num_channels() const noexcept = 0;
  virtual int64_t num_samples() const noexcept = 0;
  virtual bool sample_at(int channel, int64_t sample, float* out) const noexcept = 0;
};

struct ClipPageRequest {
  uint32_t clip_id = 0;
  uint32_t channel = 0;
  int64_t sample = 0;
};

static_assert(std::is_trivially_copyable_v<ClipPageRequest>,
              "ClipPageRequest must stay trivially copyable for lock-free queues");

class ClipPageRequestSink {
 public:
  virtual ~ClipPageRequestSink() = default;
  virtual void on_clip_page_miss(const ClipPageRequest& request) noexcept = 0;
};

enum class WarpMode : uint32_t {
  kOff = 0,
  kRepitch = 1,
  kTempoSync = 2,
};

struct WarpAnchor {
  double warp_sample = 0.0;
  double source_sample = 0.0;
};

struct ClipSchedule {
  ClipSchedule() = default;
  ClipSchedule(uint32_t clip_id, ClipAudioBuffer clip_buffer, double clip_start_ppq,
               int64_t clip_start_sample, int64_t clip_offset, int64_t clip_length, bool clip_loop,
               float clip_gain, int64_t clip_fade_in, int64_t clip_fade_out,
               FadeCurve clip_fade_curve = FadeCurve::Linear,
               FadeCurve clip_fade_out_curve = FadeCurve::Linear,
               bool clip_has_separate_fade_out_curve = false)
      : id(clip_id),
        buffer(clip_buffer),
        start_ppq(clip_start_ppq),
        start_sample(clip_start_sample),
        clip_offset_samples(clip_offset),
        length_samples(clip_length),
        loop(clip_loop),
        gain(clip_gain),
        fade_in_samples(clip_fade_in),
        fade_out_samples(clip_fade_out),
        fade_curve(clip_fade_curve),
        fade_in_curve(clip_fade_curve),
        fade_out_curve(clip_has_separate_fade_out_curve ? clip_fade_out_curve : clip_fade_curve) {}

  uint32_t id = 0;
  ClipAudioBuffer buffer{};
  double start_ppq = 0.0;
  int64_t start_sample = 0;
  int64_t clip_offset_samples = 0;
  int64_t length_samples = 0;
  bool loop = false;
  int64_t loop_length_samples = 0;
  /// Control-plane warp reference id carried from the arrangement clip. The
  /// player never dereferences project/model state on the audio thread; RT warp
  /// playback uses the immutable anchor snapshot below.
  uint32_t warp_ref_id = 0;
  WarpMode warp_mode = WarpMode::kOff;
  int64_t warp_reference_offset_samples = 0;
  std::shared_ptr<const std::vector<WarpAnchor>> warp_anchors;
  /// Control-plane source-track id carried from the arrangement clip. The clip
  /// player never reads it on the audio thread; the offline bounce uses it to
  /// group clips into per-track stems for channel-strip mixing. 0 = unset.
  uint32_t track_id = 0;
  float gain = 1.0f;
  int64_t fade_in_samples = 0;
  int64_t fade_out_samples = 0;
  /// Optional whole-clip fade domain for schedule fragments (for example comp
  /// segments). 0 length means use this schedule's own length.
  int64_t fade_reference_offset_samples = 0;
  int64_t fade_reference_length_samples = 0;
  /// Legacy combined curve view. New code should use fade_in_curve /
  /// fade_out_curve; kept so existing aggregate users keep the old shape.
  FadeCurve fade_curve = FadeCurve::Linear;
  FadeCurve fade_in_curve = FadeCurve::Linear;
  FadeCurve fade_out_curve = FadeCurve::Linear;
  std::shared_ptr<const ClipAudioStorage> storage;
  std::shared_ptr<const ClipPagedAudioProvider> page_provider;
};

struct ClipBoundaryList {
  static constexpr size_t kCapacity = 64;
  std::array<int, kCapacity> offsets{};
  size_t size = 0;
  bool overflowed = false;

  void clear() noexcept;
  bool add(int offset) noexcept;
  void sort_unique() noexcept;
};

class ClipPlayer final : public rt::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override {}

  void set_tempo_map(const transport::TempoMap* tempo_map) noexcept;
  void set_timeline_sample(int64_t timeline_sample) noexcept { timeline_sample_ = timeline_sample; }
  void set_page_request_sink(ClipPageRequestSink* sink) noexcept { page_request_sink_ = sink; }
  void set_clips(std::vector<ClipSchedule> clips,
                 const transport::TempoMap* tempo_map_override = nullptr);

  /// Adopt the latest published clip set on the audio thread. Call once at
  /// block start before process_at / collect_boundaries. RT-safe, no alloc.
  void acquire_clips() noexcept { clips_.acquire(); }

  /// Number of scheduled clips. Safe to poll from the control/host thread
  /// while audio is rendering: reads a published atomic rather than calling
  /// the audio-thread-only RtPublisher::acquire().

  void process_at(float* const* channels, int num_channels, int num_samples,
                  int64_t timeline_sample) noexcept;
  void collect_boundaries(int64_t block_start_sample, int num_frames,
                          ClipBoundaryList* out) const noexcept;

  size_t clip_count() const noexcept;

 private:
  // Curves come from the clip itself (fade_in_curve / fade_out_curve), so no
  // curve parameter: the legacy single fade_curve field is not consulted here.
  static float fade_gain(const ClipSchedule& clip, int64_t position) noexcept;
  static int64_t local_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept;
  static double source_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept;
  static int source_channel_count(const ClipSchedule& clip) noexcept;
  static int64_t source_sample_count(const ClipSchedule& clip) noexcept;
  float sample_channel(const ClipSchedule& clip, int src_ch, double source_pos) noexcept;

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int64_t timeline_sample_ = 0;
  ClipPageRequestSink* page_request_sink_ = nullptr;
  const transport::TempoMap* tempo_map_ = nullptr;
  mutable rt::RtPublisher<std::vector<ClipSchedule>> clips_;
  // Published by set_clips() on the control thread; read lock-free by
  // clip_count() so host polling never races the audio thread's acquire().
  std::atomic<size_t> clip_count_{0};
};

}  // namespace sonare::engine
