#include "editing/voice_changer/space_stage.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rt/fractional_delay.h"
#include "util/constants.h"

namespace sonare::editing::voice_changer {

using constants::kSecondsPerMinute;
using constants::kTwoPiD;

namespace {

constexpr int kReverbBlockHint = 512;
// Interpolator reads up to base + 2 samples back; keep a margin beyond the longest delay.
constexpr std::size_t kRingMargin = 8;

float note_value(NoteDivision division) noexcept {
  switch (division) {
    case NoteDivision::Quarter:
      return 1.0f;
    case NoteDivision::Eighth:
      return 0.5f;
    case NoteDivision::DottedEighth:
      return 0.75f;
    case NoteDivision::Sixteenth:
      return 0.25f;
  }
  return 0.5f;
}

}  // namespace

float space_delay_ms(const ReverbVariationConfig& variation) noexcept {
  switch (variation.variation) {
    case ReverbVariation::Echo:
    case ReverbVariation::DubEcho:
      return variation.delay_ms;
    case ReverbVariation::TempoDelay:
      if (!(variation.tempo_bpm > 0.0f)) return std::numeric_limits<float>::infinity();
      return static_cast<float>(kSecondsPerMinute * 1000.0 / variation.tempo_bpm) *
             note_value(variation.division);
    case ReverbVariation::Reverb:
      break;
  }
  return 0.0f;
}

bool space_delay_exceeds_limit(const ReverbVariationConfig& variation) noexcept {
  return !(space_delay_ms(variation) <= kMaxSpaceDelayMs);
}

void SpaceStage::prepare(double sr) {
  sr_ = sr;
  reverb_.prepare(sr, kReverbBlockHint);
  ring_.assign(static_cast<std::size_t>(std::ceil(kMaxSpaceDelayMs * 0.001 * sr)) + kRingMargin,
               0.0f);
  write_ = 0;
  delay_samples_.prepare(sr, kGlideMs);
  feedback_.prepare(sr, kGlideMs);
  mix_.prepare(sr, kGlideMs);
  const rt::BiquadCoeffs lp = rt::vicanek_lowpass(
      static_cast<float>(kTwoPiD *
                         std::min(static_cast<double>(kDubCornerHz),
                                  static_cast<double>(kMaxDesignFrequencyRatio) * sr) /
                         sr),
      constants::kInvSqrt2);
  dub_lowpass_.set(lp);
  dub_lowpass_.reset();
  snap_ = true;
}

void SpaceStage::reset() {
  reverb_.reset();
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  write_ = 0;
  dub_lowpass_.reset();
}

void SpaceStage::set_config(const StreamingReverbConfig& reverb,
                            const ReverbVariationConfig& variation) {
  reverb_.set_config(reverb);
  if (variation.variation == ReverbVariation::Reverb) {
    variation_ = ReverbVariation::Reverb;
    snap_ = true;
    return;
  }
  if (space_delay_exceeds_limit(variation) || ring_.empty()) return;
  const float delay = static_cast<float>(
      std::max(1.0, static_cast<double>(space_delay_ms(variation)) * 0.001 * sr_));
  const float mix = std::clamp(reverb.mix, 0.0f, kMaxMix);
  const float feedback = std::clamp(variation.feedback, 0.0f, 0.95f);
  if (snap_ || variation_ == ReverbVariation::Reverb) {
    delay_samples_.reset(delay);
    feedback_.reset(feedback);
    mix_.reset(mix);
    snap_ = false;
  }
  delay_samples_.set_target(delay);
  feedback_.set_target(feedback);
  mix_.set_target(mix);
  variation_ = variation.variation;
}

float SpaceStage::process_sample(float x) noexcept {
  if (variation_ == ReverbVariation::Reverb) return reverb_.process_sample(x);
  if (ring_.empty()) return x;

  const float delay = delay_samples_.process();
  const float fb = feedback_.process();
  const float mix = mix_.process();
  const float wet = rt::lagrange3_read(ring_.data(), ring_.size(), write_,
                                       static_cast<int>(std::lround(delay * 256.0f)));
  float fed = wet;
  if (variation_ == ReverbVariation::DubEcho) fed = std::tanh(dub_lowpass_.process(wet));
  ring_[write_] = x + fb * fed;
  if (++write_ >= ring_.size()) write_ = 0;
  return (1.0f - mix) * x + mix * wet;
}

}  // namespace sonare::editing::voice_changer
