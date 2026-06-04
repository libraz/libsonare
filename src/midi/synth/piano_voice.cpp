#include "midi/synth/piano_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/voice_random.h"
#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// Per-loop-traversal amplitude factor reaching -60 dB after @p t60_s.
float loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept {
  const float loops_to_t60 =
      static_cast<float>(sample_rate) * std::max(0.01f, t60_s) / std::max(1.0f, period_samples);
  return std::exp(-6.907755279f / loops_to_t60);
}

/// Exact phase delay (samples) of the first-order allpass
/// H(z) = (a + z^-1)/(1 + a z^-1) at normalized frequency @p w.
float allpass_phase_delay(float a, float w) noexcept {
  const float sinw = std::sin(w);
  const float cosw = std::cos(w);
  const float phi = std::atan2(-sinw, a + cosw) - std::atan2(-a * sinw, 1.0f + a * cosw);
  return -phi / std::max(w, 1.0e-6f);
}

/// Soundboard dominant-mode sketch (Hz / t60 s / weight). Fixed data-free
/// voicing shared by every piano patch.
struct SoundboardModeSpec {
  float freq_hz;
  float t60_s;
  float weight;
};
constexpr SoundboardModeSpec kSoundboardModes[4] = {
    {113.0f, 0.30f, 1.0f},
    {196.0f, 0.25f, 0.8f},
    {285.0f, 0.20f, 0.6f},
    {392.0f, 0.18f, 0.5f},
};

}  // namespace

void PianoVoiceCore::start(const PianoPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float f0 = note_to_hz(note);
  const float period = static_cast<float>(sr) / f0;
  const float w0 = kTwoPi / period;
  VoiceRandomSequence jitter(seed);

  // Loop lowpass (frequency-dependent damping).
  const float lp_a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.6f;
  loop_alpha_ = 1.0f - lp_a;
  const float tau_lp =
      std::atan2(lp_a * std::sin(w0), 1.0f - lp_a * std::cos(w0)) / std::max(w0, 1.0e-6f);

  // Stiffness dispersion: keyboard-graded per-stage allpass delay, clamped
  // so the loop line keeps at least a few samples.
  const float key_t = std::clamp((static_cast<float>(note & 0x7Fu) - 21.0f) / 87.0f, 0.0f, 1.0f);
  const float dispersion = std::clamp(params.dispersion, 0.0f, 1.0f);
  float tau_d = 1.0f + 3.2f * key_t * dispersion;
  const float tau_budget = (period - 4.0f - tau_lp) / static_cast<float>(kPianoDispersionStages);
  tau_d = std::clamp(tau_d, 1.0f, std::max(1.0f, tau_budget));
  const float ap_a = (1.0f - tau_d) / (1.0f + tau_d);

  // Two-stage decay rates (stretched down the keyboard).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float scale = std::exp2(stretch * octaves_below_a4);
  const float t60_fast = std::max(0.05f, params.decay_fast_s) * scale;
  const float t60_slow = std::max(t60_fast, params.decay_slow_s * scale);

  num_strings_ = std::clamp(params.strings, 1, kMaxPianoStrings);
  const float spread = std::max(0.0f, params.detune_cents);
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.buffer = slab_ != nullptr ? slab_ + static_cast<size_t>(i) * string_capacity_ : nullptr;
    // Micro-detune: symmetric spread plus seeded jitter.
    float offset = 0.0f;
    if (num_strings_ > 1) {
      offset = spread * (static_cast<float>(i) / static_cast<float>(num_strings_ - 1) - 0.5f);
      offset *= 1.0f + 0.2f * jitter.bipolar_at(static_cast<uint64_t>(i));
    }
    const float detune_ratio = std::exp2(offset / 1200.0f);
    s.base_period = period / detune_ratio;
    s.ap_a = ap_a;
    s.ap_state.fill(0.0f);
    s.lp_state = 0.0f;
    s.write_index = 0;
    const float tau_ap = allpass_phase_delay(ap_a, w0);
    s.comp = 1.0f + tau_lp + static_cast<float>(kPianoDispersionStages) * tau_ap;
    s.g_slow = loop_gain_for(s.base_period, sr, t60_slow);
    s.g_fast = loop_gain_for(s.base_period, sr, t60_fast);
    s.size = std::min(string_capacity_, static_cast<int>(s.base_period * 1.3f) + 8);
    if (s.buffer != nullptr && s.size > 0) {
      std::fill(s.buffer, s.buffer + static_cast<size_t>(s.size), 0.0f);
    }
  }
  for (int i = num_strings_; i < kMaxPianoStrings; ++i) strings_[static_cast<size_t>(i)] = String{};
  bridge_ = 0.0f;
  release_gain_ = loop_gain_for(period, sr, std::max(0.01f, params.release_damp_s));

  // Felt-hammer pulse: Hertz-contact scaling of contact time and force with
  // velocity (p = hammer_exponent), shorter contact up the keyboard.
  const float vel01 = std::max(static_cast<float>(velocity & 0x7Fu) / 127.0f, 0.02f);
  const float p = std::clamp(params.hammer_exponent, 1.5f, 4.0f);
  const float time_exp = -(p - 1.0f) / (p + 1.0f);
  const float amp_exp = 2.0f * p / (p + 1.0f);
  const float contact_ms = std::clamp(params.hammer_contact_ms, 0.2f, 10.0f) *
                           std::pow(vel01 / 0.6f, time_exp) *
                           std::exp2(-(static_cast<float>(note & 0x7Fu) - 69.0f) / 36.0f);
  contact_samples_ =
      std::clamp(static_cast<int>(contact_ms * 0.001f * static_cast<float>(sr)), 8, 2048);
  hammer_amp_ = 0.9f * std::pow(vel01, amp_exp);
  comb_delay_ = static_cast<int>(std::clamp(params.strike_position, 0.0f, 0.5f) * period + 0.5f);
  exc_pos_ = 0;
  // Felt stiffening: compressed felt (hard strike) passes far more of the
  // pulse's top end — a velocity-driven one-pole on the injected force.
  const float exc_cutoff = 800.0f * std::exp2(3.0f * vel01);
  exc_alpha_ = std::clamp(1.0f - std::exp(-6.28318530718f * exc_cutoff / static_cast<float>(sr)),
                          0.01f, 1.0f);
  exc_lp_ = 0.0f;

  // Soundboard bank (fixed data-free voicing).
  soundboard_mix_ = std::clamp(params.soundboard, 0.0f, 1.0f);
  for (size_t m = 0; m < soundboard_.size(); ++m) {
    SoundboardMode& mode = soundboard_[m];
    const SoundboardModeSpec& spec = kSoundboardModes[m];
    if (spec.freq_hz >= 0.45f * static_cast<float>(sr)) {
      mode = SoundboardMode{};
      continue;
    }
    const float w = kTwoPi * spec.freq_hz / static_cast<float>(sr);
    const float r = std::exp(-6.907755279f / (static_cast<float>(sr) * spec.t60_s));
    mode.a1 = 2.0f * r * std::cos(w);
    mode.a2 = -r * r;
    mode.gain = spec.weight * std::sin(w);
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
  }
}

float PianoVoiceCore::hammer_force(int64_t n) const noexcept {
  if (n < 0 || n >= contact_samples_) return 0.0f;
  const float t = static_cast<float>(n) / static_cast<float>(contact_samples_);
  const float half = std::sin(kPi * t);
  return hammer_amp_ * half * half;
}

float PianoVoiceCore::render(float pitch_ratio) noexcept {
  if (num_strings_ <= 0 || slab_ == nullptr) return 0.0f;

  // Analytic strike-position comb on the felt pulse, through the
  // velocity-driven felt-stiffness lowpass.
  float exc = 0.0f;
  float knock = 0.0f;
  if (exc_pos_ < static_cast<int64_t>(contact_samples_) + comb_delay_) {
    const float force = hammer_force(exc_pos_) - hammer_force(exc_pos_ - comb_delay_);
    ++exc_pos_;
    exc_lp_ += exc_alpha_ * (force - exc_lp_);
    exc = exc_lp_ / static_cast<float>(num_strings_);
    // The hammer knock radiates through the soundboard immediately — the
    // string fundamental only develops after one loop period.
    knock = 0.6f * exc_lp_;
  }

  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  float sum = 0.0f;
  float lp_sum = 0.0f;
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    if (s.buffer == nullptr || s.size < 8) continue;
    // Coupled two-stage decay: the coherent (bridge) component recirculates
    // at the fast prompt rate, the residual at the slow aftersound rate.
    const float fb = s.g_slow * s.lp_state - (s.g_slow - s.g_fast) * bridge_;
    const float delay =
        std::clamp(s.base_period / ratio - s.comp, 1.0f, static_cast<float>(s.size - 4));
    const float out =
        rt::lagrange3_fractional_delay(s.buffer, static_cast<size_t>(s.size), s.write_index,
                                       static_cast<int>(delay * 256.0f), exc + fb);
    // Dispersion allpass cascade then the loop lowpass.
    float v = out;
    for (float& state : s.ap_state) {
      const float y = s.ap_a * v + state;
      state = v - s.ap_a * y;
      v = y;
    }
    s.lp_state += loop_alpha_ * (v - s.lp_state);
    lp_sum += s.lp_state;
    sum += out;
  }
  bridge_ = lp_sum / static_cast<float>(num_strings_);
  sum += knock;

  // Soundboard resonators driven by the bridge sum (including the knock).
  if (soundboard_mix_ > 0.0f) {
    float sb = 0.0f;
    for (SoundboardMode& mode : soundboard_) {
      if (mode.gain == 0.0f && mode.a1 == 0.0f) continue;
      const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * sum;
      mode.y2 = mode.y1;
      mode.y1 = y;
      sb += y;
    }
    sum += soundboard_mix_ * sb;
  }
  return sum;
}

void PianoVoiceCore::release() noexcept {
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.g_slow = std::min(s.g_slow, release_gain_);
    s.g_fast = std::min(s.g_fast, release_gain_);
  }
}

void PianoVoiceCore::kill() noexcept {
  for (String& s : strings_) s = String{};
  for (SoundboardMode& mode : soundboard_) mode = SoundboardMode{};
  num_strings_ = 0;
  hammer_amp_ = 0.0f;
  contact_samples_ = 0;
}

}  // namespace sonare::midi::synth
