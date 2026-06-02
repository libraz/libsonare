#pragma once

/// @file channel_strip.h
/// @brief Commercial-grade channel strip: EQ, fader, pan, meter, and aux sends.

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/eq/parametric.h"
#include "mixing/alignment_delay.h"
#include "mixing/automation_lane.h"
#include "mixing/gain.h"
#include "mixing/goniometer_buffer.h"
#include "mixing/meter.h"
#include "mixing/panner.h"
#include "mixing/send.h"
#include "mixing/stereo_width.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

/// @brief Insertion point of the parametric EQ stage relative to the fader.
enum class EqPosition {
  PreFader,
  PostFader,
};

enum class TapPoint {
  PreFader,
  PostFader,
};

struct ChannelStripConfig {
  float fader_db = 0.0f;
  float pan = 0.0f;
  PanLaw pan_law = PanLaw::Const3dB;
  float smoothing_ms = 5.0f;
  EqPosition eq_position = EqPosition::PreFader;
  float input_trim_db = 0.0f;
};

/// @brief Per-track signal path: trim, EQ, pre-fader sends, fader, pan,
///        post-fader sends, meter, and optional insert chain.
///
/// @par Thread-safety
/// - @c process and @c process_at are RT-safe (no allocation, no locks)
///   once @c prepare has been called with the working block size and the
///   target number of pre/post inserts (capped at @c kMaxInserts). They must
///   be driven by a single audio thread.
/// - All structural mutators — @c add_pre_insert / @c add_post_insert,
///   @c add_send, @c set_eq_position, @c set_pan_law, @c set_polarity_invert,
///   etc. — must be called from the host/control thread. They may allocate
///   and may throw @c sonare::SonareException (InvalidParameter for null
///   processors, InvalidState when the insert cap is exhausted).
/// - Real-time parameter changes (fader, pan, send level, EQ band gains)
///   land via @c AutomationLane::push from the control thread; the audio
///   thread reads them with @c AutomationLane::pull during @c process_at.
class ChannelStrip : public rt::ProcessorBase {
 public:
  explicit ChannelStrip(ChannelStripConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void process_at(float* const* channels, int num_channels, int num_samples, int64_t block_start);
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  int pre_fader_latency_samples_q8() const noexcept;
  int post_fader_latency_samples_q8() const noexcept;

  void set_polarity_invert(bool left, bool right) noexcept;
  bool polarity_invert_left() const noexcept;
  bool polarity_invert_right() const noexcept;

  void set_channel_delay_samples(int delay_samples);
  int channel_delay_samples() const noexcept { return alignment_delay_.delay_samples(); }

  void set_width(float width) noexcept { width_.set_width(width); }
  float width() const noexcept { return width_.width(); }
  bool schedule_width_automation(int64_t sample_pos, float width,
                                 AutomationCurveType curve = AutomationCurveType::Linear) noexcept;

  void set_fader_db(float fader_db) noexcept { fader_.set_gain_db(fader_db); }
  float fader_db() const noexcept { return fader_.gain_db(); }
  bool schedule_fader_automation(int64_t sample_pos, float fader_db,
                                 AutomationCurveType curve = AutomationCurveType::Linear) noexcept;

  void set_input_trim_db(float trim_db) noexcept { input_trim_.set_gain_db(trim_db); }
  float input_trim_db() const noexcept { return input_trim_.gain_db(); }

  void set_vca_offset_db(float offset_db) noexcept { fader_.set_vca_offset_db(offset_db); }
  void add_vca_group_offset_db(float delta_db) noexcept {
    fader_.add_vca_group_offset_db(delta_db);
  }
  float vca_offset_db() const noexcept { return fader_.vca_offset_db(); }

  void set_pan(float pan) noexcept { panner_.set_pan(pan); }
  float pan() const noexcept { return panner_.pan(); }
  bool schedule_pan_automation(int64_t sample_pos, float pan,
                               AutomationCurveType curve = AutomationCurveType::Linear) noexcept;

  void set_pan_law(PanLaw law) noexcept { panner_.set_pan_law(law); }
  PanLaw pan_law() const noexcept { return panner_.pan_law(); }
  void set_pan_mode(PanMode mode) noexcept { panner_.set_pan_mode(mode); }
  PanMode pan_mode() const noexcept { return panner_.pan_mode(); }
  void set_dual_pan(float left_pan, float right_pan) noexcept {
    panner_.set_dual_pan(left_pan, right_pan);
  }
  float dual_pan_left() const noexcept { return panner_.dual_pan_left(); }
  float dual_pan_right() const noexcept { return panner_.dual_pan_right(); }

  void set_muted(bool muted) noexcept;
  bool muted() const noexcept;
  bool effectively_muted() const noexcept;

  void set_soloed(bool soloed) noexcept;
  bool soloed() const noexcept;

  void set_solo_safe(bool solo_safe) noexcept;
  bool solo_safe() const noexcept;

  void set_implied_mute(bool implied_mute) noexcept;
  bool implied_mute() const noexcept;

  // EQ stage. set_eq_band/clear_eq_band/eq() are control-thread mutators and must not
  // run concurrently with process(); the position toggle is atomic and audio-thread safe.
  void set_eq_band(size_t index, const sonare::mastering::eq::EqBand& band) {
    eq_.set_band(index, band);
  }
  void clear_eq_band(size_t index) { eq_.clear_band(index); }
  void set_eq_position(EqPosition p) noexcept { eq_position_.store(p, std::memory_order_relaxed); }
  EqPosition eq_position() const noexcept { return eq_position_.load(std::memory_order_relaxed); }
  sonare::mastering::eq::ParametricEq& eq() noexcept { return eq_; }
  // Const accessor for read-only introspection of the embedded EQ stage. The EQ
  // is currently reachable only from C++ (no scene/binding field exposes it); a
  // const overload keeps the accessor pair consistent for read-only callers.
  const sonare::mastering::eq::ParametricEq& eq() const noexcept { return eq_; }

  // Embedded meters. The no-arg overload is kept as the post-chain/output meter.
  MeterSnapshot meter_snapshot() const noexcept { return meter_snapshot(TapPoint::PostFader); }
  MeterSnapshot meter_snapshot(TapPoint tap) const noexcept;

  // Inserts are control-thread mutators and must not run concurrently with process().
  void add_pre_insert(std::unique_ptr<rt::ProcessorBase> processor);
  void add_post_insert(std::unique_ptr<rt::ProcessorBase> processor);
  size_t num_pre_inserts() const noexcept { return pre_inserts_.size(); }
  size_t num_post_inserts() const noexcept { return post_inserts_.size(); }

  // Schedules a sample-accurate insert-parameter automation event. @p insert_index addresses
  // the combined insert sequence [pre_inserts_ ... then post_inserts_ ...]. Control-thread API.
  bool schedule_insert_automation(unsigned int insert_index, unsigned int param_id,
                                  int64_t sample_pos, float value,
                                  AutomationCurveType curve = AutomationCurveType::Linear) noexcept;
  void set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                            int num_channels, int num_samples);
  void clear_insert_sidechains() noexcept;

  // Aux sends. add_send is a control-thread mutator (may allocate); it must not run
  // concurrently with process()/mix_send(), matching FxBus::add_insert's contract.
  size_t add_send(const SendConfig& cfg);
  size_t num_sends() const noexcept { return sends_.size(); }
  void set_send_db(size_t index, float db);
  SendTiming send_timing(size_t index) const;
  int send_latency_samples_q8(size_t index) const noexcept;
  bool schedule_send_automation(size_t index, int64_t sample_pos, float db,
                                AutomationCurveType curve = AutomationCurveType::Linear) noexcept;

  /// @brief RT-safe: mixes the requested send's tap (post-gain) additively into @p dest.
  /// Must be called after process() and before the next process() of the same block, since
  /// it consumes the pre/post taps captured by the most recent process() call.
  void mix_send(size_t index, float* const* dest, int num_channels, int num_samples);
  void mix_send_at(size_t index, float* const* dest, int num_channels, int num_samples,
                   int64_t block_start);
  size_t read_goniometer_latest(GoniometerPoint* dest, size_t max_points) const noexcept;

  // Upper bound on the number of distinct (insert_index, param_id) automation
  // targets the strip may track. Reserved at construction so that
  // schedule_insert_automation() (control thread) cannot trigger a
  // reallocation of insert_automation_ while the audio thread is iterating
  // it inside process_at() -- such a reallocation would invalidate the
  // audio-thread iterator (C++ UB). schedule_insert_automation() returns
  // false once this cap is reached instead of growing the vector.
  static constexpr size_t kMaxInsertAutomationLanes = 64;

  // Upper bound on inserts per strip (pre + post combined). Reserved at
  // construction so add_pre_insert / add_post_insert never reallocate
  // pre_inserts_ / post_inserts_ / insert_sidechains_ while the audio thread
  // iterates them. Exceeding the cap throws std::length_error from
  // add_pre_insert / add_post_insert.
  static constexpr size_t kMaxInserts = 64;

#ifdef SONARE_TESTING
  // Test-only introspection used to assert that schedule_insert_automation
  // does not reallocate insert_automation_ after construction.
  size_t insert_automation_capacity() const noexcept { return insert_automation_.capacity(); }
  size_t insert_automation_size() const noexcept { return insert_automation_.size(); }
  size_t insert_sidechains_capacity() const noexcept { return insert_sidechains_.capacity(); }
  size_t pre_inserts_capacity() const noexcept { return pre_inserts_.capacity(); }
  size_t post_inserts_capacity() const noexcept { return post_inserts_.capacity(); }
#endif

 private:
  static constexpr int kPreparedChannels = 2;
  static constexpr int kMaxStackChannels = 8;
  static constexpr size_t kMaxAutomationEventsPerBlock = 128;
  static constexpr size_t kGoniometerCapacity = 4096;

  void process_unsegmented(float* const* channels, int num_channels, int num_samples);
  void process_segment(float* const* channels, int num_channels, int start, int num_samples,
                       int tap_offset);
  void apply_automation_event(const AutomationEvent& event) noexcept;
  void process_insert_chain(std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts,
                            float* const* channels, int num_channels, int num_samples,
                            size_t first_insert_index, int sidechain_offset);

  struct InsertSidechain {
    std::array<const float*, kMaxStackChannels> channels{};
    int num_channels = 0;
    int num_samples = 0;
    bool managed = false;
  };

  GainProcessor input_trim_;
  AlignmentDelay alignment_delay_;
  GainProcessor fader_;
  PannerProcessor panner_;
  StereoWidthProcessor width_;
  sonare::mastering::eq::ParametricEq eq_;
  // Both taps measure true-peak so a metering UI sees real inter-sample peaks
  // pre- and post-fader rather than a floor reading on the pre-fader tap.
  MeterProcessor pre_meter_{{true, true, 4}};
  MeterProcessor post_meter_{{true, true, 4}};
  GoniometerBuffer<kGoniometerCapacity> goniometer_;
  std::vector<std::unique_ptr<rt::ProcessorBase>> pre_inserts_;
  std::vector<std::unique_ptr<rt::ProcessorBase>> post_inserts_;
  std::vector<InsertSidechain> insert_sidechains_;
  struct InsertAutomationLane {
    AutomationTarget target{};
    std::unique_ptr<AutomationLane> lane;
  };

  std::vector<std::unique_ptr<SendProcessor>> sends_;
  std::vector<std::unique_ptr<AutomationLane>> send_automation_;
  std::vector<InsertAutomationLane> insert_automation_;
  // Number of constructed lanes in insert_automation_ visible to the audio
  // thread. The control thread (schedule_insert_automation) fully constructs a
  // new lane into the reserved slot, THEN publishes the new size with
  // memory_order_release. The audio thread reads it with memory_order_acquire
  // and iterates [0, size) by index -- never via range-for over the vector,
  // whose size_ member is not atomic. Capacity is reserved up-front so the
  // backing storage never reallocates.
  std::atomic<size_t> insert_automation_size_{0};
  AutomationLane fader_automation_;
  AutomationLane pan_automation_;
  AutomationLane width_automation_;

  std::atomic<float> polarity_left_{1.0f};
  std::atomic<float> polarity_right_{1.0f};
  std::atomic<bool> muted_{false};
  std::atomic<bool> soloed_{false};
  std::atomic<bool> solo_safe_{false};
  std::atomic<bool> implied_mute_{false};
  std::atomic<EqPosition> eq_position_{EqPosition::PreFader};

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;

  // Preallocated scratch taps, [kPreparedChannels][max_block_size_].
  std::vector<std::vector<float>> pre_tap_;    // post-input/pre-insert chain, pre-fader signal
  std::vector<std::vector<float>> post_tap_;   // final output
  std::vector<std::vector<float>> send_temp_;  // per-send work buffer
};

}  // namespace sonare::mixing
