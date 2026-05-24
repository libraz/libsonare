#pragma once

/// @file channel_strip.h
/// @brief Commercial-grade channel strip: EQ, fader, pan, meter, and aux sends.

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "mastering/eq/parametric.h"
#include "mixing/gain.h"
#include "mixing/meter.h"
#include "mixing/panner.h"
#include "mixing/send.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

/// @brief Insertion point of the parametric EQ stage relative to the fader.
enum class EqPosition {
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
  void reset() override;

  void set_fader_db(float fader_db) noexcept { fader_.set_gain_db(fader_db); }
  float fader_db() const noexcept { return fader_.gain_db(); }

  void set_vca_offset_db(float offset_db) noexcept { fader_.set_vca_offset_db(offset_db); }
  float vca_offset_db() const noexcept { return fader_.vca_offset_db(); }

  void set_pan(float pan) noexcept { panner_.set_pan(pan); }
  float pan() const noexcept { return panner_.pan(); }

  void set_pan_law(PanLaw law) noexcept { panner_.set_pan_law(law); }
  PanLaw pan_law() const noexcept { return panner_.pan_law(); }

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

  // Embedded output meter, tapped after the full chain.
  MeterSnapshot meter_snapshot() const noexcept { return meter_.snapshot(); }

  // Aux sends. add_send is a control-thread mutator (may allocate); it must not run
  // concurrently with process()/mix_send(), matching FxBus::add_insert's contract.
  size_t add_send(const SendConfig& cfg);
  size_t num_sends() const noexcept { return sends_.size(); }
  void set_send_db(size_t index, float db);
  SendTiming send_timing(size_t index) const;

  /// @brief RT-safe: mixes the requested send's tap (post-gain) additively into @p dest.
  /// Must be called after process() and before the next process() of the same block, since
  /// it consumes the pre/post taps captured by the most recent process() call.
  void mix_send(size_t index, float* const* dest, int num_channels, int num_samples);

 private:
  static constexpr int kPreparedChannels = 2;

  GainProcessor fader_;
  PannerProcessor panner_;
  sonare::mastering::eq::ParametricEq eq_;
  MeterProcessor meter_;
  std::vector<std::unique_ptr<SendProcessor>> sends_;

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
