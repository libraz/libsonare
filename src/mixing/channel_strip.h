#pragma once

/// @file channel_strip.h
/// @brief Commercial-grade channel strip: EQ, fader, pan, meter, and aux sends.

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
};

class ChannelStrip : public rt::ProcessorBase {
 public:
  explicit ChannelStrip(ChannelStripConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void process_at(float* const* channels, int num_channels, int num_samples, int64_t block_start);
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;

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

  void set_vca_offset_db(float offset_db) noexcept { fader_.set_vca_offset_db(offset_db); }
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

  // Embedded meters. The no-arg overload is kept as the post-chain/output meter.
  MeterSnapshot meter_snapshot() const noexcept { return meter_snapshot(TapPoint::PostFader); }
  MeterSnapshot meter_snapshot(TapPoint tap) const noexcept;

  // Inserts are control-thread mutators and must not run concurrently with process().
  void add_pre_insert(std::unique_ptr<rt::ProcessorBase> processor);
  void add_post_insert(std::unique_ptr<rt::ProcessorBase> processor);
  size_t num_pre_inserts() const noexcept { return pre_inserts_.size(); }
  size_t num_post_inserts() const noexcept { return post_inserts_.size(); }

  // Aux sends. add_send is a control-thread mutator (may allocate); it must not run
  // concurrently with process()/mix_send(), matching FxBus::add_insert's contract.
  size_t add_send(const SendConfig& cfg);
  size_t num_sends() const noexcept { return sends_.size(); }
  void set_send_db(size_t index, float db);
  SendTiming send_timing(size_t index) const;
  bool schedule_send_automation(size_t index, int64_t sample_pos, float db,
                                AutomationCurveType curve = AutomationCurveType::Linear) noexcept;

  /// @brief RT-safe: mixes the requested send's tap (post-gain) additively into @p dest.
  /// Must be called after process() and before the next process() of the same block, since
  /// it consumes the pre/post taps captured by the most recent process() call.
  void mix_send(size_t index, float* const* dest, int num_channels, int num_samples);
  void mix_send_at(size_t index, float* const* dest, int num_channels, int num_samples,
                   int64_t block_start);
  size_t read_goniometer_latest(GoniometerPoint* dest, size_t max_points) const noexcept;

 private:
  static constexpr int kPreparedChannels = 2;
  static constexpr int kMaxStackChannels = 8;
  static constexpr size_t kMaxAutomationEventsPerBlock = 128;
  static constexpr size_t kGoniometerCapacity = 4096;

  void process_unsegmented(float* const* channels, int num_channels, int num_samples);
  void process_segment(float* const* channels, int num_channels, int start, int num_samples,
                       int tap_offset);
  void apply_automation_event(const AutomationEvent& event) noexcept;

  AlignmentDelay alignment_delay_;
  GainProcessor fader_;
  PannerProcessor panner_;
  StereoWidthProcessor width_;
  sonare::mastering::eq::ParametricEq eq_;
  MeterProcessor pre_meter_;
  MeterProcessor post_meter_{{true, true, 4}};
  GoniometerBuffer<kGoniometerCapacity> goniometer_;
  std::vector<std::unique_ptr<rt::ProcessorBase>> pre_inserts_;
  std::vector<std::unique_ptr<rt::ProcessorBase>> post_inserts_;
  std::vector<std::unique_ptr<SendProcessor>> sends_;
  std::vector<std::unique_ptr<AutomationLane>> send_automation_;
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
  std::vector<std::vector<float>> pre_tap_;    // post-EQ-if-pre, pre-fader signal
  std::vector<std::vector<float>> post_tap_;   // final output
  std::vector<std::vector<float>> send_temp_;  // per-send work buffer
};

}  // namespace sonare::mixing
