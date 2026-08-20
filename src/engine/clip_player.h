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

#include "engine/warp_stretch.h"
#include "rt/pan_law.h"
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
  /// Non-owning deinterleaved channel pointers. Every channel MUST contain at
  /// least num_samples frames; the arrangement compiler enforces equal channel
  /// lengths before constructing this RT shape.
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
  virtual int64_t page_frames() const noexcept { return 1; }
  virtual bool sample_at(int channel, int64_t sample, float* out) const noexcept = 0;
  /// True when reading page @p page_index would NOT miss. Answers the player's
  /// look-ahead pass, which reports a page request BEFORE the audio thread
  /// reaches that page rather than after it already read silence there.
  ///
  /// Must be as cheap and as RT-safe as sample_at(). The default returns true
  /// ("assume resident"), which keeps a provider that cannot answer out of the
  /// look-ahead path entirely: it never emits a prefetch request and behaves
  /// exactly as before.
  virtual bool page_resident(int64_t page_index) const noexcept {
    (void)page_index;
    return true;
  }
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
  /// Realtime pitch-preserving warp. Follows the same anchor map as kRepitch,
  /// but synthesizes the output with a WSOLA overlap-add instead of resampling,
  /// so a rate change moves the timing without moving the pitch. Falls back to
  /// kRepitch behaviour when no stretcher voice is free or the source has more
  /// channels than the stretcher handles.
  kTimeStretch = 3,
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
  /// Equal-power crossfade length, in samples, applied at the loop seam so a
  /// non-zero-aligned loop boundary does not click. 0 (default) keeps the hard
  /// integer-modulo wrap. The crossfade is period-preserving: it blends the loop
  /// tail with the source material immediately before the loop start, so it needs
  /// that many source samples of pre-roll (clip_offset_samples) and is clamped to
  /// the available pre-roll and to half the loop length. Ignored under warp.
  int64_t loop_crossfade_samples = 0;
  /// Control-plane warp reference id carried from the arrangement clip. The
  /// player never dereferences project/model state on the audio thread; RT warp
  /// playback uses the immutable anchor snapshot below.
  uint32_t warp_ref_id = 0;
  WarpMode warp_mode = WarpMode::kOff;
  int64_t warp_reference_offset_samples = 0;
  std::shared_ptr<const std::vector<WarpAnchor>> warp_anchors;
  /// Source-track id carried from the arrangement clip. 0 = unset.
  uint32_t track_id = 0;
  float gain = 1.0f;
  /// Stereo balance in [-1, +1] (0 = center). Applied per output channel by the
  /// player as a balance control: unity at center, attenuating only the channel
  /// away from the pan direction. Folded from the source track's pan at compile
  /// time.
  float pan = 0.0f;
  /// Pan law the player evaluates @c pan with. The compiler stamps the law of
  /// the channel strip whose controls it folded into this clip, so a track's pan
  /// lands on the same gains whether it was folded here or left on its own
  /// strip. The default is the law a clip carries when no strip was involved at
  /// all — a build without the mixing runtime, or a clip whose pan was set
  /// directly rather than folded from a track.
  rt::PanLaw pan_law = rt::PanLaw::Linear0dB;
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
  /// Timeline frames of look-ahead used to request clip pages BEFORE the audio
  /// thread reads them. 0 disables the look-ahead entirely, leaving only the
  /// historical read-then-miss reporting (which necessarily renders one block of
  /// silence at every page the host has not resident yet). Safe to change while
  /// audio runs.
  void set_page_prefetch_frames(int64_t frames) noexcept {
    page_prefetch_frames_.store(std::max<int64_t>(frames, 0), std::memory_order_relaxed);
  }
  int64_t page_prefetch_frames() const noexcept {
    return page_prefetch_frames_.load(std::memory_order_relaxed);
  }
  void begin_page_miss_block() noexcept;
  void end_page_miss_block() noexcept { external_page_miss_block_ = false; }
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
  void process_track_at(uint32_t track_id, float* const* channels, int num_channels,
                        int num_samples, int64_t timeline_sample) noexcept;
  void process_excluding_tracks_at(const uint32_t* track_ids, size_t track_count,
                                   float* const* channels, int num_channels, int num_samples,
                                   int64_t timeline_sample) noexcept;
  void collect_boundaries(int64_t block_start_sample, int num_frames,
                          ClipBoundaryList* out) const noexcept;

  size_t clip_count() const noexcept;

  /// Number of blocks in which a @c WarpMode::kTimeStretch clip could not be
  /// given a stretcher voice and fell back to resampling. Monotonic; read from
  /// the control thread for telemetry.
  uint32_t warp_stretch_overflow_count() const noexcept { return stretch_overflow_count_; }

 private:
  // Curves come from the clip itself (fade_in_curve / fade_out_curve), so no
  // curve parameter: the legacy single fade_curve field is not consulted here.
  static float fade_gain(const ClipSchedule& clip, int64_t position) noexcept;
  static int64_t local_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept;
  static double source_position(const ClipSchedule& clip, int64_t timeline_sample) noexcept;
  /// Resolved source read for one timeline sample. Normally a single read
  /// (@c pos with @c gain == 1, @c partner_gain == 0); inside a loop-seam
  /// crossfade it additionally returns the pre-roll partner read and the
  /// equal-power blend gains so the caller can sum both source reads.
  struct LoopRead {
    double pos = -1.0;          // primary source position, or < 0 if no read
    double partner = -1.0;      // crossfade partner (pre-roll) source position
    float gain = 1.0f;          // primary read gain
    float partner_gain = 0.0f;  // partner read gain (0 => single read)
  };
  static LoopRead resolve_loop_read(const ClipSchedule& clip, int64_t timeline_sample) noexcept;
  static int source_channel_count(const ClipSchedule& clip) noexcept;
  static int64_t source_sample_count(const ClipSchedule& clip) noexcept;
  void notify_page_miss(const ClipSchedule& clip, int src_ch, int64_t sample) noexcept;
  /// Reports the pages this clip will read over the look-ahead window that are
  /// not resident yet. Runs after the block is rendered, so its requests land
  /// behind this block's genuine misses in the queue and a host that keeps only
  /// the newest request per clip therefore tracks the look-ahead frontier.
  void prefetch_pages(const ClipSchedule& clip, int64_t block_end_sample) noexcept;
  float sample_channel(const ClipSchedule& clip, int src_ch, double source_pos) noexcept;
  /// Context handed to the stretcher's reader / mapper thunks. Stack-allocated
  /// per clip per block; nothing here outlives the render call.
  struct StretchContext {
    ClipPlayer* player;
    const ClipSchedule* clip;
  };
  static float stretch_read_thunk(void* context, int channel, int64_t sample) noexcept;
  static double stretch_map_thunk(void* context, int64_t clip_local_output) noexcept;
  /// Returns the voice already streaming @p clip_id, else a free one, else the
  /// longest-idle one. Null when every voice is busy with a different clip this
  /// block, which is the caller's signal to fall back to resampling.
  WarpStretchVoice* acquire_stretch_voice(uint32_t clip_id) noexcept;
  /// Renders the block range [start, end) of a kTimeStretch clip. Returns false
  /// when the stretcher cannot take the clip, leaving the output untouched so
  /// the caller can run the ordinary resampling path instead.
  bool render_stretched(const ClipSchedule& clip, float* const* channels, int num_channels,
                        int start, int end, int64_t timeline_sample) noexcept;
  enum class TrackFilterMode {
    kAll,
    kOnlyTrack,
    kExcludeTracks,
  };
  void process_filtered_at(uint32_t track_id, const uint32_t* track_ids, size_t track_count,
                           TrackFilterMode filter_mode, float* const* channels, int num_channels,
                           int num_samples, int64_t timeline_sample) noexcept;

  struct PageMissCacheEntry {
    uint32_t clip_id = 0;
    uint32_t channel = 0;
    int64_t page_index = -1;
  };

  /// Preallocated stretcher voices. Eight covers the realistic "a few warped
  /// clips overlap" case; beyond that a clip falls back to resampling rather
  /// than allocating on the audio thread.
  static constexpr size_t kMaxWarpedClips = 8;
  std::array<WarpStretchVoice, kMaxWarpedClips> stretch_voices_{};
  std::array<std::vector<float>, WarpStretchVoice::kMaxChannels> stretch_scratch_{};
  int stretch_scratch_capacity_ = 0;
  uint32_t stretch_overflow_count_ = 0;

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int64_t timeline_sample_ = 0;
  ClipPageRequestSink* page_request_sink_ = nullptr;
  std::array<PageMissCacheEntry, 64> page_miss_cache_{};
  size_t page_miss_cache_size_ = 0;
  bool page_miss_cache_overflowed_ = false;
  bool external_page_miss_block_ = false;
  std::atomic<int64_t> page_prefetch_frames_{0};
  const transport::TempoMap* tempo_map_ = nullptr;
  mutable rt::RtPublisher<std::vector<ClipSchedule>> clips_;
  // Published by set_clips() on the control thread; read lock-free by
  // clip_count() so host polling never races the audio thread's acquire().
  std::atomic<size_t> clip_count_{0};
};

}  // namespace sonare::engine
