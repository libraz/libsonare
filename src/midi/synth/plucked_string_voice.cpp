#include "midi/synth/plucked_string_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "rt/dispersion.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kTwoPi;

/// Excitation noise draws live far above the voice-level draw indices
/// (detune/phase/drift use 0..~103 on the same per-voice seed).
constexpr uint64_t kNoiseIndexBase = 1ull << 16;

/// Bridge-limiter travel above the threshold: the curved surface gives a little
/// before the string is fully pinned, and a stronger jawari sits the string
/// closer to the surface (less give -> harder knee -> more high partials).
SONARE_TUNABLE(kBuzzSpanBase, 0.35f);

/// Output trim bringing the raw string loop up to a musical voice level (a
/// forte pluck lands roughly in [0.3, 0.8] peak).
SONARE_TUNABLE(kPluckedOutputScale, 0.85f);

/// Where plucked_string.hf_decay_s and mid_decay_s are quoted: the frequencies
/// the Karplus-Strong core quotes its string damping at, fixed because a target
/// quoted at a harmonic is unreachable in the bass.
SONARE_TUNABLE(kPluckedHfQuoteHz, 4000.0f);
SONARE_TUNABLE(kPluckedMidQuoteHz, 1000.0f);

/// Pick contact-click draws sit past the excitation burst's, and the click's
/// length and corner are the Karplus-Strong pick's.
constexpr uint64_t kPickNoiseIndexBase = 1ull << 24;
SONARE_TUNABLE(kPluckedPickNoiseMs, 25.0f);
SONARE_TUNABLE(kPluckedPickNoiseCutoffHz, 6000.0f);

}  // namespace

void PluckedStringVoiceCore::start(const PluckedStringPatchParams& params, double sample_rate,
                                   uint8_t note, Velocity16 velocity, uint64_t seed) noexcept {
  const double host_sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  // A loop too short to hold its pitch and decay at the host rate runs at a multiple of it;
  // everything below is built for that internal rate.
  const int factor = settle_loop_oversample(
      [&](int f) { return configure(params, host_sr * f, note, velocity, seed); });
  decimator_.configure(factor);
}

LoopBudget PluckedStringVoiceCore::configure(const PluckedStringPatchParams& params, double sr,
                                             uint8_t note, Velocity16 velocity,
                                             uint64_t seed) noexcept {
  noise_ = VoiceRandomSequence(seed);
  killed_ = false;

  const float f0 = note_to_hz(note);
  base_period_ = static_cast<float>(sr) / f0;

  // Decay: t60 stretched per octave below A4 (low strings ring longer).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (kMidiA4 - static_cast<float>(note & 0x7Fu)) / kSemitonesPerOctave;
  const float t60 = std::max(0.05f, params.decay_s) * std::exp2(stretch * octaves_below_a4);

  const float omega = kTwoPi / base_period_;
  const float g_requested = string_loop_gain_for(base_period_, sr, t60);
  const float requested_a =
      loss_pole_at_rate((1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.7f, sr);
  const float release_t60 = std::max(0.01f, params.release_damp_s);
  const float hf_t60 = std::max(0.0f, params.hf_decay_s);
  const float quote_w = kTwoPi * kPluckedHfQuoteHz / static_cast<float>(sr);
  float a = 0.0f;
  if (hf_t60 > 0.0f && quote_w > omega * 1.5f) {
    // Loop loss from decay targets: the fundamental's t60 and the ring at the quote frequency,
    // with the mid anchor as a second pole when given, solved as the Karplus-Strong core does.
    const float repay = interpolation_repayment(g_requested, base_period_, requested_a);
    const float mid_t60 = std::max(0.0f, params.mid_decay_s);
    const float mid_w = kTwoPi * kPluckedMidQuoteHz / static_cast<float>(sr);
    const float g_mid =
        mid_t60 > 0.0f ? string_loop_gain_for(base_period_, sr, mid_t60) * repay : 0.0f;
    const QuotedStringLoopFilter solved = solve_quoted_loop_filter(
        omega, quote_w, g_requested * repay, string_loop_gain_for(base_period_, sr, hf_t60) * repay,
        mid_w, g_mid);
    const float release_g = std::min(
        0.9999f, std::min(0.9999f, string_loop_gain_for(base_period_, sr, release_t60) * repay) *
                     solved.release_scale);
    string_.configure_filter(buffer_, capacity_, base_period_, solved.a, solved.g, release_g,
                             solved.has_pole2, solved.a2, solved.g2);
    a = solved.a;
  } else {
    // Loop loss: brightness sets the pole, capped so the fundamental keeps its t60. The read's
    // magnitude at the fundamental is spent from the same budget, sized against the loop delay
    // the unboosted filter leaves (its pole moves only slightly with the boost).
    const float a_unboosted = cap_loss_pole(requested_a, omega, g_requested);
    const float interp_gain =
        loop_budget(base_period_, 1.0f + onepole_group_delay_samples(a_unboosted, omega), 1.0f)
            .interp_gain;
    const float g0 = repay_interpolation(g_requested, interp_gain);
    a = cap_loss_pole(requested_a, omega, g0);
    const float release_g0 = string_loop_gain_for(base_period_, sr, release_t60);
    string_.configure_filter(
        buffer_, capacity_, base_period_, a, compensated_loop_gain(a, omega, g0),
        compensated_loop_gain(a, omega, std::min(0.9999f, release_g0 * interp_gain)));
  }

  // Stiff-string dispersion: an in-loop allpass cascade from B, its phase delay folded into the
  // tuning. 0 skips it, so the loop stays a harmonic string, bit-identical.
  disp_a_ = 0.0f;
  for (float& state : disp_state_) state = 0.0f;
  const float b_coeff = std::max(0.0f, params.inharmonicity_b);
  if (b_coeff > 0.0f) {
    const float phase_budget = base_period_ - 4.0f - onepole_group_delay_samples(a, omega);
    disp_a_ = rt::dispersion_allpass_a(b_coeff, omega, a, kPluckedDispersionStages, phase_budget);
    if (disp_a_ != 0.0f) {
      string_.loop_comp +=
          static_cast<float>(kPluckedDispersionStages) * rt::allpass_phase_delay(disp_a_, omega);
    }
  }

  // Buzzing bridge: a stronger jawari sits the threshold lower into the
  // string's swing, so the returning wave grazes the curved surface on more of
  // every period. 0 disables the limiter entirely so the render path stays
  // bit-identical to the plain plucked string.
  buzz_amount_ = std::clamp(params.buzz, 0.0f, 1.0f);
  buzz_threshold_ = buzz_amount_ > 0.0f ? 0.6f - 0.4f * buzz_amount_ : 0.0f;

  // Excitation: one period of seeded noise through the pick-position comb and
  // the velocity-driven dynamic-level lowpass (hard pluck = bright).
  exc_total_ = std::max(8, static_cast<int>(base_period_));
  exc_pos_ = 0;
  pick_delay_ =
      static_cast<int>(std::clamp(params.pick_position, 0.0f, 0.5f) * base_period_ + 0.5f);
  const float vel01 = velocity.f7() / 127.0f;
  const float vel_amount = std::clamp(params.vel_to_brightness, 0.0f, 1.0f);
  const float bright =
      std::clamp(params.exc_brightness, 0.0f, 1.0f) * ((1.0f - vel_amount) + vel_amount * vel01);
  // Exponential brightness -> cutoff map (300 Hz .. ~12 kHz) through a single
  // one-pole, so the velocity swing is clearly audible.
  const float exc_cutoff = 300.0f * std::exp2(5.3f * bright);
  exc_alpha_ = lowpass_alpha_at_rate(exc_cutoff, sr);
  exc_noise_gain_ = noise_gain_at_rate(sr);
  exc_lp_ = 0.0f;

  // Pick contact click, armed at note-on; 0 adds nothing (bit-identical).
  const float pick_level = std::clamp(params.pick_noise, 0.0f, 1.0f) * vel01;
  pick_.configure(pick_level, kPluckedPickNoiseMs, kPluckedPickNoiseCutoffHz, sr,
                  pick_level > 0.0f);

  output_scale_ = kPluckedOutputScale;
  return loop_budget(base_period_, string_.loop_comp, 1.0f, sr);
}

float PluckedStringVoiceCore::render(float pitch_ratio) noexcept {
  if (killed_) return 0.0f;
  return decimator_.run([&] { return render_internal(pitch_ratio); });
}

float PluckedStringVoiceCore::render_internal(float pitch_ratio) noexcept {
  if (killed_) return 0.0f;
  if (buffer_ == nullptr || capacity_ < 8) return 0.0f;

  float exc = 0.0f;
  if (exc_pos_ < exc_total_ + pick_delay_) {
    // Pick-position comb: burst[n] - burst[n - pick_delay]. The delayed copy
    // runs pick_delay samples past the burst so the comb notches stay exact.
    float burst =
        exc_pos_ < exc_total_
            ? exc_noise_gain_ * noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(exc_pos_))
            : 0.0f;
    if (pick_delay_ > 0 && exc_pos_ >= pick_delay_) {
      burst -= exc_noise_gain_ *
               noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(exc_pos_ - pick_delay_));
    }
    ++exc_pos_;
    exc_lp_ += exc_alpha_ * (burst - exc_lp_);
    exc = 0.7f * exc_lp_;  // comb headroom
  }

  // pitch_ratio scales the frequency, so it divides the loop delay.
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  float loop_in = string_.add_feedback(exc);
  if (buzz_threshold_ > 0.0f && loop_in > buzz_threshold_) {
    // Buzzing bridge: displacement toward the curved surface is softly limited
    // once it grazes the threshold, so the flattened crest sprays energy into
    // the high partials each period (one-sided, memoryless). The rational
    // saturator keeps the crest strictly below threshold + span, so the loop
    // stays bounded; a stronger jawari leaves less give above the surface.
    const float over = loop_in - buzz_threshold_;
    const float span = kBuzzSpanBase * (1.0f - 0.5f * buzz_amount_);
    loop_in = buzz_threshold_ + span * over / (span + over);
  }

  const float out = string_.advance(loop_in, ratio);
  // Stiff-string dispersion (gated): the highs travel faster, stretching the partials sharp.
  float shaped = out;
  if (disp_a_ != 0.0f) {
    for (float& state : disp_state_) {
      const float y = disp_a_ * shaped + state;
      state = shaped - disp_a_ * y;
      shaped = y;
    }
  }
  string_.commit(shaped);
  float result = out * output_scale_;
  if (pick_.active()) {
    pick_.add_to(result, noise_.bipolar_at(kPickNoiseIndexBase + static_cast<uint64_t>(pick_.pos)));
  }
  return result;
}

void PluckedStringVoiceCore::release() noexcept { string_.release(); }

void PluckedStringVoiceCore::kill() noexcept {
  killed_ = true;
  decimator_.reset();
  exc_pos_ = exc_total_ + pick_delay_;
  string_.kill();
  pick_.pos = pick_.len;
  exc_lp_ = 0.0f;
}

}  // namespace sonare::midi::synth
