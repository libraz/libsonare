#include "editing/voice_changer/character_stage.h"

#include <algorithm>
#include <cmath>

#include "rt/fractional_delay.h"
#include "util/constants.h"

namespace sonare::editing::voice_changer {

using sonare::constants::kTwoPiD;

namespace {

constexpr float kMegaphoneCenterHz = 1200.0f;
constexpr float kMegaphoneQ = 0.8f;
constexpr float kMegaphoneMaxDrive = 7.0f;

constexpr float kRadioHighpassHz = 300.0f;
constexpr float kRadioLowpassHz = 3000.0f;
constexpr float kRadioMaxHold = 4.0f;
constexpr float kRadioQuantLevels = 127.0f;
constexpr float kRadioNoiseDb = -50.0f;

constexpr double kChorusCenterMs = 7.0;
constexpr double kChorusDepthMs = 2.0;
constexpr double kChorusRateHz = 0.8;
constexpr float kChorusLowpassHz = 8000.0f;
constexpr double kChorusHeadroomMs = 4.0;

constexpr float kStroboMinHz = 4.0f;
constexpr float kStroboMaxHz = 16.0f;
constexpr double kStroboEdgeMs = 2.0;

constexpr double kMsPerSecond = 1000.0;
constexpr int kRadioFilterOrder = 4;

}  // namespace

float CharacterStage::corner_hz(float hz) const noexcept {
  return std::min(hz, kMaxDesignFrequencyRatio * static_cast<float>(sr_));
}

void CharacterStage::prepare(double sr) {
  sr_ = sr;
  const double max_ms = kChorusCenterMs + kChorusDepthMs + kChorusHeadroomMs;
  delay_.assign(static_cast<std::size_t>(std::ceil(max_ms * sr / kMsPerSecond)) + 4, 0.0f);
  apply_config();
  clear_state();
}

void CharacterStage::reset() { clear_state(); }

void CharacterStage::clear_state() noexcept {
  band_.reset();
  drive_.reset();
  for (auto& s : radio_hp_) s.reset();
  for (auto& s : radio_lp_) s.reset();
  hold_phase_ = 1.0f;
  held_ = 0.0f;
  std::fill(delay_.begin(), delay_.end(), 0.0f);
  write_index_ = 0;
  lfo_phase_ = 0.0;
  wet_lp_.reset();
  gate_phase_ = 0.0;
  gate_ = 1.0f;
}

void CharacterStage::set_config(const CharacterConfig& config) {
  const bool type_changed = config.type != config_.type;
  config_ = config;
  config_.amount = std::clamp(config_.amount, 0.0f, 1.0f);
  config_.tone = std::clamp(config_.tone, -1.0f, 1.0f);
  apply_config();
  if (type_changed && sr_ > 0.0) clear_state();
}

void CharacterStage::apply_config() noexcept {
  if (!(sr_ > 0.0)) return;
  const float amount = config_.amount;
  const float tone = config_.tone;
  const auto w0 = [this](float hz) {
    return static_cast<float>(kTwoPiD * static_cast<double>(corner_hz(hz)) / sr_);
  };

  switch (config_.type) {
    case CharacterType::Megaphone: {
      band_.set(rt::rbj_bandpass(w0(kMegaphoneCenterHz * std::exp2(tone)), kMegaphoneQ));
      drive_gain_ = 1.0f + kMegaphoneMaxDrive * amount;
      drive_trim_ = 1.0f / std::sqrt(drive_gain_);
      break;
    }
    case CharacterType::Radio: {
      for (int i = 0; i < 2; ++i) {
        const float q = rt::butterworth_stage_q(kRadioFilterOrder, i);
        radio_hp_[i].set(rt::rbj_highpass(w0(kRadioHighpassHz), q));
        radio_lp_[i].set(rt::rbj_lowpass(w0(kRadioLowpassHz), q));
      }
      hold_step_ = 1.0f / (1.0f + (kRadioMaxHold - 1.0f) * amount);
      // Uniform noise of the requested RMS: peak = rms * sqrt(3).
      noise_peak_ = std::pow(10.0f, kRadioNoiseDb / 20.0f) * amount * std::sqrt(3.0f);
      break;
    }
    case CharacterType::BbdChorus: {
      lfo_step_ = kChorusRateHz / sr_;
      center_delay_samples_ = static_cast<float>(kChorusCenterMs * sr_ / kMsPerSecond);
      depth_delay_samples_ = static_cast<float>(kChorusDepthMs * sr_ / kMsPerSecond);
      chorus_mix_ = 0.5f * amount;
      wet_lp_.set(rt::rbj_lowpass(w0(kChorusLowpassHz), rt::butterworth_stage_q(2, 0)));
      break;
    }
    case CharacterType::Strobo: {
      const float rate = kStroboMinHz + (kStroboMaxHz - kStroboMinHz) * 0.5f * (tone + 1.0f);
      gate_step_ = static_cast<double>(rate) / sr_;
      gate_slew_ = static_cast<float>(kMsPerSecond / (kStroboEdgeMs * sr_));
      gate_depth_ = amount;
      break;
    }
    case CharacterType::None:
      break;
  }
}

float CharacterStage::process_sample(float x) noexcept {
  switch (config_.type) {
    case CharacterType::Megaphone: {
      const float b = band_.process(x);
      return drive_.process(drive_gain_ * b) * drive_trim_;
    }
    case CharacterType::Radio: {
      float y = x;
      for (auto& s : radio_hp_) y = s.process(y);
      for (auto& s : radio_lp_) y = s.process(y);
      hold_phase_ += hold_step_;
      if (hold_phase_ >= 1.0f) {
        hold_phase_ -= 1.0f;
        held_ = std::round(std::clamp(y, -1.0f, 1.0f) * kRadioQuantLevels) / kRadioQuantLevels;
      }
      noise_state_ ^= noise_state_ << 13;
      noise_state_ ^= noise_state_ >> 17;
      noise_state_ ^= noise_state_ << 5;
      const float u = static_cast<float>(noise_state_) * (2.0f / 4294967296.0f) - 1.0f;
      return held_ + noise_peak_ * u;
    }
    case CharacterType::BbdChorus: {
      if (delay_.empty()) return x;
      delay_[write_index_] = x;
      const float sweep = static_cast<float>(std::sin(kTwoPiD * lfo_phase_));
      const float d = center_delay_samples_ + depth_delay_samples_ * sweep;
      const int q8 = static_cast<int>(std::lround(d * 256.0f));
      const float wet =
          wet_lp_.process(rt::lagrange3_read(delay_.data(), delay_.size(), write_index_, q8));
      write_index_ = (write_index_ + 1) % delay_.size();
      lfo_phase_ += lfo_step_;
      if (lfo_phase_ >= 1.0) lfo_phase_ -= 1.0;
      return x * (1.0f - chorus_mix_) + wet * chorus_mix_;
    }
    case CharacterType::Strobo: {
      const float target = gate_phase_ < 0.5 ? 1.0f : 0.0f;
      if (gate_ < target) {
        gate_ = std::min(target, gate_ + gate_slew_);
      } else {
        gate_ = std::max(target, gate_ - gate_slew_);
      }
      gate_phase_ += gate_step_;
      if (gate_phase_ >= 1.0) gate_phase_ -= 1.0;
      return x * (1.0f - gate_depth_ * (1.0f - gate_));
    }
    case CharacterType::None:
      break;
  }
  return x;
}

}  // namespace sonare::editing::voice_changer
