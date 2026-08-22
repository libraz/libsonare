#include "midi/synth/harpsichord_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kTwoPi;

/// How the plectrum's release displacement grows with key speed below its peak.
/// The instrument's whole dynamic is a few dB wide, so the shape inside that
/// span matters far less than the span itself; this is the compressed rise a
/// captured 8' reference shows, fitted as a power of normalized key speed. It is
/// NOT a reproduction of any particular reference's curve — a sampler's velocity
/// spline is that instrument's, and the span is what the literature measures.
SONARE_TUNABLE(kPlectrumRise, 2.0f);

/// The plectrum's contact patch, as a fraction of the loop period, from a worn
/// tongue (wide, dull) to a fresh quill (narrow, bright).
SONARE_TUNABLE(kContactWide, 0.55f);
SONARE_TUNABLE(kContactNarrow, 0.06f);

/// The chiff of the plectrum leaving the string, and the two note-off events:
/// the jack falling back past the string, then the felt arriving. They are
/// separate sounds a few milliseconds apart, and collapsing them into one thump
/// is audible as a click where the instrument has a mechanism.
SONARE_TUNABLE(kChiffMs, 4.0f);
SONARE_TUNABLE(kChiffCutoffHz, 5200.0f);
SONARE_TUNABLE(kJackMs, 26.0f);
SONARE_TUNABLE(kJackCutoffHz, 1500.0f);
SONARE_TUNABLE(kDamperDelayMs, 7.0f);

/// The raw string sample is a displacement, not a level; this brings a drawn 8'
/// up to where the rest of the bank sits.
SONARE_TUNABLE(kOutputTrim, 2.6f);

/// The behind-the-bridge segment can never approach the speaking length, however
/// short the string gets: the hitch-pin rail converges toward the bridge in the
/// treble rather than staying a fixed distance from it. The cap is deliberately
/// not the reciprocal of a whole number — at the top of the compass every note
/// sits on it, and a segment pinned to an exact harmonic would add no inharmonic
/// content at all, which is the one thing it is there for.
SONARE_TUNABLE(kRearMaxRatio, 0.17f);

/// Noise draws live far above the voice-level draw indices (detune / phase /
/// drift use 0..~103 on the same per-voice seed), and the two mechanism bursts
/// sit far enough apart never to share a draw.
constexpr uint64_t kChiffNoiseBase = 1ull << 16;
constexpr uint64_t kJackNoiseBase = 1ull << 20;

/// The speaking length of a string in millimetres.
///
/// Only the behind-the-bridge segment needs this: that segment's length is set
/// by the case, so how many times it divides into the speaking length — and so
/// how far off the harmonic series its modes fall — changes right across the
/// keyboard. Pitch does not come from here.
///
/// A harpsichord's scale is close to Pythagorean (the length doubling every
/// octave down) through the treble and middle, and foreshortened below, because
/// no case is long enough to keep doubling: an unforeshortened bass would want
/// strings several metres long.
float speaking_length_mm(const HarpsichordPatchParams& params, uint8_t note) noexcept {
  // Octaves below c'' (MIDI 72), the note the scale is quoted at.
  const float octaves = (72.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float fore = std::clamp(params.bass_foreshortening, 0.0f, 0.9f);
  const float exponent = octaves <= 1.0f ? octaves : 1.0f + (octaves - 1.0f) * (1.0f - fore);
  return std::max(20.0f, params.scale_c5_mm) * std::exp2(exponent);
}

/// The plectrum's release displacement for a key speed, in dB relative to its
/// own peak (so never above 0).
float plectrum_release_db(const HarpsichordPatchParams& params, uint8_t velocity) noexcept {
  constexpr float kSoftest = 1.0f / 127.0f;
  const float v = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float peak = std::max(kSoftest, static_cast<float>(params.peak_velocity & 0x7Fu) / 127.0f);
  const float range = std::max(0.0f, params.velocity_range_db);
  if (v <= peak) {
    const float t = std::clamp((v - kSoftest) / std::max(1.0e-6f, peak - kSoftest), 0.0f, 1.0f);
    return -range * (1.0f - std::pow(t, kPlectrumRise));
  }
  // Past the peak the plectrum slips off the string sooner, so a faster key
  // gives a SMALLER release displacement. The droop is quadratic in the overrun
  // because it is a loss of contact, not a gain law.
  const float over = (v - peak) / std::max(1.0e-6f, 1.0f - peak);
  return -std::max(0.0f, params.velocity_droop_db) * over * over;
}

float onepole_alpha(float cutoff_hz, double sample_rate) noexcept {
  return std::clamp(1.0f - std::exp(-kTwoPi * cutoff_hz / static_cast<float>(sample_rate)), 0.01f,
                    1.0f);
}

}  // namespace

void HarpsichordVoiceCore::start(const HarpsichordPatchParams& params, double sample_rate,
                                 uint8_t note, uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);

  const float f0 = note_to_hz(note);
  const float period = static_cast<float>(sr) / f0;

  // The decay the fundamental is asked for, and the one the octave above it
  // gets. Both are per-traversal gains, which is the form the loss filter is
  // solved in.
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 2.0f);
  const float t60 = std::max(0.05f, params.decay_s) * std::exp2(stretch * octaves_below_a4);
  const float hf = std::clamp(params.hf_damping, 0.05f, 1.0f);
  const float damper_t60 = std::max(0.005f, params.damper_s);
  // The frequency the HF decay is quoted at, kept clear of the fundamental (a
  // reference at or below it has no tilt to describe) and of Nyquist.
  const float ref_hz = std::clamp(params.damping_ref_hz, 2.0f * f0, 0.45f * static_cast<float>(sr));
  const float omega_ref = kTwoPi * ref_hz / static_cast<float>(sr);

  const float detune_8b =
      std::exp2(std::clamp(params.unison_detune_cents, -50.0f, 50.0f) / 1200.0f);
  const float detune_4 = std::exp2(std::clamp(params.octave_detune_cents, -50.0f, 50.0f) / 1200.0f);

  // Voice one choir: solve its loss filter against the two decay targets at ITS
  // period, then hand the solved (a, g) straight to the loop.
  auto voice_choir = [&](Choir& choir, float* span, int span_capacity, float choir_period,
                         bool drawn, float pluck_fraction, bool damped) noexcept {
    if (!drawn || slab_ == nullptr) {
      choir.loop.disable();
      choir.level = 0.0f;
      choir.pluck_delay = 0;
      choir.damped = true;
      return;
    }
    const float g0 = string_loop_gain_for(choir_period, sr, t60);
    const float g_ref = string_loop_gain_for(choir_period, sr, t60 * hf);
    const StringLoopFilter filter =
        solve_string_loop_filter(kTwoPi / choir_period, omega_ref, g0, g_ref);
    // The damper is broadband, so its per-traversal gain is compensated by the
    // same factor the fundamental's was; otherwise the pole's own attenuation
    // would be counted into the damping a second time.
    const float compensation = g0 > 0.0f ? filter.g / g0 : 1.0f;
    const float release_g =
        std::min(0.9999f, string_loop_gain_for(choir_period, sr, damper_t60) * compensation);
    choir.loop.configure_filter(span, span_capacity, choir_period, filter.a, filter.g, release_g);
    choir.level = 1.0f;
    choir.pluck_delay =
        static_cast<int>(std::clamp(pluck_fraction, 0.0f, 0.5f) * choir_period + 0.5f);
    choir.damped = damped;
  };

  const int full = capacity_;
  float* span_8a = slab_;
  float* span_8b = slab_ != nullptr ? slab_ + full : nullptr;
  float* span_4 = slab_ != nullptr ? slab_ + 2 * full : nullptr;
  float* span_rear = slab_ != nullptr ? slab_ + 2 * full + full / 2 : nullptr;

  voice_choir(eight_a_, span_8a, full, period, params.eight_a, params.pluck_8a, true);
  voice_choir(eight_b_, span_8b, full, period * detune_8b, params.eight_b, params.pluck_8b, true);
  // The top of the 4' choir carries no dampers on a real instrument, so above
  // the break those strings go on sounding after the key is released.
  voice_choir(four_, span_4, full / 2, 0.5f * period * detune_4, params.four, params.pluck_4,
              (note & 0x7Fu) < params.undamped_from_note);

  // The string behind the bridge. Its length is fixed by the case while the
  // speaking length is not, so the ratio between them — and with it how far the
  // segment's modes sit off the harmonic series — moves right across the
  // keyboard. That is the whole point of it.
  const float rear_mm = std::max(0.0f, params.rear_segment_mm);
  rear_level_ = 0.0f;
  rear_drive_ = 0.0f;
  if (rear_mm > 0.0f && span_rear != nullptr) {
    const float ratio =
        std::clamp(rear_mm / speaking_length_mm(params, note), 0.004f, kRearMaxRatio);
    const float rear_period = std::max(4.0f, period * ratio);
    // Undamped and short: it rings well past the note. Its top goes first, like
    // any string's.
    const float g0 = string_loop_gain_for(rear_period, sr, t60);
    const float g_ref = string_loop_gain_for(rear_period, sr, t60 * hf);
    const StringLoopFilter filter = solve_string_loop_filter(
        kTwoPi / rear_period, std::max(omega_ref, kTwoPi * 2.0f / rear_period), g0, g_ref);
    rear_.configure_filter(span_rear, full / 8, rear_period, filter.a, filter.g, filter.g);
    // How loudly the segment answers scales with how much of the string it is.
    // A bass segment is a twentieth of its string and a treble one nearly a
    // fifth, and holding the coupling fixed makes the bass halo louder than the
    // note it is behind — the bridge moves slowly there and a short stiff
    // segment neither takes much from it nor radiates much of what it takes.
    rear_level_ = std::clamp(params.rear_coupling, 0.0f, 1.0f) * (ratio / kRearMaxRatio);
    rear_drive_ = rear_level_;
  } else {
    rear_.disable();
  }

  // The plectrum. Its release displacement is what the key speed buys, and that
  // is nearly nothing — the whole span is a few dB.
  const float edge = std::clamp(params.plectrum_edge, 0.0f, 1.0f);
  const float contact = kContactWide + (kContactNarrow - kContactWide) * edge;
  pluck_len_ = std::max(4, static_cast<int>(contact * period));
  pluck_amp_ = std::pow(10.0f, plectrum_release_db(params, velocity) / 20.0f);
  pluck_pos_ = 0;
  // The pulse has to keep running until the last jack's combed copy has been
  // injected, or that choir loses the second lobe of its doublet.
  const int longest_comb =
      std::max({eight_a_.pluck_delay, eight_b_.pluck_delay, four_.pluck_delay});
  pluck_span_ = pluck_len_ + longest_comb;

  // Mechanism. Both bursts are inert at 0 and are skipped rather than scaled.
  chiff_amount_ = std::clamp(params.pluck_noise, 0.0f, 1.0f);
  chiff_len_ = std::max(1, static_cast<int>(kChiffMs * 0.001f * static_cast<float>(sr)));
  chiff_pos_ = chiff_amount_ > 0.0f ? 0 : chiff_len_;
  chiff_lp_ = 0.0f;
  chiff_alpha_ = onepole_alpha(kChiffCutoffHz, sr);
  jack_amount_ = std::clamp(params.jack_noise, 0.0f, 1.0f);
  jack_len_ = std::max(1, static_cast<int>(kJackMs * 0.001f * static_cast<float>(sr)));
  jack_pos_ = jack_len_;  // inactive until release()
  jack_lp_ = 0.0f;
  jack_alpha_ = onepole_alpha(kJackCutoffHz, sr);
  damper_delay_ = std::max(1, static_cast<int>(kDamperDelayMs * 0.001f * static_cast<float>(sr)));

  // Level is set by the mechanism, not by the note: the plectrum lifts every
  // string to the same place, so a harpsichord's peak level is flat across the
  // compass (a captured reference holds within 3.7 dB over five octaves). The
  // drawn choirs share the output rather than adding to it, so drawing a second
  // 8' thickens the sound without making it twice as loud.
  const float drawn = eight_a_.level + eight_b_.level + four_.level;
  output_scale_ = kOutputTrim / std::max(1.0f, std::sqrt(std::max(1.0f, drawn)));
}

float HarpsichordVoiceCore::render(float pitch_ratio) noexcept {
  if (slab_ == nullptr) return 0.0f;

  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // The plectrum's release pulse, evaluated in closed form so each jack can comb
  // it at its own plucking point.
  auto pluck_at = [this](int k) noexcept -> float {
    if (k < 0 || k >= pluck_len_) return 0.0f;
    const float win = 0.5f * (1.0f - std::cos(kTwoPi * (static_cast<float>(k) + 1.0f) /
                                              static_cast<float>(pluck_len_ + 1)));
    // Zero-mean: the string is lifted and let go, not pushed. A pulse with a DC
    // component would charge the loop instead of exciting it.
    return (k < pluck_len_ / 2) ? win : -win;
  };
  const bool plucking = pluck_pos_ < pluck_span_;
  const int k = pluck_pos_;
  if (plucking) ++pluck_pos_;

  auto excite = [&](const Choir& choir) noexcept -> float {
    if (!plucking || choir.level <= 0.0f) return 0.0f;
    // The plucking-point comb: the string is driven at one point, so the
    // harmonics with a node there are not driven at all.
    return pluck_amp_ * (pluck_at(k) - pluck_at(k - choir.pluck_delay));
  };

  float bridge = 0.0f;
  float bridge_exc = 0.0f;
  if (eight_a_.level > 0.0f) {
    const float e = excite(eight_a_);
    bridge_exc += e;
    bridge += eight_a_.loop.process(e + eight_a_.loop.feedback(), ratio);
  }
  if (eight_b_.level > 0.0f) {
    const float e = excite(eight_b_);
    bridge_exc += e;
    bridge += eight_b_.loop.process(e + eight_b_.loop.feedback(), ratio);
  }
  if (four_.level > 0.0f) {
    // The 4' choir sounds an octave up, so its own loop runs at half the period;
    // the shared pitch factor still applies to it.
    const float e = excite(four_);
    bridge_exc += e;
    bridge += four_.loop.process(e + four_.loop.feedback(), ratio);
  }

  float result = bridge;

  if (rear_level_ > 0.0f) {
    // Behind the bridge: the pluck's energy crosses the bridge into this segment
    // as the string is released, and after that the segment rings on its own.
    //
    // It is excited by the pluck rather than driven by the string's continuous
    // output, which is not a simplification but the difference between a halo
    // and a howl: this loop is short and barely damped, so its resonant gain is
    // in the hundreds, and feeding it a broadband signal every sample builds up
    // a tone louder than the note. It never drives back into the speaking
    // string either — that path is real but weak, and leaving it out keeps a
    // high-Q loop out of the string's feedback path entirely.
    result += rear_level_ * rear_.process(rear_drive_ * bridge_exc + rear_.feedback(), ratio);
  }

  if (chiff_pos_ < chiff_len_) {
    // The plectrum scraping off the string, in front of the tone.
    const float nz = noise_.bipolar_at(kChiffNoiseBase + static_cast<uint64_t>(chiff_pos_));
    chiff_lp_ += chiff_alpha_ * (nz - chiff_lp_);
    const float env = 1.0f - static_cast<float>(chiff_pos_) / static_cast<float>(chiff_len_);
    result += chiff_amount_ * env * env * chiff_lp_;
    ++chiff_pos_;
  }

  if (jack_pos_ < jack_len_) {
    // Note-off: the tongue pivoting past the string, then the felt landing on
    // it. Two events, not one — the second is the louder and the duller.
    const float nz = noise_.bipolar_at(kJackNoiseBase + static_cast<uint64_t>(jack_pos_));
    jack_lp_ += jack_alpha_ * (nz - jack_lp_);
    const float t = static_cast<float>(jack_pos_) / static_cast<float>(jack_len_);
    float env = (1.0f - t) * (1.0f - t) * 0.45f;
    if (jack_pos_ >= damper_delay_) {
      const float u = static_cast<float>(jack_pos_ - damper_delay_) /
                      static_cast<float>(std::max(1, jack_len_ - damper_delay_));
      env += (1.0f - u) * (1.0f - u);
    }
    result += jack_amount_ * env * jack_lp_;
    ++jack_pos_;
  }

  return output_scale_ * result;
}

void HarpsichordVoiceCore::release() noexcept {
  // The felt reaches whichever choirs have dampers. The 4' top has none, and the
  // string behind the bridge never had any.
  if (eight_a_.damped) eight_a_.loop.release();
  if (eight_b_.damped) eight_b_.loop.release();
  if (four_.damped) four_.loop.release();
  if (jack_amount_ > 0.0f) {
    jack_pos_ = 0;
    jack_lp_ = 0.0f;
  }
}

void HarpsichordVoiceCore::kill() noexcept {
  pluck_pos_ = pluck_span_;
  eight_a_.loop.kill();
  eight_b_.loop.kill();
  four_.loop.kill();
  rear_.kill();
  chiff_pos_ = chiff_len_;
  jack_pos_ = jack_len_;
}

}  // namespace sonare::midi::synth
