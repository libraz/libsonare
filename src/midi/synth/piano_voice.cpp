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

/// Phase delay (samples) of the one-pole loop lowpass y = (1-a)x + a*y^-1 at
/// normalized frequency @p w.
float onepole_phase_delay(float a, float w) noexcept {
  return std::atan2(a * std::sin(w), 1.0f - a * std::cos(w)) / std::max(w, 1.0e-6f);
}

/// First-order allpass coefficient a (<= 0) for a cascade of @p stages that
/// disperses the waveguide loop into the stiff-string law f_n =
/// n*f0*sqrt(1 + B*n^2). The loop resonates where the total round-trip phase
/// delay equals an integer number of periods, and only the lowpass and the
/// allpass cascade vary the phase delay with frequency, so a is solved
/// (bisection) to supply the stiff-string phase-delay differential between
/// the fundamental and a high reference partial, then clamped so the
/// per-stage delay still fits the loop budget. Endpoint-matched after Rauhala
/// & Valimaki (2006); RT-safe (bounded, allocation-free, deterministic).
float dispersion_allpass_a(float b_coeff, float w0, float lp_a, int stages,
                           float phase_budget) noexcept {
  if (b_coeff <= 0.0f || stages <= 0) return 0.0f;
  // Reference partial: high enough for a measurable differential but shrunk
  // until its stiff-string frequency sits safely below Nyquist (so the
  // treble, where B is large, still gets dispersion instead of bailing out).
  const float n_max = 0.8f * kPi / std::max(w0, 1.0e-6f);
  int n_ref = std::clamp(static_cast<int>(n_max), 2, 12);
  while (n_ref > 2 && w0 * static_cast<float>(n_ref) *
                              std::sqrt(1.0f + b_coeff * static_cast<float>(n_ref) *
                                                   static_cast<float>(n_ref)) >=
                          0.9f * kPi)
    --n_ref;
  const float fr = static_cast<float>(n_ref);
  const float w1 = w0 * std::sqrt(1.0f + b_coeff);
  const float wr = w0 * fr * std::sqrt(1.0f + b_coeff * fr * fr);
  if (wr >= 0.97f * kPi) return 0.0f;
  const float period = kTwoPi / w0;
  // Total phase-delay differential the dispersion must realize between the
  // two partials, net of the (frequency-independent) delay line.
  const float total_diff =
      period * (1.0f / std::sqrt(1.0f + b_coeff) - 1.0f / std::sqrt(1.0f + b_coeff * fr * fr));
  const float lp_diff = onepole_phase_delay(lp_a, w1) - onepole_phase_delay(lp_a, wr);
  const float need = (total_diff - lp_diff) / static_cast<float>(stages);
  if (need <= 0.0f) return 0.0f;
  // p_ap(w1;a) - p_ap(wr;a) increases monotonically as a -> -1.
  float lo = -0.999f;
  float hi = 0.0f;
  for (int it = 0; it < 40; ++it) {
    const float a = 0.5f * (lo + hi);
    const float diff = allpass_phase_delay(a, w1) - allpass_phase_delay(a, wr);
    if (diff > need)
      lo = a;
    else
      hi = a;
  }
  float a = 0.5f * (lo + hi);
  // Clamp so the per-stage phase delay at the fundamental fits the loop
  // budget (the delay line must keep a few samples).
  const float max_pap = phase_budget / static_cast<float>(stages);
  if (max_pap > 1.0f && allpass_phase_delay(a, w1) > max_pap) {
    float blo = a;
    float bhi = 0.0f;
    for (int it = 0; it < 30; ++it) {
      const float c = 0.5f * (blo + bhi);
      if (allpass_phase_delay(c, w1) > max_pap)
        blo = c;
      else
        bhi = c;
    }
    a = bhi;
  }
  return a;
}

}  // namespace

float piano_inharmonicity_b(uint8_t note) noexcept {
  const float n = static_cast<float>(note & 0x7Fu);
  // ~threefold growth per octave anchored near A4 (note 69), with a deep-bass
  // floor so the lowest wrapped strings keep a touch of stiffness.
  constexpr float kBAtA4 = 7.0e-4f;
  constexpr float kBetaPerSemitone = 0.0915750f;  // ln(3) / 12
  const float b = kBAtA4 * std::exp(kBetaPerSemitone * (n - 69.0f));
  return std::max(b, 2.0e-5f);
}

int piano_unison_strings(uint8_t note) noexcept {
  const int n = static_cast<int>(note & 0x7Fu);
  if (n <= 29) return 1;  // A0..F1: single wound string.
  if (n <= 47) return 2;  // F#1..B2: wound bichords.
  return 3;               // tenor break up: plain trichords.
}

void PianoVoiceCore::start(const PianoPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed, bool una_corda) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float f0 = note_to_hz(note);
  const float period = static_cast<float>(sr) / f0;
  const float w0 = kTwoPi / period;
  VoiceRandomSequence jitter(seed);

  // Loop lowpass (frequency-dependent damping).
  const float lp_a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.6f;
  loop_alpha_ = 1.0f - lp_a;
  const float tau_lp = onepole_phase_delay(lp_a, w0);

  // Stiffness dispersion: the per-note inharmonicity coefficient B drives a
  // first-order allpass cascade that stretches the partials sharp to
  // f_n = n*f0*sqrt(1 + B*n^2). The patch dispersion knob scales B
  // (0 = harmonic string).
  const float dispersion = std::clamp(params.dispersion, 0.0f, 1.0f);
  const float b_coeff = piano_inharmonicity_b(note) * dispersion;
  const float phase_budget = period - 4.0f - tau_lp;
  const float ap_a = dispersion_allpass_a(b_coeff, w0, lp_a, kPianoDispersionStages, phase_budget);

  // Two-stage decay rates (stretched down the keyboard).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float scale = std::exp2(stretch * octaves_below_a4);
  const float t60_fast = std::max(0.05f, params.decay_fast_s) * scale;
  const float t60_slow = std::max(t60_fast, params.decay_slow_s * scale);

  // The patch string count is the treble voicing; the real grand strings the
  // bass with fewer (a single wound string has no unison aftersound).
  num_strings_ =
      std::clamp(std::min(params.strings, piano_unison_strings(note)), 1, kMaxPianoStrings);
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
  // Una corda shifts the action onto a softer, less-grooved felt patch: a
  // touch quieter with a darker attack (lower felt-stiffness cutoff).
  hammer_amp_ = 0.9f * std::pow(vel01, amp_exp) * (una_corda ? 0.8f : 1.0f);
  comb_delay_ = static_cast<int>(std::clamp(params.strike_position, 0.0f, 0.5f) * period + 0.5f);
  exc_pos_ = 0;
  // Felt stiffening: compressed felt (hard strike) passes far more of the
  // pulse's top end — a velocity-driven one-pole on the injected force.
  const float exc_cutoff = 800.0f * std::exp2(3.0f * vel01) * (una_corda ? 0.4f : 1.0f);
  exc_alpha_ = std::clamp(1.0f - std::exp(-6.28318530718f * exc_cutoff / static_cast<float>(sr)),
                          0.01f, 1.0f);
  exc_lp_ = 0.0f;
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
  num_strings_ = 0;
  hammer_amp_ = 0.0f;
  contact_samples_ = 0;
}

void PianoResonanceBank::prepare(double sample_rate) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  // A reduced set of string modes spread E1..E6 (every 4 semitones) — the
  // bass-to-mid register where undamped sympathetic resonance is strongest.
  for (int i = 0; i < kResonanceModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const int note = 28 + 4 * i;
    const float f = 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f);
    if (f >= 0.45f * sr) {
      m = Mode{};
      continue;
    }
    const float w = kTwoPi * f / sr;
    const float r = std::exp(-6.907755279f / (sr * 0.6f));  // ~0.6 s ring t60
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Normalize the resonator to ~unity peak gain (the (1-r) factor cancels
    // the high-Q resonant boost) so the bank is a weak coupling, not a
    // runaway bandpass on the played note.
    m.gain = 1.0f - r;
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
  // Damper-open envelope: ~10 ms to lift, ~60 ms to fall.
  gate_open_coeff_ = 1.0f - std::exp(-1.0f / (0.010f * sr));
  gate_close_coeff_ = 1.0f - std::exp(-1.0f / (0.060f * sr));
  // Extra ring-out applied while the dampers are falling (~0.15 s t60).
  ringout_ = std::exp(-6.907755279f / (sr * 0.15f));
  // Weak sympathetic coupling (the played string still dominates).
  out_gain_ = 0.06f;
}

void PianoResonanceBank::reset() noexcept {
  for (Mode& m : modes_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
}

float PianoResonanceBank::process(float bridge_in, bool damper_open) noexcept {
  const float target = damper_open ? 1.0f : 0.0f;
  gate_ += (damper_open ? gate_open_coeff_ : gate_close_coeff_) * (target - gate_);
  const float x = gate_ * bridge_in;
  float sum = 0.0f;
  for (Mode& m : modes_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * x;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // As the dampers fall back the undamped strings stop ringing quickly.
  if (!damper_open && gate_ < 0.5f) {
    for (Mode& m : modes_) {
      m.y1 *= ringout_;
      m.y2 *= ringout_;
    }
  }
  return out_gain_ * sum;
}

void PianoSoundboard::prepare(double sample_rate, float mix) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  out_gain_ = std::clamp(mix, 0.0f, 1.0f);
  // Modes log-spread across the soundboard's radiating band. A perfectly
  // geometric spacing would comb; a deterministic per-mode nudge breaks the
  // periodicity (no RNG — derived from the index so bounces stay bit-stable).
  constexpr float kFLow = 92.0f;
  constexpr float kFHigh = 5400.0f;
  for (int i = 0; i < kSoundboardModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float u = static_cast<float>(i) / static_cast<float>(kSoundboardModes - 1);
    const uint32_t h = (static_cast<uint32_t>(i) + 1u) * 2654435761u;
    const float jit = (static_cast<float>((h >> 9) & 0xFFFFu) / 65535.0f - 0.5f) * 0.08f;
    const float f = kFLow * std::pow(kFHigh / kFLow, u) * (1.0f + jit);
    if (f >= 0.45f * sr) {
      m = Mode{};
      continue;
    }
    const float w = kTwoPi * f / sr;
    // Damping rises with frequency: the low body modes ring ~0.28 s, the high
    // modes are broad and brief (~0.03 s).
    const float t60 = std::clamp(0.28f * std::pow(kFLow / f, 0.55f), 0.03f, 0.30f);
    const float r = std::exp(-6.907755279f / (sr * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Radiation envelope: a low-mid tilt plus a broad bridge formant near
    // ~320 Hz, where a grand soundboard radiates most efficiently.
    const float tilt = std::pow(320.0f / f, 0.35f);
    const float l = std::log(f / 320.0f);
    const float formant = 1.0f + 0.6f * std::exp(-l * l / 0.9f);
    // Unity-peak normalization (the (1-r) factor cancels the resonant boost),
    // so the bank is a body colour, not a runaway bandpass on the input.
    m.gain = tilt * formant * (1.0f - r);
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
}

void PianoSoundboard::reset() noexcept {
  for (Mode& m : modes_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
}

float PianoSoundboard::process(float in) noexcept {
  float sum = 0.0f;
  for (Mode& m : modes_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * in;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  return out_gain_ * sum;
}

}  // namespace sonare::midi::synth
