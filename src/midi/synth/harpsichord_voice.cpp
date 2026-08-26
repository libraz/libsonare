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

/// How long the tongue takes to let the string go, from a worn Delrin one to a
/// fresh quill, in milliseconds. It rounds the corners of the bridge-force
/// rectangle, so it rolls the top of the series off and leaves the rest alone.
///
/// Absolute rather than a share of the period, because the release is set by the
/// plectrum and the tension and not by the note. That is what makes one value
/// cut a bass string's series above its fiftieth partial and a treble string's
/// above its second — the references' partials 3-10 fall from -6 dB at FF to
/// -35 at f''', and a pitch-relative width holds them flat across the compass.
SONARE_TUNABLE(kReleaseWornMs, 0.55f);
SONARE_TUNABLE(kReleaseFreshMs, 0.06f);

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
/// up to where the rest of the bank sits. Re-measured once the board's radiation
/// took the voice 8.6 dB under the references across the phrase set — a stage
/// that attenuates has to be paid for somewhere, and this is where.
SONARE_TUNABLE(kOutputTrim, 7.0f);

/// How fast the board's diffuse field follows the strings driving it, as a time
/// constant in milliseconds, and the frequency it starts at — the board's
/// Schroeder frequency, below which its modes are separable and BodyType carries
/// them instead. A follow much shorter than a period tracks the waveform rather
/// than its energy and modulates the tone; much longer and the field outlives
/// the note it belongs to.
SONARE_TUNABLE(kDiffuseFollowMs, 145.0f);
SONARE_TUNABLE(kDiffuseSchroederHz, 1790.0f);

/// Where the board stops radiating what reaches it. A plate's radiation
/// efficiency levels off above its critical frequency while its internal losses
/// keep rising, so the diffuse field is a band and not a shelf — left open, it
/// fills 2-8 kHz with white noise and buries the treble partials that are
/// measured against it.
SONARE_TUNABLE(kDiffuseTopHz, 3270.0f);

/// The band the board's radiation efficiency climbs across: from above its first
/// modes, where the plate starts radiating as a plate, to coincidence for a thin
/// spruce one, above which the efficiency is already one and stops rising. The
/// tilt is flat outside it at both ends. Measured against the baroque slot this
/// band beats both a wider one and a narrower one, so it is the band the voice
/// wants and not only the band a plate would have.
SONARE_TUNABLE(kBoardTiltLoHz, 200.0f);
SONARE_TUNABLE(kBoardTiltHiHz, 4000.0f);

/// Below this the diffuse field is off rather than merely quiet, so the default
/// renders bit-identically to a voice that never had one.
constexpr float kDiffuseOffDb = -119.0f;

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
/// The diffuse field runs for the whole note rather than a burst, so its draws
/// start far enough above the bursts' to never reach them.
constexpr uint64_t kDiffuseNoiseBase = 1ull << 24;

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

/// One rounded corner of the bridge-force rectangle: 0 before the transition, 1
/// after it, a raised cosine of width @p w across it.
float edge_ramp(float x, float w) noexcept {
  const float half = 0.5f * w;
  if (x <= -half) return 0.0f;
  if (x >= half) return 1.0f;
  return 0.5f * (1.0f - std::cos(sonare::constants::kPi * (x / w + 0.5f)));
}

/// The bridge force an ideally plucked string produces, at sample @p k of one
/// period, before its DC trim: a rectangle standing at (1 - beta) while the kink
/// travels the short side of the pluck point and at -beta.image while it travels
/// the long one, with both corners rounded by the plectrum's release.
float bridge_force(float k, float beta, float duty, float image, float w) noexcept {
  const float back = beta * image;
  const float step = (1.0f - beta) + back;
  // Both corners are shifted by the same half-width, so rounding them leaves the
  // duty cycle — and with it the comb — exactly where the pluck point put it.
  const float rise = edge_ramp(k - 0.5f * w, w);
  const float fall = edge_ramp(k - duty - 0.5f * w, w);
  return step * (rise - fall) - back;
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
      choir.n_eff = 0.0f;
      choir.duty = 0.0f;
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
    // The span one period of bridge force is laid down over is the loop's own,
    // which is shorter than the ideal period by the feedback path and the loss
    // filter's phase delay. Laying it over the ideal one instead leaves the wave
    // meeting itself a sample or two early on the first wrap, which is a click.
    choir.n_eff = std::max(4.0f, choir_period - choir.loop.loop_comp);
    choir.duty = std::clamp(pluck_fraction, 0.02f, 0.5f) * choir.n_eff;
    choir.inject_len = static_cast<int>(choir.n_eff);
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
    // Its own t60, and an absolute one: the segment's length is set by the case,
    // so neither the note's decay nor the bass stretch applies to it. Its top
    // goes first, like any string's.
    const float rear_t60 = params.rear_decay_s > 0.0f ? params.rear_decay_s : t60;
    const float g0 = string_loop_gain_for(rear_period, sr, rear_t60);
    const float g_ref = string_loop_gain_for(rear_period, sr, rear_t60 * hf);
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
  const float release_ms = kReleaseWornMs + (kReleaseFreshMs - kReleaseWornMs) * edge;
  edge_w_ = std::max(1.0f, std::max(0.0f, release_ms) * 0.001f * static_cast<float>(sr));
  // Neither corner may reach the other. Once they overlap the rectangle stops
  // reaching its full height, and the note goes quiet rather than dull — which
  // is not what a slower release does: the plectrum lifts every string to the
  // same place whatever speed it lets go at, and the flat compass both captured
  // references hold is that fact. A treble string is where this binds, and its
  // binding is why the top of the compass is the darkest part of it.
  float narrowest = 0.0f;
  for (const Choir* c : {&eight_a_, &eight_b_, &four_}) {
    if (c->level <= 0.0f) continue;
    const float room = std::min(c->duty, c->n_eff - c->duty);
    narrowest = narrowest > 0.0f ? std::min(narrowest, room) : room;
  }
  if (narrowest > 0.0f) edge_w_ = std::min(edge_w_, 0.95f * narrowest);
  pluck_amp_ = std::pow(10.0f, plectrum_release_db(params, velocity) / 20.0f);
  pluck_image_ = std::clamp(params.end_reflection, 0.0f, 1.0f);
  pluck_pos_ = 0;
  exc_prev_ = 0.0f;

  // The mean of exactly the samples about to be injected, summed rather than
  // derived: the continuous rectangle is zero-mean by construction and the
  // sampling of it is not, and the loop has no path that removes what is left.
  // Once per note-on against a period of render calls, so the walk is cheap
  // where a per-sample blocker would sit on the fundamental's own decay.
  for (Choir* c : {&eight_a_, &eight_b_, &four_}) {
    c->dc_trim = 0.0f;
    if (c->level <= 0.0f || c->inject_len <= 0) continue;
    const float beta = c->duty / c->n_eff;
    double sum = 0.0;
    for (int i = 0; i < c->inject_len; ++i) {
      sum += bridge_force(static_cast<float>(i), beta, c->duty, pluck_image_, edge_w_);
    }
    c->dc_trim = static_cast<float>(sum / static_cast<double>(c->inject_len));
  }
  // Every drawn choir gets exactly one period of force, and the 4' choir's is
  // half the 8' choirs'; the counter runs until the longest of them is done.
  pluck_span_ = std::max({eight_a_.inject_len, eight_b_.inject_len, four_.inject_len});

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

  // Where the board stops radiating at all: below its own size it moves air
  // without launching a wave, so a bass string reaches the room through its
  // partials and not through its fundamental.
  const float radiate_hz = std::max(0.0f, params.board_radiating_from_hz);
  hp_on_ = radiate_hz > 0.0f;
  if (hp_on_) {
    const float fc = std::min(radiate_hz, 0.45f * static_cast<float>(sr));
    const float w0 = kTwoPi * fc / static_cast<float>(sr);
    for (int i = 0; i < kRadiationStages; ++i) {
      hp_[i].set(rt::rbj_highpass(w0, rt::butterworth_stage_q(2 * kRadiationStages, i)));
      hp_[i].reset();
    }
  }

  // The board's radiation efficiency. A first-order section can only hold
  // 6 dB/oct, so a gentler slope is built by spreading three of them across the
  // band and giving each a pole/zero ratio of the band's cube root raised to the
  // slope's share of first order. Off at zero tilt, and skipped there.
  const float tilt_db_oct = std::clamp(params.board_tilt_db_oct, 0.0f, 6.0f);
  tilt_on_ = tilt_db_oct > 0.0f;
  if (tilt_on_) {
    const float lo = std::clamp(kBoardTiltLoHz, 10.0f, 0.2f * static_cast<float>(sr));
    const float hi = std::clamp(kBoardTiltHiHz, lo * 2.0f, 0.45f * static_cast<float>(sr));
    const float span = std::pow(hi / lo, 1.0f / static_cast<float>(kTiltSections));
    // 6.0206 dB/oct is one first-order section's whole slope; the ratio below is
    // how much of one this tilt asks for.
    const float share = tilt_db_oct / 6.0206f;
    const float pole_ratio = std::pow(span, share);
    for (int i = 0; i < kTiltSections; ++i) {
      const float zero_hz = lo * std::pow(span, static_cast<float>(i));
      const float pole_hz = std::min(zero_hz * pole_ratio, 0.45f * static_cast<float>(sr));
      TiltSection& s = tilt_[i];
      s.zero = std::exp(-kTwoPi * zero_hz / static_cast<float>(sr));
      s.pole = std::exp(-kTwoPi * pole_hz / static_cast<float>(sr));
      // Unity ABOVE the band, where a radiation efficiency reaches one and stops.
      // Normalised at DC instead the cascade is not a tilt at all but a
      // broadband boost — same shape, and every partial louder: it put the
      // voice's peak 13 dB over the references on all nine phrase takes while
      // every level-blind measure on the note grid reported it unchanged.
      s.g = (1.0f + s.pole) / std::max(1.0e-9f, 1.0f + s.zero);
      s.x1 = 0.0f;
      s.y1 = 0.0f;
    }
  }

  // The soundboard's diffuse field. Off by default and skipped entirely there,
  // so a patch that does not ask for one renders exactly as it did before.
  diffuse_level_ = params.board_diffuse_db <= kDiffuseOffDb
                       ? 0.0f
                       : std::pow(10.0f, std::min(0.0f, params.board_diffuse_db) / 20.0f);
  diffuse_env_ = 0.0f;
  diffuse_lp_ = 0.0f;
  diffuse_top_ = 0.0f;
  diffuse_age_ = 0;
  const float follow_s = std::max(0.001f, kDiffuseFollowMs) * 0.001f;
  diffuse_follow_ = 1.0f - std::exp(-1.0f / (follow_s * static_cast<float>(sr)));
  diffuse_tilt_ = onepole_alpha(kDiffuseSchroederHz, sr);
  diffuse_top_a_ = onepole_alpha(std::max(kDiffuseTopHz, kDiffuseSchroederHz), sr);

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

  const bool plucking = pluck_pos_ < pluck_span_;
  const float k = static_cast<float>(pluck_pos_);
  if (plucking) ++pluck_pos_;

  // One period of the force an ideally plucked string puts on its bridge: a
  // rectangle that stands at (1 - beta) while the kink travels the short side of
  // the pluck point and at -beta while it travels the long one. Its partials are
  // sin(n.pi.beta)/n, so the plucking-point comb and the 1/n envelope are one
  // shape rather than a comb and an envelope chosen apart from each other.
  //
  // Injecting exactly one period into an empty loop loads it, which is why this
  // runs once and stops rather than driving the string continuously.
  auto excite = [&](const Choir& choir) noexcept -> float {
    if (!plucking || choir.level <= 0.0f || k >= static_cast<float>(choir.inject_len)) return 0.0f;
    const float beta = choir.duty / choir.n_eff;
    return pluck_amp_ * (bridge_force(k, beta, choir.duty, pluck_image_, edge_w_) - choir.dc_trim);
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

  // What crosses the bridge is the force's transient, not the force: the
  // rectangle stands at a level for most of a period, and a level fed to a loop
  // this short and this lightly damped is a tone rather than a strike.
  const float exc_slew = bridge_exc - exc_prev_;
  exc_prev_ = bridge_exc;

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
    result += rear_level_ * rear_.process(rear_drive_ * exc_slew + rear_.feedback(), ratio);
  }

  if (hp_on_) {
    for (rt::BiquadState& s : hp_) result = s.process(result);
  }

  if (tilt_on_) {
    // Everything the bridge carries goes out through the board, so the strings
    // and the segment behind them are tilted together. The diffuse field below
    // is already radiated and is added after.
    for (int i = 0; i < kTiltSections; ++i) {
      TiltSection& s = tilt_[i];
      const float y = s.g * (result - s.zero * s.x1) + s.pole * s.y1;
      s.x1 = result;
      s.y1 = y;
      result = y;
    }
  }

  if (diffuse_level_ > 0.0f) {
    // The board radiating what it cannot resolve: the field follows the strings'
    // energy, so it fills the space between their partials while they sound and
    // is gone when they are. Subtracting the lowpass leaves the band above the
    // board's Schroeder frequency, which is where a mode field stops being a set
    // of resonances; the second pole closes it above the critical frequency,
    // where the board stops radiating what reaches it.
    diffuse_env_ += diffuse_follow_ * (std::fabs(bridge) - diffuse_env_);
    const float nz = noise_.bipolar_at(kDiffuseNoiseBase + diffuse_age_);
    diffuse_lp_ += diffuse_tilt_ * (nz - diffuse_lp_);
    diffuse_top_ += diffuse_top_a_ * ((nz - diffuse_lp_) - diffuse_top_);
    result += diffuse_level_ * diffuse_env_ * diffuse_top_;
    ++diffuse_age_;
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
  exc_prev_ = 0.0f;
  for (TiltSection& s : tilt_) {
    s.x1 = 0.0f;
    s.y1 = 0.0f;
  }
  for (rt::BiquadState& s : hp_) s.reset();
  eight_a_.loop.kill();
  eight_b_.loop.kill();
  four_.loop.kill();
  rear_.kill();
  chiff_pos_ = chiff_len_;
  jack_pos_ = jack_len_;
  diffuse_env_ = 0.0f;
  diffuse_lp_ = 0.0f;
  diffuse_top_ = 0.0f;
}

}  // namespace sonare::midi::synth
