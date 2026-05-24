#pragma once

/// @file channel_strip.h
/// @brief Basic channel strip with fader and pan stages.

#include <atomic>

#include "mixing/gain.h"
#include "mixing/panner.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

struct ChannelStripConfig {
  float fader_db = 0.0f;
  float pan = 0.0f;
  PanLaw pan_law = PanLaw::Const3dB;
  float smoothing_ms = 5.0f;
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

 private:
  GainProcessor fader_;
  PannerProcessor panner_;
  std::atomic<bool> muted_{false};
  std::atomic<bool> soloed_{false};
  std::atomic<bool> solo_safe_{false};
  std::atomic<bool> implied_mute_{false};
};

}  // namespace sonare::mixing
