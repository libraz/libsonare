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

/// The permanently undamped treble: coupling level, where the dampers stop,
/// and how long those strings ring. Level 0 removes the population entirely
/// and is the identity.
///
/// A grand's damper felts run out partway up the treble; from there to the top
/// every string is free whether or not a foot is on the pedal. So with the
/// pedal UP -- which is how nearly everything is played, and how the reference
/// was captured -- the only strings that can answer another note are those,
/// and the damped register the bank had been tuned to (E1..E6, every four
/// semitones) is exactly the part of the instrument that is held silent.
///
/// It is not a subtlety. Strike C8 on the reference and look below its
/// fundamental, where the string it belongs to cannot radiate: a second later
/// the 1.15-3.4 kHz band holds a row of discrete peaks on CONSECUTIVE
/// semitones -- notes 88, 90, 91, 92, 93, 94, 95, 96, 97 -- each a few cents
/// sharp of equal temperament, which is a tuner's stretch and not a
/// coincidence. A soundboard answers with a wash; that is a set of strings.
/// The same row appears under C7 from note 83 up. The model radiated nothing
/// there, and no amount of fitting could produce it, because a resonator that
/// is not in the bank has no knob.
///
/// The decay is measured off the same band: it falls 22.7 dB between the
/// 0.5-1.5 s and 2.5-3.5 s windows on C8 and 23.1 dB on C7, which is a t60 of
/// about five seconds. Longer than a struck treble note by a wide margin, and
/// it should be -- these strings are never hit, they are only leaned on, and
/// nothing is damping them.
///
/// The level is nonetheless zero, and the reason is worth recording rather
/// than quietly leaving a dead knob. Swept against the reference at a damper
/// line the instrument actually has -- note 88 or above, which is where its own
/// release measurement puts it, since a released C6 keeps 0.9 percent of its
/// level and a string in this population would keep far more -- the bank moves
/// the keyboard error by nothing at all, at any coupling from 2 to 100. Drop
/// the line to 84 and it appears to help, but every decibel of that comes from
/// the played note finding ITSELF in the bank and ringing on undamped, which
/// then measures as a released C6 holding 7 percent against the reference's
/// 0.9. That is an artefact with the sign of a result.
///
/// The reason it cannot be exercised is upstream. These resonators are narrow,
/// so they take almost nothing from a transient and live instead on the played
/// note's upper partials landing on them — and those partials are the model's
/// known deficit: measured at 0.2 s a C6 arrives with h2 twenty-five decibels
/// under the reference. There is nothing for the coupling to work with. The
/// population is real and the bank is now tuned to it; turning it up is work
/// that follows fixing the excitation, not work that substitutes for it.
SONARE_TUNABLE(kSympTopLevel, 0.0f);
SONARE_TUNABLE(kSympTopNoteLo, 88.0f);
SONARE_TUNABLE(kSympTopT60S, 5.0f);
/// How the ring shortens with pitch, in halvings of t60 per octave above the
/// lowest undamped string.
SONARE_TUNABLE(kSympTopT60Oct, 0.5f);

/// Soundboard radiating band: the modes are log-spread between these corners.
SONARE_TUNABLE(kFLow, 92.0f);
SONARE_TUNABLE(kFHigh, 5400.0f);

/// Soundboard decay: the lowest mode's t60 in seconds, how fast that falls
/// with frequency, and the ceiling the whole law is clamped to.
///
/// These had been written inline at six-tenths of a second, which both hid what
/// they claim and was too short: strike C8, whose string is gone within a
/// second, and the reference still carries 125-1000 Hz two and a half seconds
/// later where the model was 85 dB under.
///
/// A second is the number spruce gives. A resonator's decay follows its loss
/// factor as t60 ~ 2.2/(f*eta), and spruce's one-to-three percent puts a 100 Hz
/// board mode at about a second — so this is now quoted from the material
/// rather than fitted, and what the fit had wanted instead is worth recording.
/// Against the reference, and with no frame bank present, the board scored best
/// at 2.5 s: it recovered 38 dB of the C7 sustain and cost under 2 dB of
/// broadband level anywhere. That was the board standing in for a member the
/// instrument has and the model did not, and it is why the number came out two
/// and a half times what the wood can do. With the frame bank carrying the long
/// tail the board measures better back at its own value, so the pair moved
/// together.
///
/// The slope is the part of the law the fit is confident about, and it is
/// steep: a board mode at 1 kHz keeps a fortieth of the t60 of one at 92. That
/// is the wood doing what wood does, and it is also what leaves room for the
/// frame — with a gentle slope the board would still be ringing where only the
/// iron should be, and the two banks would be fitting against each other
/// instead of dividing the spectrum between them.
SONARE_TUNABLE(kBoardT60Base, 1.0f);
SONARE_TUNABLE(kBoardT60Slope, 2.0f);
SONARE_TUNABLE(kBoardT60Max, 1.0f);

/// Frame (plate and rim) bank: its radiating band, its decay, and its return
/// level. See the bank's own commentary in piano_voice.h for why an iron
/// member is a different resonator from a wooden one.
///
/// The decay is quoted at the bottom of the band and graded flat, not because
/// a material says so but because the fit will not accept a grade: what the
/// bank is standing in for turns out to be the whole long-decay structure and
/// not a plate whose loss factor can be looked up. It is worth being precise
/// about what is and is not derived here, because an earlier revision of this
/// comment claimed more than the measurement supports.
///
/// What IS derived is that the member exists. A grand carries 150 kg of cast
/// iron and a laminated rim; iron's loss factor is an order of magnitude below
/// wood's, so it is the only part of the instrument that can hold a low
/// frequency for seconds, and the reference plainly does hold one. Adding the
/// bank at all takes the keyboard's low-band error from 15.8 dB to about 7.
///
/// What is NOT derived is nine seconds. Quote it against 62 Hz and it implies
/// 0.39 percent, above the 0.1-0.3 that iron gives; graded flat, the implied
/// loss factor varies right across the band, which no single material does.
/// The value it replaces was eighteen, which does land on iron's own 0.20
/// percent at 62 Hz — and it was wrong: scored only on the note while the key
/// is DOWN it looked best, and it left a released treble note ringing forty
/// times louder than the instrument does. Nine is what survives once the
/// release is in the metric. A number that lands on a textbook constant is
/// evidence for nothing if the measurement it came from could not see half the
/// note.
///
/// The band runs to 1800 Hz rather than stopping where the plate stops
/// behaving as a plate. Above roughly 600 Hz its modal density does turn its
/// response into a broadband floor rather than a set of rings, so the upper
/// part of this band is not literally plate modes; it is the rim, the lid and
/// the case answering, which have the same property of ringing long and the
/// same indifference to which note was struck.
SONARE_TUNABLE(kFrameFLow, 62.0f);
SONARE_TUNABLE(kFrameFHigh, 1800.0f);
SONARE_TUNABLE(kFrameT60S, 9.0f);
SONARE_TUNABLE(kFrameT60Slope, 0.0f);
/// Return level. Zero renders exactly as a build without the bank.
///
/// Four is where the whole-keyboard fit settles with the frame's decay and
/// band free to disagree. Two had been chosen against a level comparison with
/// no gain alignment, which read the body as louder than it was and so wanted
/// less of it; corrected, the bank fills the tail and touches the attack and
/// the notes below the tenor break by well under a decibel, which is the
/// behaviour a member with this decay and this band should have.
SONARE_TUNABLE(kFrameLevel, 4.0f);

/// Soundboard phase diffusion coefficient (both allpass stages).
SONARE_TUNABLE(kDiffuserG, 0.55f);

/// Soundboard air/sizzle noise gain, envelope-followed off the radiated
/// signal. Reference renders measure 20-40 dB tone-to-noise; a clean partial
/// stack reads dry and synthetic.
SONARE_TUNABLE(kAirGain, 0.0f);

}  // namespace

void PianoResonanceBank::prepare(double sample_rate) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  for (Mode& m : modes_) m = Mode{};
  int n = 0;
  // The permanently undamped treble, one mode per semitone and ungated. The
  // pitches carry the same Railsback stretch the played strings do, because
  // they ARE played strings — the reference's rows sit a few cents sharp and
  // a bank on bare equal temperament would beat against every note that
  // drives it.
  const int top_lo = static_cast<int>(std::lround(std::clamp(kSympTopNoteLo, 21.0f, 120.0f)));
  if (kSympTopLevel != 0.0f) {
    for (int note = top_lo; note <= 108 && n < kResonanceModes; ++note) {
      const auto midi_note = static_cast<uint8_t>(note);
      const float f = note_to_hz(midi_note) * std::exp2(piano_stretch_cents(midi_note) / 1200.0f);
      if (f >= 0.45f * sr) break;
      const float oct = static_cast<float>(note - top_lo) / 12.0f;
      const float t60 = std::max(0.05f, kSympTopT60S * std::exp2(-kSympTopT60Oct * oct));
      const float w = kTwoPi * f / sr;
      const float r = std::exp(-6.907755279f / (sr * t60));
      Mode& m = modes_[static_cast<size_t>(n++)];
      m.a1 = 2.0f * r * std::cos(w);
      m.a2 = -r * r;
      m.gain = kSympTopLevel * (1.0f - r);
    }
  }
  ungated_count_ = n;
  // The damped register, which answers only while the pedal holds it off the
  // strings: a reduced set spread E1..E6, every 4 semitones.
  for (int i = 0; i < 16 && n < kResonanceModes; ++i) {
    const int note = 28 + 4 * i;
    const float f = 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f);
    if (f >= 0.45f * sr) continue;
    Mode& m = modes_[static_cast<size_t>(n++)];
    const float w = kTwoPi * f / sr;
    const float r = std::exp(-6.907755279f / (sr * 0.6f));  // ~0.6 s ring t60
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Normalize the resonator to ~unity peak gain (the (1-r) factor cancels
    // the high-Q resonant boost) so the bank is a weak coupling, not a
    // runaway bandpass on the played note.
    m.gain = 1.0f - r;
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
  // Plucked open strings have no dampers either, but this path is driven with
  // damper_open held true, so the gate is transparent and the split the piano
  // path needs would be a distinction without a difference here.
  ungated_count_ = 0;
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
  float sum = 0.0f;
  // The undamped treble takes the drive raw: no felt ever touches it, so
  // neither the gate nor the ring-out below has anything to say about it.
  for (int i = 0; i < ungated_count_; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * bridge_in;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  const float x = gate_ * bridge_in;
  for (int i = ungated_count_; i < kResonanceModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * x;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // As the dampers fall back the pedal-lifted strings stop ringing quickly
  // (down to the duplex floor, whose faint ring stays).
  if (!damper_open && gate_ < 0.5f && gate_ > 1.2f * kDuplexFloor) {
    for (int i = ungated_count_; i < kResonanceModes; ++i) {
      Mode& m = modes_[static_cast<size_t>(i)];
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
    const float t60 =
        std::clamp(kBoardT60Base * std::pow(kFLow / f, kBoardT60Slope), 0.04f, kBoardT60Max);
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
  // Frame modes, log-spread over the plate/rim band. Same bandpass-residue
  // normalization as the board's, so the two sum without one of them piling a
  // low-frequency skirt onto the other.
  const float frame_hi = std::max(kFrameFHigh, kFrameFLow * 1.5f);
  for (int i = 0; i < kFrameModes; ++i) {
    Mode& m = frame_[static_cast<size_t>(i)];
    m = Mode{};
    const float u = static_cast<float>(i) / static_cast<float>(kFrameModes - 1);
    const uint32_t h = (static_cast<uint32_t>(i) + 7u) * 2246822519u;
    const float jit = (static_cast<float>((h >> 9) & 0xFFFFu) / 65535.0f - 0.5f) * 0.10f;
    const float f = kFrameFLow * std::pow(frame_hi / kFrameFLow, u) * (1.0f + jit);
    if (kFrameLevel <= 0.0f || f >= 0.45f * sr) continue;
    const float w = kTwoPi * f / sr;
    const float t60 =
        std::max(0.05f, kFrameT60S * std::pow(kFrameFLow / f, std::max(0.0f, kFrameT60Slope)));
    const float r = std::exp(-6.907755279f / (sr * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    const float d_re = 1.0f - m.a1 * std::cos(w) - m.a2 * std::cos(2.0f * w);
    const float d_im = m.a1 * std::sin(w) + m.a2 * std::sin(2.0f * w);
    const float d_mag = std::sqrt(d_re * d_re + d_im * d_im);
    m.gain = kFrameLevel * d_mag / std::max(2.0f * std::sin(w), 1.0e-6f);
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
  for (Mode& m : frame_) {
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
  // Frame bank, off the same bandpass residue. The diffusers ahead of it are
  // allpass, so they cost the drive no magnitude and a resonator does not care
  // about the phase it is struck with; sharing the residue keeps the two banks
  // on one normalization instead of two that have to be kept in step. Tested
  // rather than multiplied out for the same reason the air layer below is: at
  // a zero return level the modes are cleared and the loop would spend eight
  // biquads producing a zero, and `0.0f * x` is not a constant the optimizer
  // may fold. In a shipped build kFrameLevel is a constexpr zero and the
  // block folds out entirely.
  if (kFrameLevel != 0.0f) {
    for (Mode& m : frame_) {
      const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * bp;
      m.y2 = m.y1;
      m.y1 = y;
      sum += y;
    }
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
