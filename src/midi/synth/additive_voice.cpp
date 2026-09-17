#include "midi/synth/additive_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kTwoPi;

/// Hammond drawbar pitches as ratios to the played note.
constexpr std::array<float, kAdditivePartials> kDrawbarRatios = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f,
                                                                 4.0f, 5.0f, 6.0f, 8.0f};

/// Drawbar stop digit (0-8) -> linear gain (~3 dB per stop, 0 = off).
float drawbar_gain(float level) noexcept {
  const float stops = std::clamp(level, 0.0f, 8.0f);
  if (stops <= 0.0f) return 0.0f;
  return std::pow(10.0f, (stops - 8.0f) * 3.0f / 20.0f);
}

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

// Live-control smoothing time (ms) for the morph position: the same ramp the
// continuously-excited engines give their CC targets.
SONARE_TUNABLE(kControlSmoothMs, 8.0f);

}  // namespace

void AdditiveVoiceCore::start(const AdditivePatchParams& params, double sample_rate, uint8_t note,
                              uint8_t velocity, uint64_t seed, bool percussion) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float f0 = note_to_hz(note);
  noise_ = VoiceRandomSequence(seed);

  // Both registrations are normalized against their own full sum, so a sweep
  // between them holds level rather than tracking how many bars are drawn.
  sum_a_ = 0.0f;
  sum_b_ = 0.0f;
  for (int k = 0; k < kAdditivePartials; ++k) {
    sum_a_ += drawbar_gain(params.drawbars[static_cast<size_t>(k)]);
    sum_b_ += drawbar_gain(params.drawbars_b[static_cast<size_t>(k)]);
  }
  // No second registration drawn: the far end is the near one, so the crossfade
  // is the identity at every position and a patch that declared nothing sounds
  // exactly as it did before this axis existed.
  const bool second = sum_b_ > 0.0f;
  if (!second) sum_b_ = sum_a_;

  for (int k = 0; k < kAdditivePartials; ++k) {
    Partial& partial = partials_[static_cast<size_t>(k)];
    gain_a_[static_cast<size_t>(k)] = 0.0f;
    gain_b_[static_cast<size_t>(k)] = 0.0f;
    const float freq = f0 * kDrawbarRatios[static_cast<size_t>(k)];
    // Tonewheels above the generator range simply do not exist.
    if (freq >= 0.45f * static_cast<float>(sr)) {
      partial = Partial{};
      continue;
    }
    partial.base_inc = static_cast<float>(static_cast<double>(freq) / sr);
    const float a = drawbar_gain(params.drawbars[static_cast<size_t>(k)]);
    gain_a_[static_cast<size_t>(k)] = a;
    gain_b_[static_cast<size_t>(k)] =
        second ? drawbar_gain(params.drawbars_b[static_cast<size_t>(k)]) : a;
    // Seeded start phase: free-running tonewheels are never phase-locked.
    partial.phase = noise_.unipolar_at(static_cast<uint64_t>(k));
  }

  morph_base_ = std::clamp(params.morph, 0.0f, 1.0f);
  morph_ = morph_base_;
  morph_target_ = morph_base_;
  morph_coeff_ = ramp_coeff(kControlSmoothMs, sr);
  morph_live_ = false;
  apply_morph();

  // Key click: contact transient scaled a little by velocity.
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  click_level_ = std::clamp(params.key_click, 0.0f, 1.0f) * (0.5f + 0.5f * vel01) * 0.6f;
  const float decay_ms = std::max(0.5f, params.click_decay_ms);
  click_coeff_ = std::exp(-1.0f / (decay_ms * 0.001f * static_cast<float>(sr)));
  click_index_ = 0;

  perc_phase_ = 0.0;
  perc_base_inc_ = 0.0f;
  perc_level_ = 0.0f;
  perc_coeff_ = 0.0f;
  const int harmonic = params.percussion_harmonic >= 3 ? 3 : 2;
  const float perc_freq = f0 * static_cast<float>(harmonic);
  if (percussion && params.percussion_harmonic >= 2 && perc_freq < 0.45f * static_cast<float>(sr)) {
    perc_base_inc_ = static_cast<float>(static_cast<double>(perc_freq) / sr);
    perc_level_ = std::clamp(params.percussion_level, 0.0f, 1.0f);
    const float perc_ms = std::max(1.0f, params.percussion_decay_ms);
    perc_coeff_ = std::exp(-1.0f / (perc_ms * 0.001f * static_cast<float>(sr)));
  }
}

void AdditiveVoiceCore::apply_morph() noexcept {
  const float m = morph_;
  const float sum = sum_a_ + (sum_b_ - sum_a_) * m;
  const float norm = sum > 0.0f ? 1.0f / sum : 1.0f;
  for (int k = 0; k < kAdditivePartials; ++k) {
    const size_t i = static_cast<size_t>(k);
    partials_[i].gain = (gain_a_[i] + (gain_b_[i] - gain_a_[i]) * m) * norm;
  }
}

void AdditiveVoiceCore::set_spectrum_mod(float morph_offset) noexcept {
  morph_target_ = std::clamp(morph_base_ + morph_offset, 0.0f, 1.0f);
  if (morph_target_ != morph_) morph_live_ = true;
}

float AdditiveVoiceCore::render(float pitch_ratio) noexcept {
  if (morph_live_) {
    const float gap = morph_target_ - morph_;
    if (std::abs(gap) < 1.0e-6f) {
      morph_ = morph_target_;
      morph_live_ = false;
    } else {
      morph_ += morph_coeff_ * gap;
    }
    apply_morph();
  }
  float mix = 0.0f;
  for (Partial& partial : partials_) {
    if (partial.gain <= 0.0f) continue;
    mix += std::sin(kTwoPi * static_cast<float>(partial.phase)) * partial.gain;
    partial.phase += static_cast<double>(partial.base_inc * pitch_ratio);
    if (partial.phase >= 1.0) partial.phase -= std::floor(partial.phase);
  }
  if (click_level_ > 1.0e-5f) {
    mix += click_level_ * noise_.bipolar_at((1ull << 16) + click_index_++);
    click_level_ *= click_coeff_;
  }
  if (perc_level_ > 1.0e-5f) {
    mix += std::sin(kTwoPi * static_cast<float>(perc_phase_)) * perc_level_;
    perc_phase_ += static_cast<double>(perc_base_inc_ * pitch_ratio);
    if (perc_phase_ >= 1.0) perc_phase_ -= std::floor(perc_phase_);
    perc_level_ *= perc_coeff_;
  }
  return mix;
}

void AdditiveVoiceCore::kill() noexcept {
  for (Partial& partial : partials_) partial.gain = 0.0f;
  // Both registrations too: a morph still ramping would otherwise write the
  // gains back on the next sample and the voice would keep sounding.
  gain_a_.fill(0.0f);
  gain_b_.fill(0.0f);
  morph_live_ = false;
  click_level_ = 0.0f;
  perc_level_ = 0.0f;
}

}  // namespace sonare::midi::synth
