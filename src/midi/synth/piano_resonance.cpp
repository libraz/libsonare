#include <algorithm>
#include <cmath>

#include "midi/synth/piano_voice.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kTwoPi;

/// Sympathetic-resonance gate floor with the dampers down: the dampers only
/// rest on the speaking lengths, so the duplex/aliquot segments beyond the
/// bridge and the undamped top octaves keep a faint shimmer ringing even with
/// the pedal up. Without this floor the pedal-up sustain is a spectrally bare
/// harmonic stack — it reads as a plucked string, not a whole instrument.
SONARE_TUNABLE(kDuplexFloor, 0.3f);

/// Soundboard radiating band: the modes are log-spread between these corners.
SONARE_TUNABLE(kFLow, 92.0f);
SONARE_TUNABLE(kFHigh, 5400.0f);

/// Soundboard phase diffusion coefficient (both allpass stages).
SONARE_TUNABLE(kDiffuserG, 0.55f);

/// Soundboard air/sizzle noise gain, envelope-followed off the radiated
/// signal. Reference renders measure 20-40 dB tone-to-noise; a clean partial
/// stack reads dry and synthetic.
SONARE_TUNABLE(kAirGain, 0.0f);

}  // namespace

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

void PianoResonanceBank::prepare_custom(double sample_rate, const float* freqs, int count,
                                        float ring_t60_s, float out_gain) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  const int n = std::min(count, kResonanceModes);
  const float t60 = std::max(0.02f, ring_t60_s);
  const float r = std::exp(-6.907755279f / (sr * t60));
  for (int i = 0; i < kResonanceModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float f = (i < n && freqs != nullptr) ? freqs[i] : 0.0f;
    if (f <= 0.0f || f >= 0.45f * sr) {
      m = Mode{};
      continue;
    }
    const float w = kTwoPi * f / sr;
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Unity-peak normalization (the (1-r) factor cancels the high-Q resonant
    // boost) so the bank is a weak coupling, not a runaway bandpass on the note.
    m.gain = 1.0f - r;
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
  // Held open by the caller (plucked strings have no dampers), so the fall-time
  // coefficient is only the ~10 ms lift; reuse the piano smoothing constants.
  gate_open_coeff_ = 1.0f - std::exp(-1.0f / (0.010f * sr));
  gate_close_coeff_ = 1.0f - std::exp(-1.0f / (0.060f * sr));
  ringout_ = std::exp(-6.907755279f / (sr * 0.15f));
  out_gain_ = std::max(0.0f, out_gain);
}

void PianoResonanceBank::prepare_guitar_sympathetic(double sample_rate) noexcept {
  constexpr uint8_t kOpenStrings[6] = {40, 45, 50, 55, 59, 64};  // E2 A2 D3 G3 B3 E4
  float freqs[kResonanceModes];
  int n = 0;
  for (uint8_t note : kOpenStrings) freqs[n++] = note_to_hz(note);
  for (uint8_t note : kOpenStrings) freqs[n++] = 2.0f * note_to_hz(note);
  for (int i = 0; i < 4; ++i) freqs[n++] = 3.0f * note_to_hz(kOpenStrings[i]);
  // Open guitar/harp strings ring for seconds; a ~1.5 s bank t60 keeps the
  // halo audible without a runaway tail, at a weak coupling level.
  prepare_custom(sample_rate, freqs, n, /*ring_t60_s=*/1.5f, /*out_gain=*/0.05f);
}

void PianoResonanceBank::reset() noexcept {
  for (Mode& m : modes_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
}

float PianoResonanceBank::process(float bridge_in, bool damper_open) noexcept {
  const float target = damper_open ? 1.0f : kDuplexFloor;
  gate_ += (damper_open ? gate_open_coeff_ : gate_close_coeff_) * (target - gate_);
  const float x = gate_ * bridge_in;
  float sum = 0.0f;
  for (Mode& m : modes_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * x;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // As the dampers fall back the pedal-lifted strings stop ringing quickly
  // (down to the duplex floor, whose faint ring stays).
  if (!damper_open && gate_ < 0.5f && gate_ > 1.2f * kDuplexFloor) {
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
  // Phase diffusers: two short Schroeder allpasses (flat magnitude) standing
  // in for the board's dense high-order mode lattice, which scatters every
  // partial's phase by a different amount. Sized in ms so the smear tracks
  // the sample rate; incommensurate lengths avoid a combined echo.
  constexpr float kDiffuserMs[2] = {4.1f, 9.7f};
  for (int d = 0; d < 2; ++d) {
    diff_len_[d] =
        std::clamp<size_t>(static_cast<size_t>(kDiffuserMs[d] * 0.001f * sr), 4, kDiffuserCapacity);
    diff_buf_[d].fill(0.0f);
    diff_idx_[d] = 0;
  }
  // Modes log-spread across the soundboard's radiating band. A perfectly
  // geometric spacing would comb; a deterministic per-mode nudge breaks the
  // periodicity (no RNG — derived from the index so bounces stay bit-stable).
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
    // Damping rises with frequency: the low body modes ring ~0.45 s, the
    // high modes are broad and brief. The ring matters as much as the colour
    // — the board smears the strike into a short diffuse bloom, which is a
    // large part of why a piano does not read as a naked plucked string.
    const float t60 = std::clamp(0.6f * std::pow(kFLow / f, 0.55f), 0.04f, 0.6f);
    const float r = std::exp(-6.907755279f / (sr * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Radiation envelope: a low-mid tilt plus a broad bridge formant near
    // ~320 Hz, where a grand soundboard radiates most efficiently.
    const float tilt = std::pow(320.0f / f, 0.35f);
    const float l = std::log(f / 320.0f);
    const float formant = 1.0f + 1.0f * std::exp(-l * l / 0.9f);
    // Bandpass residue (the process() zero at DC/Nyquist), exactly
    // peak-normalized: |H| at the resonance is |1 - e^{-2jw}| / |D(e^{jw})|,
    // so gain = envelope * |D| / (2 sin w) puts every mode's peak at the
    // envelope level. Two-pole modes without the DC zero pile their
    // low-frequency skirts up in phase (a >10 dB bass shelf over the dry
    // path), and their inter-mode phase flips carve deep notches into the
    // dry sum right where note harmonics can land; the bandpass residue is
    // in quadrature with the dry path off-resonance, so it can only add.
    const float d_re = 1.0f - m.a1 * std::cos(w) - m.a2 * std::cos(2.0f * w);
    const float d_im = m.a1 * std::sin(w) + m.a2 * std::sin(2.0f * w);
    const float d_mag = std::sqrt(d_re * d_re + d_im * d_im);
    m.gain = tilt * formant * d_mag / std::max(2.0f * std::sin(w), 1.0e-6f);
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  in1_ = 0.0f;
  in2_ = 0.0f;
  air_env_ = 0.0f;
  air_lp_ = 0.0f;
  air_hp_ = 0.0f;
  air_rng_ = 0x9E3779B9u;
  air_attack_ = 1.0f - std::exp(-1.0f / (0.03f * sr));
  air_release_ = 1.0f - std::exp(-1.0f / (0.2f * sr));
  air_lp_a_ = 1.0f - std::exp(-kTwoPi * 2800.0f / sr);
  air_hp_a_ = 1.0f - std::exp(-kTwoPi * 500.0f / sr);
}

void PianoSoundboard::reset() noexcept {
  for (Mode& m : modes_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  for (int d = 0; d < 2; ++d) {
    diff_buf_[d].fill(0.0f);
    diff_idx_[d] = 0;
  }
  in1_ = 0.0f;
  in2_ = 0.0f;
  air_env_ = 0.0f;
  air_lp_ = 0.0f;
  air_hp_ = 0.0f;
  air_rng_ = 0x9E3779B9u;
}

float PianoSoundboard::process(float in) noexcept {
  // Radiate: diffuse the phases, then colour with the mode bank. The return
  // is the (1 - kPianoDirectGain) complement of the host's direct share plus
  // the modal colour, so the overall level is preserved while most of the
  // note arrives phase-scattered.
  float d = in;
  for (int st = 0; st < 2; ++st) {
    if (diff_len_[st] == 0) break;
    float* buf = diff_buf_[st].data();
    size_t& idx = diff_idx_[st];
    const float v = d + kDiffuserG * buf[idx];
    const float y = buf[idx] - kDiffuserG * v;
    buf[idx] = v;
    idx = idx + 1 < diff_len_[st] ? idx + 1 : 0;
    d = y;
  }
  const float bp = d - in2_;
  in2_ = in1_;
  in1_ = d;
  float sum = 0.0f;
  for (Mode& m : modes_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * bp;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // Sustain air: level-tracked bandpassed noise. Real piano sustain is not a
  // bare line spectrum — string/board sizzle and the undamped-segment wash
  // fill the space between the partials (reference renders measure 20-40 dB
  // tone-to-noise; a clean stack reads dry and synthetic). Deterministic
  // seed, so bounces stay bit-stable.
  //
  // The gain has never been fitted away from zero, and `0.0f * x` is not a
  // constant the optimizer may fold (a NaN or an infinity in x would have to
  // survive), so without this test the follower, the generator and both
  // one-poles would run per sample to produce a zero. The layer's state feeds
  // nothing but `air`, so skipping it cannot reach any other output. In a
  // shipped build kAirGain is a constexpr zero and the whole block folds out;
  // in a tuning build the test is what lets a fit switch the layer back on
  // without a rebuild.
  float air = 0.0f;
  if (kAirGain != 0.0f) {
    const float mag = d >= 0.0f ? d : -d;
    air_env_ += (mag > air_env_ ? air_attack_ : air_release_) * (mag - air_env_);
    air_rng_ = air_rng_ * 1664525u + 1013904223u;
    const float white = static_cast<float>(air_rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
    air_lp_ += air_lp_a_ * (white - air_lp_);
    air_hp_ += air_hp_a_ * (air_lp_ - air_hp_);
    air = kAirGain * air_env_ * (air_lp_ - air_hp_);
  }
  return (1.0f - kPianoDirectGain) * d + out_gain_ * sum + air;
}

}  // namespace sonare::midi::synth
