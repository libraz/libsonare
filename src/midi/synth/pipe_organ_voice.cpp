#include "midi/synth/pipe_organ_voice.h"

#include <algorithm>
#include <cmath>

#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Mouth-pressure calibration (shared with the flute jet). The exposed band lands
// the jet in its self-oscillating region — the knobs colour the tone, they do
// not gate it on and off; dynamic LOUDNESS rides the voice's amp VCA. The jet
// self-oscillates where the cubic's slope makes the loop gain exceed one: the
// high-pressure band, not the near-zero-slope low-pressure region.
constexpr float kBreathBase = 0.80f;
constexpr float kBreathSpan = 0.35f;

// Jet delay / bore-line ratio: ~0.5 drives an open pipe's fundamental (every
// rank uses the open topology, so a single ratio serves the whole registration).
constexpr float kJetRatioOpen = 0.5f;

// Reflection coefficients (the two feedback taps). Clamped below the runaway
// region; the STK-stable operating point is ~0.5 each.
constexpr float kReflectMax = 0.62f;

// Open-end reflection lowpass corner as a MULTIPLE of the sounding f0 (not a
// fixed absolute pole): the harmonic damping must scale with pitch or a low
// pedal note overblows to the third mode (the fixed-pole flute trick works only
// over the flute's octaves, not the organ's 16'-down range). brightness lifts
// the corner so a bright principal reflects more upper partials. Below ~1.5*f0
// the fundamental itself would be damped and the pipe would not speak.
constexpr float kReflectCornerBase = 1.6f;
constexpr float kReflectCornerSpan = 3.4f;

// Pitch correction: the jet+bore lock lands a touch off the naive loop, so the
// loop delay is trimmed to bring the sounding note onto pitch (probe-calibrated
// across the compass; the DC-blocker phase is compensated separately below).
constexpr float kPitchCorrectOpen = 1.0012f;

// In-loop DC-blocker corner (~10 Hz): the jet's rectified DC does not radiate and
// would charge the bore.
constexpr float kDcCornerHz = 10.0f;

// Even-harmonic pump: the asymmetric offset jet voices the octave the way an
// open flue pipe's spectrum is octave-rich. A stopped rank keeps only a trace
// (the gedackt is fundamental-dominant, nearly free of the octave).
constexpr float kEvenPumpGain = 0.6f;
constexpr float kEvenPumpStopped = 0.08f;
constexpr float kEvenPumpDcHz = 30.0f;

// Reed (lingual) voicing: a reed pipe drives the jet harder and more
// asymmetrically than a smooth flue labium, so it buzzes with a bright, brassy,
// harmonic-rich spectrum. Both self-limit through the cubic clamp.
constexpr float kReedAsym = 0.55f;
constexpr float kReedDrive = 0.5f;

// Mouth/radiation high-shelf corner (Hz): the absolute frequency above which a
// pipe radiates efficiently. Fixed in absolute terms (a room-coupling property,
// not a per-note one).
constexpr float kRadiationCornerHz = 800.0f;
constexpr float kRadiationLift = 2.5f;

// Chiff onset burst depth.
constexpr float kChiffGain = 0.5f;
// Bore pre-fill: a low-level seed so the jet has an f0 component to lock onto.
constexpr float kBorePrefill = 0.05f;

// Output trim: the driven jet loop settles with a raw bore peak that falls with
// pitch, so the per-pipe output is frequency-compensated toward a flat target
// peak before the chorus sum. peak_raw ~= kPeakBase + kPeakTilt*log2(f0/kPeakRefHz).
constexpr float kOutputTargetPeak = 0.32f;
constexpr float kPeakBase = 4.0f;
constexpr float kPeakTilt = -0.65f;
constexpr float kPeakRefHz = 261.63f;

// Determinism: chiff draw base; a per-rank offset in the high bits separates the
// ranks (kRankNoiseShift), and the pre-fill uses a separate base.
constexpr uint64_t kFillIndexBase = 1ull << 16;
constexpr uint64_t kChiffIndexBase = 1ull << 24;
constexpr uint64_t kRankNoiseShift = 48;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

/// The jet function: the S-shaped saturating transfer of the air jet deflecting
/// across the labium (Fabre-Hirschberg lumped model / STK JetTable). The odd
/// cubic's small-signal slope is inverting near zero (the oscillator drive); the
/// @p asym offset seeds the even harmonics; the clamp bounds the limit cycle.
float jet_table(float x, float asym) noexcept {
  const float y = x * (x * x - 1.0f) + asym * x * x;
  return y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
}

}  // namespace

void PipeOrganVoiceCore::start(const PipeOrganPatchParams& params, double sample_rate, uint8_t note,
                               uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float srf = static_cast<float>(sr);
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;
  releasing_ = false;
  breath_level_ = 0.0f;

  // Resolve the registration: an explicit rank list, or a single implicit 8'
  // rank built from the flat {stopped, brightness}.
  PipeOrganRank implicit{};
  implicit.footage_mult = 1.0f;
  implicit.stopped = params.stopped;
  implicit.brightness = params.brightness;
  implicit.level = 1.0f;
  implicit.reed = params.reed;
  implicit.radiation = params.radiation;
  const PipeOrganRank* ranks = &implicit;
  int count = 1;
  if (params.rank_count > 0) {
    ranks = params.ranks.data();
    count = std::min(params.rank_count, kMaxPipeRanks);
  }
  rank_count_ = count;

  // Chorus normalisation: decorrelated pipes add in power, so divide by
  // sqrt(sum level^2) to hold the stop at a single-pipe loudness.
  float power = 0.0f;
  for (int r = 0; r < count; ++r) {
    const float lvl = std::clamp(ranks[r].level, 0.0f, 1.0f);
    power += lvl * lvl;
  }
  const float norm = 1.0f / std::sqrt(std::max(1.0e-6f, power));

  const float base_f0 = note_to_hz(note);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  // Mouth pressure: the patch breath sets the dynamic, velocity opens it a touch.
  const float level =
      std::clamp(0.7f * std::clamp(params.breath, 0.0f, 1.0f) + 0.3f * vel01, 0.0f, 1.0f);
  const float mouth = kBreathBase + kBreathSpan * level;

  // Wind gate contour: a quick speak on, a ring-down on release.
  attack_coeff_ = ramp_coeff(8.0f, sr);
  release_coeff_ = ramp_coeff(std::max(0.01f, params.release_damp_s) * 1000.0f, sr);

  // Bore purity: tone_decay_s maps to the bore loss (a purer, more sustained,
  // sharply pitched pipe reflects with less loss).
  const float purity = std::clamp(params.tone_decay_s / 8.0f, 0.0f, 1.0f);
  const float loss_gain = std::clamp(0.945f + 0.05f * purity, 0.5f, 0.998f);

  const float dc_r = 1.0f - static_cast<float>(kTwoPi * kDcCornerHz / sr);
  const float even_hp = std::clamp(1.0f - std::exp(-kTwoPi * kEvenPumpDcHz / srf), 0.0f, 1.0f);

  for (int r = 0; r < count; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    pipe.bore = slab_ != nullptr ? slab_ + static_cast<size_t>(2 * r) * span_capacity_ : nullptr;
    pipe.jet = slab_ != nullptr ? slab_ + static_cast<size_t>(2 * r + 1) * span_capacity_ : nullptr;
    pipe.noise_offset = static_cast<uint64_t>(r) << kRankNoiseShift;
    pipe.mix = std::clamp(ranks[r].level, 0.0f, 1.0f) * norm;

    const bool stopped = ranks[r].stopped;
    const float footage = ranks[r].footage_mult > 0.01f ? ranks[r].footage_mult : 1.0f;
    const float f0 = base_f0 * footage;
    const float period = srf / std::max(1.0f, f0);
    // Every rank is a positive-feedback open jet pipe (it locks its fundamental
    // and tunes cleanly across the whole 16'-down compass, where the alternative
    // negative-feedback half-length stopped comb would not hold pitch). A STOPPED
    // (gedackt/bourdon) rank is voiced as that open pipe made hollow: the
    // even-harmonic pump is nearly muted and the reflection is darkened, so the
    // tone is fundamental-dominant with very little upperwork — the covered,
    // flute-like colour of a stopped pipe, without the odd-only comb's tuning
    // instability.
    pipe.bore_period = period * kPitchCorrectOpen;
    pipe.sign = 1.0f;
    pipe.jet_ratio = kJetRatioOpen;

    // Reed voicing: drive the jet harder and more asymmetrically for a buzzing
    // lingual stop; a flue pipe (reed == 0) keeps the smooth symmetric labium.
    const float reed = std::clamp(ranks[r].reed, 0.0f, 1.0f);
    pipe.jet_asym = 0.5f + kReedAsym * reed;
    pipe.jet_drive = 1.0f + kReedDrive * reed;
    pipe.jet_reflection = std::min(0.5f + 0.12f * reed, kReflectMax);
    pipe.end_reflection = 0.5f;
    pipe.loss_gain = loss_gain;
    // Note-off bore loss: a per-loop gain reaching -60 dB in release_damp_s so the
    // pipe stops speaking promptly once the wind is cut.
    const float rel_t60 = std::max(0.02f, params.release_damp_s);
    const float loops_to_t60 = srf * rel_t60 / std::max(1.0f, period);
    pipe.release_loss = std::min(loss_gain, std::exp(-6.907755279f / std::max(1.0f, loops_to_t60)));

    // Open-end reflection lowpass: a one-pole whose corner tracks the pitch
    // (corner = corner_mult * f0) so the harmonic damping is consistent across
    // the whole compass and a low pedal note does not overblow. A brighter end
    // (and a reed's harmonic-rich buzz) lifts the corner; a stopped rank is
    // darkened toward the hollow gedackt tone.
    float bright = std::clamp(ranks[r].brightness + 0.3f * reed, 0.0f, 1.0f);
    if (stopped) bright = std::min(bright, 0.35f);
    const float corner = (kReflectCornerBase + kReflectCornerSpan * bright) * f0;
    const float alpha = std::clamp(1.0f - std::exp(-kTwoPi * corner / srf), 0.05f, 1.0f);
    const float a = 1.0f - alpha;
    pipe.lp_alpha = alpha;
    pipe.lp_state = 0.0f;

    pipe.dc_x1 = 0.0f;
    pipe.dc_y1 = 0.0f;
    pipe.dc_r = dc_r;
    pipe.bore_out = 0.0f;

    // Even-harmonic pump: the octave-rich open flue colour, nearly muted for a
    // stopped rank (a gedackt is fundamental-dominant, not octave-rich).
    pipe.even_gain = stopped ? kEvenPumpStopped : kEvenPumpGain;
    pipe.even_state = 0.0f;
    pipe.even_hp_alpha = even_hp;

    // Tuning compensation at the SOUNDING fundamental (f0): one feedback
    // register, the reflection lowpass's phase delay, and the in-loop DC
    // blocker's phase LEAD (which shortens the effective loop and sounds a low
    // pedal note sharp if left uncompensated — H_dc = (1 - z^-1)/(1 - r z^-1)).
    const float omega = kTwoPi * f0 / srf;
    const float tau_lp =
        std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
    const float phase_dc = std::atan2(std::sin(omega), 1.0f - std::cos(omega)) -
                           std::atan2(dc_r * std::sin(omega), 1.0f - dc_r * std::cos(omega));
    const float tau_dc = phase_dc / std::max(omega, 1.0e-6f);
    pipe.comp = 1.0f + tau_lp - tau_dc;

    pipe.breath = mouth;

    // Chiff onset burst (post-loop bright noise).
    pipe.chiff_level = std::clamp(params.chiff, 0.0f, 1.0f) * kChiffGain;
    pipe.chiff_coeff = std::exp(
        -1.0f / std::max(1.0f, static_cast<float>(std::max(0.5f, params.chiff_ms) * 0.001 * sr)));

    // Mouth/radiation high-shelf (post-loop, outside the loop).
    const float radiation = std::clamp(ranks[r].radiation, 0.0f, 1.0f);
    pipe.rad_gain = radiation * kRadiationLift;
    pipe.rad_alpha = std::clamp(1.0f - std::exp(-kTwoPi * kRadiationCornerHz / srf), 0.0f, 1.0f);
    pipe.rad_state = 0.0f;

    // Output trim (frequency-compensated toward a flat target peak).
    const float peak_est =
        std::clamp(kPeakBase + kPeakTilt * std::log2(std::max(1.0f, f0) / kPeakRefHz), 0.8f, 6.0f);
    pipe.output_scale = kOutputTargetPeak / peak_est;

    // Circular spans: the loop period plus bend-down headroom and the
    // interpolator stencil margin. The jet span reuses the same size.
    const float eff = std::max(2.0f, pipe.bore_period - pipe.comp);
    const int span = std::min(span_capacity_, std::max(16, static_cast<int>(eff * 1.15f) + 8));
    pipe.bore_size = span;
    pipe.jet_size = span;
    pipe.bore_write = 0;
    pipe.jet_write = 0;

    // Pre-fill the bore with a low-level seed so the jet locks promptly; the jet
    // span starts silent.
    if (pipe.bore != nullptr) {
      const float pf = kBorePrefill * mouth;
      for (int i = 0; i < span; ++i) {
        pipe.bore[static_cast<size_t>(i)] =
            pf * noise_.bipolar_at(kFillIndexBase + pipe.noise_offset + static_cast<uint64_t>(i));
      }
    }
    if (pipe.jet != nullptr) {
      for (int i = 0; i < span; ++i) pipe.jet[static_cast<size_t>(i)] = 0.0f;
    }
  }
  drive_index_ = static_cast<uint64_t>(pipe_organ_buffer_capacity(sr));
}

float PipeOrganVoiceCore::render(float pitch_ratio) noexcept {
  if (slab_ == nullptr) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Shared wind gate: ramp to 1 while blowing, to 0 once released.
  const float target = releasing_ ? 0.0f : 1.0f;
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  breath_level_ += coeff * (target - breath_level_);

  float mix = 0.0f;
  for (int r = 0; r < rank_count_; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    if (pipe.bore == nullptr || pipe.jet == nullptr || pipe.bore_size < 8) continue;

    const float breath = pipe.breath * breath_level_;

    // Open/stopped-end reflection from the previous bore output: one-pole loss
    // lowpass, sign-selected feedback (+ open / - stopped).
    pipe.lp_state += pipe.lp_alpha * (pipe.bore_out - pipe.lp_state);
    const float temp = pipe.sign * pipe.loss_gain * pipe.lp_state;

    // Jet drive: the pressure difference across the flue convects (jet delay)
    // and deflects across the labium (the cubic jet table).
    const float pd = breath - pipe.jet_reflection * temp;
    const float bore_delay = std::clamp(pipe.bore_period / ratio - pipe.comp, 1.0f,
                                        static_cast<float>(pipe.bore_size - 4));
    const float jet_delay =
        std::clamp(pipe.jet_ratio * bore_delay, 1.0f, static_cast<float>(pipe.jet_size - 4));
    const float pd_j =
        rt::lagrange3_fractional_delay(pipe.jet, static_cast<size_t>(pipe.jet_size), pipe.jet_write,
                                       static_cast<int>(jet_delay * 256.0f), pd);
    const float jet_out = jet_table(pipe.jet_drive * pd_j, pipe.jet_asym);

    // DC-block the jet output, then drive the bore: jet flow plus the bore end
    // reflection.
    const float jet_dc = jet_out - pipe.dc_x1 + pipe.dc_r * pipe.dc_y1;
    pipe.dc_x1 = jet_out;
    pipe.dc_y1 = jet_dc;
    float into = jet_dc + pipe.end_reflection * temp;

    // Even-harmonic pump (open pipe): a half-wave rectified bore feedback carries
    // a 2f0 component; strip its DC and inject the octave the open flue voices.
    if (pipe.even_gain > 0.0f) {
      const float rect = temp > 0.0f ? temp : 0.0f;
      pipe.even_state += pipe.even_hp_alpha * (rect - pipe.even_state);
      float pump = pipe.even_gain * (rect - pipe.even_state);
      pump = pump < -1.5f ? -1.5f : (pump > 1.5f ? 1.5f : pump);
      into += pump;
    }

    pipe.bore_out = rt::lagrange3_fractional_delay(pipe.bore, static_cast<size_t>(pipe.bore_size),
                                                   pipe.bore_write,
                                                   static_cast<int>(bore_delay * 256.0f), into);

    // Mouth/radiation high-shelf (post-loop): lift the partials the pipe
    // radiates more efficiently into the room. rad_gain == 0 is a true bypass.
    float radiated = pipe.bore_out;
    if (pipe.rad_gain > 0.0f) {
      pipe.rad_state += pipe.rad_alpha * (pipe.bore_out - pipe.rad_state);
      radiated = pipe.bore_out + pipe.rad_gain * (pipe.bore_out - pipe.rad_state);
    }

    // Chiff: a decaying bright onset burst on top of the pitched tone.
    float chiff = 0.0f;
    if (pipe.chiff_level > 1.0e-5f) {
      chiff = pipe.chiff_level * breath_level_ *
              noise_.bipolar_at(kChiffIndexBase + pipe.noise_offset + drive_index_);
      pipe.chiff_level *= pipe.chiff_coeff;
    }

    mix += pipe.mix * (pipe.output_scale * radiated + chiff);
  }
  ++drive_index_;
  return mix;
}

void PipeOrganVoiceCore::release() noexcept {
  releasing_ = true;
  // Cut the wind and damp each bore so it stops speaking promptly.
  for (int r = 0; r < rank_count_; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    pipe.loss_gain = std::min(pipe.loss_gain, pipe.release_loss);
  }
}

void PipeOrganVoiceCore::kill() noexcept {
  releasing_ = true;
  breath_level_ = 0.0f;
  for (int r = 0; r < rank_count_; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    pipe.lp_state = 0.0f;
    pipe.bore_out = 0.0f;
    pipe.dc_x1 = 0.0f;
    pipe.dc_y1 = 0.0f;
    pipe.chiff_level = 0.0f;
  }
}

// ---------------------------------------------------------------------------
// OrganWindSupply
// ---------------------------------------------------------------------------

void OrganWindSupply::prepare(double sample_rate, float tremulant_rate_hz, float tremulant_depth,
                              float wind_sag) noexcept {
  sr_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float rate = tremulant_rate_hz > 0.0f ? tremulant_rate_hz : 0.0f;
  const float depth = std::clamp(tremulant_depth, 0.0f, 1.0f);
  sag_depth_ = std::clamp(wind_sag, 0.0f, 1.0f);

  trem_phase_ = 0.0;
  trem_inc_ = rate > 0.0f ? kTwoPi * static_cast<double>(rate) / sr_ : 0.0;
  // A full-depth tremulant undulates the pressure by ~3% -> ~25 cents of pitch
  // and ~2.5 dB of level, the gentle wobble of a real wind tremulant.
  trem_pitch_cents_ = (rate > 0.0f) ? 25.0f * depth : 0.0f;
  trem_amp_ = (rate > 0.0f) ? 0.28f * depth : 0.0f;

  // Wind sag follower: pressure relaxes toward its target over ~80 ms, so a
  // chord onset dips then the regulator catches up.
  pressure_ = 1.0f;
  follow_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.08 * sr_)));

  active_ = trem_inc_ > 0.0 || sag_depth_ > 0.0f;
}

void OrganWindSupply::reset() noexcept {
  trem_phase_ = 0.0;
  pressure_ = 1.0f;
}

OrganWindSupply::State OrganWindSupply::process(int demand) noexcept {
  State s;
  if (!active_) return s;

  // Wind sag: demand (sounding pipes) pulls the pressure down through a
  // saturating load, smoothed by the regulator follower.
  if (sag_depth_ > 0.0f) {
    const float load = static_cast<float>(std::max(0, demand));
    const float target = 1.0f - sag_depth_ * (load / (load + 6.0f));
    pressure_ += follow_coeff_ * (target - pressure_);
  }

  // Tremulant: a slow undulation of the pressure on top of the sag.
  float trem = 0.0f;
  if (trem_inc_ > 0.0) {
    trem = std::sin(static_cast<float>(trem_phase_));
    trem_phase_ += trem_inc_;
    if (trem_phase_ >= kTwoPi) trem_phase_ -= kTwoPi;
  }

  const float pitch_cents = trem_pitch_cents_ * trem;
  // Sag drops pitch a touch as pressure falls (a slack pipe speaks flat).
  const float sag_pitch_cents = (pressure_ - 1.0f) * 30.0f;
  s.pitch_ratio = std::exp2((pitch_cents + sag_pitch_cents) * (1.0f / 1200.0f));
  s.gain = pressure_ * (1.0f + trem_amp_ * trem);
  return s;
}

}  // namespace sonare::midi::synth
