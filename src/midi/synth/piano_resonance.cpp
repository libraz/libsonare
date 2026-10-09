#include "midi/synth/piano_resonance.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/piano_voice.h"
#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kTwoPi;

/// Gate floor with the dampers down: duplex/aliquot segments and the undamped top octaves keep a
/// faint ring, so pedal-up sustain is not a bare harmonic stack.
SONARE_TUNABLE(kDuplexFloor, 0.3f);

/// Permanently undamped treble: coupling level, where the dampers stop, and ring t60. Level 0
/// removes the population (identity). Measured t60 is ~5 s (22.7 dB fall between the 0.5-1.5 s and
/// 2.5-3.5 s windows on C8, 23.1 dB on C7). Stays 0: enabled, notes 88+ ring as fixed-pitch chimes.
SONARE_TUNABLE(kSympTopLevel, 0.0f);
SONARE_TUNABLE(kSympTopNoteLo, 88.0f);
SONARE_TUNABLE(kSympTopT60S, 5.0f);
/// How the ring shortens with pitch, in halvings of t60 per octave above the
/// lowest undamped string.
SONARE_TUNABLE(kSympTopT60Oct, 0.5f);

/// Pedal-lifted register: ring t60, partial tilt, extra partial decay, and coupling into the
/// output. The pedal-up single-note corpus never excites this population, so no fit has seen it.
SONARE_TUNABLE(kSympRingT60S, 0.6f);
SONARE_TUNABLE(kSympPartialTilt, 0.7f);
SONARE_TUNABLE(kSympPartialDamp, 0.5f);
SONARE_TUNABLE(kSympCoupling, 0.06f);

/// Halvings of a mode's coupling per octave below the anchor: a heavy bass string answers the
/// bridge less. Set on the pedal take: the tail below 160 Hz sat 3.7 dB over the three grands'
/// span, and 1.0 lands within 0.1 dB of its midpoint.
SONARE_TUNABLE(kSympBassTaperOct, 1.0f);
SONARE_TUNABLE(kSympTaperAnchorHz, 261.6256f);

/// Soundboard radiating band: the modes are log-spread between these corners.
SONARE_TUNABLE(kFLow, 92.0f);
SONARE_TUNABLE(kFHigh, 5400.0f);

/// Soundboard decay: the lowest mode's t60 in seconds, its fall with frequency, and the ceiling.
/// Spruce's loss factor gives ~1 s at 100 Hz; 0.4 s is the whole-keyboard fit with the frame bank
/// carrying the long tail (2.5 s scored best without it). The steep slope leaves the tail to the
/// frame.
SONARE_TUNABLE(kBoardT60Base, 0.4f);
SONARE_TUNABLE(kBoardT60Slope, 2.0f);
SONARE_TUNABLE(kBoardT60Max, 1.0f);

/// Frame (plate and rim) bank: band, decay and return level (see piano_resonance.h). The bank takes
/// the low-band error from 15.8 dB to ~7. A 9 s decay is what survives once release is scored; 18 s
/// left a released treble note 40x louder than the instrument.
SONARE_TUNABLE(kFrameFLow, 39.68f);
SONARE_TUNABLE(kFrameFHigh, 4500.0f);
SONARE_TUNABLE(kFrameT60S, 9.0f);
SONARE_TUNABLE(kFrameT60Slope, 0.1f);
/// Return level. Zero renders as a build without the bank and ships so, since the late field
/// carries its job. At four the bank answered every note at 40, 195, 945 and 4547 Hz (a bell); the
/// continuum cut the 31 Hz-1 kHz profile error from 24.16 to 7.93 dB rms.
SONARE_TUNABLE(kFrameLevel, 0.0f);
/// Split between the two halves of each frame mode, in cents. Zero keeps eight separate modes;
/// above zero they are four near-degenerate pairs at no audio-thread cost. At 8 cents the whole-
/// keyboard error goes 10.39 -> 9.50, held-out 10.80 -> 9.97; 4 and 14 are within noise, 20 and 35
/// worse.
SONARE_TUNABLE(kFrameSplitCents, 8.0f);

/// Soundboard phase diffusion coefficient (both allpass stages).
SONARE_TUNABLE(kDiffuserG, 0.22f);

/// Air/sizzle noise gain, envelope-followed off the radiated signal; reference renders measure
/// 20-40 dB tone-to-noise.
SONARE_TUNABLE(kAirGain, 0.01f);

/// Air attack/release, in milliseconds: the release must outlast the string, since the grands'
/// 2-8 kHz tonality fills in ~20 dB between a held C4 and its tail (36.4..39.4 vs 11.9..19.6),
/// where this voice became 7.5 dB purer.
SONARE_TUNABLE(kAirAttackMs, 30.0f);
SONARE_TUNABLE(kAirReleaseMs, 200.0f);

/// Share of the air excitation taken from the air band of the drive; zero is broadband. Broadband,
/// a C8 drives the layer like a C4: a 1500 ms release slowed its decay 7.8 dB/s against the grands
/// (1000 ms: 3.7), whose C8 decay agrees within 0.57 dB/s.
SONARE_TUNABLE(kAirDriveBandMix, 0.0f);

/// Board width: how much of the board's return is radiated differently to the two legs, via
/// allpasses (same magnitude, different phase). Grands' channel correlation runs 0.09-0.92 and side
/// energy 0-12 dB under mid; 0.8 sits inside their 0.29-0.85 (C6) and -0.35-0.13 (C7) spread.
SONARE_TUNABLE(kBoardWidth, 0.8f);
SONARE_TUNABLE(kBoardWidthG, 0.62f);
/// Air band lowpass is two poles: one leaves the octave above 8 kHz 14-31 dB over the instrument,
/// three overshoots to 3-6 dB more tonal, two lands within 2.5 dB on every note (2-8 kHz tonality).
SONARE_TUNABLE(kAirHpHz, 500.0f);
SONARE_TUNABLE(kAirLpHz, 2800.0f);

/// Case and rim network: return level, decay, and where decay starts falling with frequency (see
/// piano_resonance.h). Zero renders as a build without it. Built to 0.28 modes/Hz and 30 dB
/// peak-to-floor (t60 = 2.2*0.28/0.15 s), it takes the 31 Hz-1 kHz profile error from 24.16
/// to 7.93 dB at level 1, which the corpus loss cannot resolve (5.86 Hz bins against 3.58 Hz mode
/// spacing). The level is quoted against the board return: 1.0 of the output over the 0.35
/// soundboard mix. The 17.5 dB shortfall at 31 Hz belongs to kRadiationHpHz, not to this level.
SONARE_TUNABLE(kCaseLevel, 2.857143f);
SONARE_TUNABLE(kCaseT60S, 4.2f);
/// Blow entry time into the board bank, in milliseconds; unity DC gain keeps the blow's area. In
/// one sample the blow is a click (C8's first 50 ms overshot by 11 dB while the second undershot by
/// 7.5); 10 ms reconciles them, 3 ms still moves the peak faster than the energy, 25 ms loses it.
SONARE_TUNABLE(kBoardStrikeSpreadMs, 10.0f);
/// Ratio between the slowest line's decay time and kCaseT60S, which the fastest keeps; above one
/// the lines spread log-uniformly. Identity and unproven, like kCaseDriveDirect: the fit walked it
/// to sixteen (a 67 s line) against a recorded floor that does not decay.
SONARE_TUNABLE(kCaseT60Spread, 1.0f);
/// One-pole corner on each line's feedback. A radiating case loses its high
/// frequencies first, and this is the only place that grading is stated: the
/// frame bank's slope had to be flat because its modes are too far apart to
/// grade anything between them.
SONARE_TUNABLE(kCaseDampHz, 2200.0f);
/// Corner of the two-pole filter on the network's drive, which sets what reaches the loop (see
/// header). 80 rather than 300 Hz: at 300, 250 and 500 Hz carry 7 and 4 dB too much; two poles at
/// 80 keep the in-band level and take 16 dB more out of 250 Hz.
SONARE_TUNABLE(kCaseInHz, 80.0f);
/// Share of the board's own diffused signal in the case drive, against the second-difference
/// residue (a differentiator, 42 dB down at 30 Hz, 36 dB at 60 Hz). Unity moves the C8 low-band
/// deficit 29-32 dB but moves the whole keyboard (C3 overshoots 34 dB, C8 stays 13 short), so it
/// stays identity: the missing register dependence needs a body excited by the blow. The corpus
/// carries ~-64 dBFS of recorded rumble below ~250 Hz and cannot fit this control.
SONARE_TUNABLE(kCaseDriveDirect, 0.0f);
/// Delay lengths in samples at the network's 6 kHz internal rate (a 48 kHz host decimated by
/// kCaseDecim), all prime. Their total, 1676 samples = 0.2793 s, sets the modal density: 0.28 modes
/// per hertz, as measured off three grands' low band.
constexpr uint32_t kCaseDelays6k[8] = {131, 149, 173, 197, 223, 241, 269, 293};
/// Injection signs, one per line: equal signs feed eight coherent copies, a comb audible as
/// flutter; a fixed pattern decorrelates them and keeps a bounce bit-stable.
constexpr float kCaseInSign[8] = {1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f};
/// Coefficient of the allpass diffuser inside each line, and its lengths. Zero skips the stage
/// outright, since an allpass at zero is a plain delay.
SONARE_TUNABLE(kCaseDiffuseG, 0.0f);
constexpr uint32_t kCaseApDelays6k[8] = {11, 13, 17, 19, 23, 29, 31, 37};

}  // namespace

void PianoResonanceBank::prepare(double sample_rate) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  for (Mode& m : modes_) m = Mode{};
  int n = 0;
  // Permanently undamped treble: one ungated mode per semitone, stretched like the played strings
  // so it does not beat against them.
  const int top_lo = static_cast<int>(std::lround(std::clamp(kSympTopNoteLo, 21.0f, 120.0f)));
  if (kSympTopLevel != 0.0f) {
    for (int note = top_lo; note <= 108 && n < kResonanceModes; ++note) {
      const auto midi_note = static_cast<uint8_t>(note);
      const float f = note_to_hz(midi_note) * cents_to_ratio(piano_stretch_cents(midi_note));
      if (f >= 0.45f * sr) break;
      const float oct = static_cast<float>(note - top_lo) / kSemitonesPerOctave;
      const float t60 = std::max(0.05f, kSympTopT60S * std::exp2(-kSympTopT60Oct * oct));
      const float w = kTwoPi * f / sr;
      const float r = std::exp(-6.907755279f / (sr * t60));
      Mode& m = modes_[static_cast<size_t>(n++)];
      m.a1 = 2.0f * r * std::cos(w);
      m.a2 = -r * r;
      m.gain = kSympTopLevel * resonator_gain_at_rate(r, w, sr);
    }
  }
  ungated_count_ = n;
  // Damped register, answering only while the pedal lifts the felts: E1..E6 every 4 semitones,
  // then partials 2 and 3. A pedalled bass note lights the treble's upper partials, so fundamentals
  // alone would be a hum. Stretched like the played strings.
  const float ring = std::max(0.05f, kSympRingT60S);
  const float tilt = std::max(0.0f, kSympPartialTilt);
  const float pdamp = std::max(0.0f, kSympPartialDamp);
  for (int k = 1; k <= 3 && n < kResonanceModes; ++k) {
    const float kf = static_cast<float>(k);
    for (int i = 0; i < 16 && n < kResonanceModes; ++i) {
      const auto note = static_cast<uint8_t>(28 + 4 * i);
      const float b = piano_inharmonicity_b(note);
      const float f0 = note_to_hz(note) * cents_to_ratio(piano_stretch_cents(note));
      // Stiff-string placement, as the played strings carry.
      const float f = f0 * kf * std::sqrt(1.0f + b * kf * kf);
      if (f >= 0.45f * sr) continue;
      // A string's upper partials shed energy faster than its fundamental, so
      // the higher the partial the shorter the ring.
      const float t60 = std::max(0.02f, ring * std::pow(kf, -pdamp));
      const float w = kTwoPi * f / sr;
      const float r = std::exp(-6.907755279f / (sr * t60));
      Mode& m = modes_[static_cast<size_t>(n++)];
      m.a1 = 2.0f * r * std::cos(w);
      m.a2 = -r * r;
      // Unity-peak normalization ((1-r) cancels the resonant boost), then tilt the series down.
      m.gain = resonator_gain_at_rate(r, w, sr) * std::pow(kf, -tilt);
      // Bridge admittance, per mode: heavier strings take up less of the bridge's motion.
      if (kSympBassTaperOct != 0.0f && f > 0.0f) {
        const float octaves_below =
            std::max(0.0f, std::log2(std::max(kSympTaperAnchorHz, 1.0f) / f));
        m.gain *= std::exp2(-kSympBassTaperOct * octaves_below);
      }
    }
  }
  gate_ = 0.0f;
  // Damper-open envelope: ~10 ms to lift, ~60 ms to fall.
  gate_open_coeff_ = 1.0f - std::exp(-1.0f / (0.010f * sr));
  gate_close_coeff_ = 1.0f - std::exp(-1.0f / (0.060f * sr));
  // Extra ring-out applied while the dampers are falling (~0.15 s t60).
  ringout_ = std::exp(-6.907755279f / (sr * 0.15f));
  // Weak sympathetic coupling (the played string still dominates).
  out_gain_ = std::max(0.0f, kSympCoupling);
}

void PianoResonanceBank::prepare_custom(double sample_rate, const float* freqs, int count,
                                        float ring_t60_s, float out_gain) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  const int n = std::min(count, kResonanceModes);
  // No "undamped treble" split for this path: every mode goes through gate_,
  // so a plucked-string caller's sustain-pedal state reaches every string in
  // the bank, not just the ones a piano would leave ungated.
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
    m.gain = resonator_gain_at_rate(r, w, sr);
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
  // Same lift/fall smoothing as the piano board (~10 ms open, ~60 ms close).
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
  // Two Schroeder allpasses stand in for the board's dense mode lattice; sized in ms, with
  // incommensurate lengths to avoid a combined echo.
  constexpr float kDiffuserMs[2] = {4.1f, 9.7f};
  for (int d = 0; d < 2; ++d) {
    diff_len_[d] =
        std::clamp<size_t>(static_cast<size_t>(kDiffuserMs[d] * 0.001f * sr), 4, kDiffuserCapacity);
    diff_buf_[d].fill(0.0f);
    diff_idx_[d] = 0;
  }
  // One decorrelator per leg. Longer than the diffusers and incommensurate
  // with them, so the two legs' phase fields separate at low frequencies too
  // rather than only where the diffusers act.
  constexpr float kSideMs[2] = {13.3f, 21.1f};
  for (int d = 0; d < 2; ++d) {
    side_len_[d] =
        std::clamp<size_t>(static_cast<size_t>(kSideMs[d] * 0.001f * sr), 4, kDiffuserCapacity);
    side_buf_[d].fill(0.0f);
    side_idx_[d] = 0;
  }
  side_ = 0.0f;
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
    // Damping rises with frequency: low body modes ring ~0.45 s, high modes are brief.
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
    // Bandpass residue, exactly peak-normalized: gain = envelope * |D| / (2 sin w) puts every
    // mode's peak at the envelope level; modes without the DC zero pile up a >10 dB bass shelf.
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
    // Paired or separate; jitter is drawn per pair so the halves stay a pair.
    const bool paired = kFrameSplitCents > 0.0f;
    const int slot = paired ? i / 2 : i;
    const int slots = paired ? kFrameModes / 2 : kFrameModes;
    const float u = static_cast<float>(slot) / static_cast<float>(slots - 1);
    const uint32_t h = (static_cast<uint32_t>(slot) + 7u) * 2246822519u;
    const float jit = (static_cast<float>((h >> 9) & 0xFFFFu) / 65535.0f - 0.5f) * 0.10f;
    const float split =
        paired ? cents_to_ratio(((i % 2 == 0) ? -0.5f : 0.5f) * kFrameSplitCents) : 1.0f;
    const float f = kFrameFLow * std::pow(frame_hi / kFrameFLow, u) * (1.0f + jit) * split;
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
  // Case network at its decimated rate: lengths scale with it, and the set shrinks together if it
  // will not fit the pool (costing density at very high rates, not truncating one line).
  {
    const float sr_case = sr / static_cast<float>(kCaseDecim);
    const double rate = static_cast<double>(sr_case) / 6000.0;
    uint32_t total = 0;
    for (const uint32_t d6 : kCaseDelays6k) {
      total += std::max(2u, static_cast<uint32_t>(std::lround(static_cast<double>(d6) * rate)));
    }
    const double fit = total > kCaseCapacity
                           ? static_cast<double>(kCaseCapacity) / static_cast<double>(total)
                           : 1.0;
    uint32_t ap_total = 0;
    for (const uint32_t d6 : kCaseApDelays6k) {
      ap_total += std::max(2u, static_cast<uint32_t>(std::lround(static_cast<double>(d6) * rate)));
    }
    const double ap_fit = ap_total > kCaseApCapacity
                              ? static_cast<double>(kCaseApCapacity) / static_cast<double>(ap_total)
                              : 1.0;
    uint32_t off = 0;
    uint32_t ap_off = 0;
    for (size_t i = 0; i < kCaseLines; ++i) {
      const auto len = std::max(
          2u,
          static_cast<uint32_t>(std::lround(static_cast<double>(kCaseDelays6k[i]) * fit * rate)));
      case_off_[i] = off;
      case_len_[i] = len;
      case_idx_[i] = 0;
      off += len;
      const auto ap_len =
          kCaseDiffuseG == 0.0f
              ? 0u
              : std::max(2u, static_cast<uint32_t>(std::lround(
                                 static_cast<double>(kCaseApDelays6k[i]) * ap_fit * rate)));
      case_ap_off_[i] = ap_off;
      case_ap_len_[i] = ap_len;
      case_ap_idx_[i] = 0;
      ap_off += ap_len;
      // Per-line feedback for a common t60, charging the loss against delay plus diffuser length;
      // the spread is log-uniform and kCaseT60S stays the fastest.
      float t60 = std::max(0.05f, kCaseT60S);
      if (kCaseT60Spread != 1.0f && kCaseLines > 1) {
        const float u = static_cast<float>(i) / static_cast<float>(kCaseLines - 1);
        t60 *= std::pow(std::max(0.01f, kCaseT60Spread), u);
      }
      case_g_[i] = std::min(
          0.9999f, std::exp(-6.907755279f * static_cast<float>(len + ap_len) / (sr_case * t60)));
      case_lp_[i] = 0.0f;
    }
    case_buf_.fill(0.0f);
    case_ap_buf_.fill(0.0f);
    // In-loop damping is quoted against the network's own rate, since that is
    // what its one-poles run at.
    case_lp_a_ =
        1.0f - std::exp(-kTwoPi * std::clamp(kCaseDampHz, 100.0f, 0.45f * sr_case) / sr_case);
    // Host-rate anti-alias filter, acting before decimation; the ceiling keeps fold-back two
    // octaves down its skirt.
    case_in_a_ = 1.0f - std::exp(-kTwoPi * std::clamp(kCaseInHz, 20.0f, 0.1f * sr_case) / sr);
    case_in_1_ = 0.0f;
    case_in_2_ = 0.0f;
    case_phase_ = 0;
    case_hold_ = 0.0f;
    case_out_lp_ = 0.0f;
    // Smooths the held sample. Well above the band the drive filter passes, so
    // it costs the member nothing and only removes the hold's own steps.
    case_out_a_ = 1.0f - std::exp(-kTwoPi * std::min(800.0f, 0.45f * sr) / sr);
    // 0.4 of the internal rate: flat through the member's band, 30 dB down at its first image.
    const float image_w0 = rt::frequency_to_w0(0.4f * sr_case, static_cast<double>(sr));
    for (int i = 0; i < kCaseImageStages; ++i) {
      case_image_lp_[i].set(
          rt::rbj_lowpass(image_w0, rt::butterworth_stage_q(2 * kCaseImageStages, i)));
      case_image_lp_[i].reset();
    }
    board_strike_a_ = std::clamp(
        1.0f - std::exp(-1000.0f / (std::max(0.01f, kBoardStrikeSpreadMs) * sr)), 0.0f, 1.0f);
  }
  in1_ = 0.0f;
  in2_ = 0.0f;
  air_env_ = 0.0f;
  air_lp_ = 0.0f;
  air_lp2_ = 0.0f;
  air_hp_ = 0.0f;
  air_rng_ = 0x9E3779B9u;
  air_attack_ =
      1.0f - std::exp(-1.0f / (std::max(kAirAttackMs, 0.1f) * 0.001f * static_cast<float>(sr)));
  air_release_ =
      1.0f - std::exp(-1.0f / (std::max(kAirReleaseMs, 0.1f) * 0.001f * static_cast<float>(sr)));
  air_lp_a_ = 1.0f - std::exp(-kTwoPi * std::min(kAirLpHz, 0.45f * sr) / sr);
  air_hp_a_ = 1.0f - std::exp(-kTwoPi * kAirHpHz / sr);
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
  for (int d = 0; d < 2; ++d) {
    side_buf_[d].fill(0.0f);
    side_idx_[d] = 0;
  }
  side_ = 0.0f;
  board_strike_ = 0.0f;
  board_strike_lp_ = 0.0f;
  case_strike_ = 0.0f;
  case_buf_.fill(0.0f);
  case_idx_.fill(0u);
  case_lp_.fill(0.0f);
  case_ap_buf_.fill(0.0f);
  case_ap_idx_.fill(0u);
  // The damping poles hold energy the buffers do not; left set, an All Sound Off keeps radiating.
  case_lp_.fill(0.0f);
  case_in_1_ = 0.0f;
  case_in_2_ = 0.0f;
  case_phase_ = 0;
  case_hold_ = 0.0f;
  case_out_lp_ = 0.0f;
  for (rt::BiquadState& st : case_image_lp_) st.reset();
  in1_ = 0.0f;
  in2_ = 0.0f;
  air_env_ = 0.0f;
  air_lp_ = 0.0f;
  air_lp2_ = 0.0f;
  air_hp_ = 0.0f;
  air_rng_ = 0x9E3779B9u;
}

float PianoSoundboard::process(float in) noexcept {
  // Radiate: diffuse the phases, then colour with the mode bank, preserving the overall level.
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
  // Pending blow: tested rather than added, so with none the drive is `bp` exactly.
  float board_in = bp;
  if (board_strike_ != 0.0f || board_strike_lp_ != 0.0f) {
    board_strike_lp_ += board_strike_a_ * (board_strike_ - board_strike_lp_);
    board_strike_ = 0.0f;
    board_in += board_strike_lp_;
  }
  for (Mode& m : modes_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * board_in;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // Frame bank, off the same bandpass residue; tested rather than multiplied out, since
  // `0.0f * x` is not foldable and a zero level would still run the biquads.
  if (kFrameLevel != 0.0f) {
    for (Mode& m : frame_) {
      const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * bp;
      m.y2 = m.y1;
      m.y1 = y;
      sum += y;
    }
  }
  // Sustain air: level-tracked bandpassed noise with a fixed seed so bounces stay bit-stable;
  // skipped at zero gain because `0.0f * x` is not foldable and its state feeds only `air`.
  // Case network, off the same bandpass residue and band-limited to its radiating range (see
  // kCaseInHz); tested rather than multiplied out so a zero level costs nothing.
  float late = 0.0f;
  if (kCaseLevel != 0.0f) {
    constexpr size_t kLines = static_cast<size_t>(kCaseLines);
    // Band-limit the drive at the host rate (also the anti-alias filter); unity DC gain keeps the
    // in-band level. Tested rather than interpolated, so the identity share is bp exactly.
    const float case_in = kCaseDriveDirect != 0.0f ? bp + kCaseDriveDirect * (d - bp) : bp;
    case_in_1_ += case_in_a_ * (case_in - case_in_1_);
    case_in_2_ += case_in_a_ * (case_in_1_ - case_in_2_);
    if (case_phase_ == 0u) {
      // A blow bypasses the drive poles, which model sustained bridge force (see kCaseInHz); it is
      // an impulse into the plate. Tested so with none pending the drive is case_in_2_ exactly.
      float drive = case_in_2_;
      if (case_strike_ != 0.0f) {
        drive += case_strike_;
        case_strike_ = 0.0f;
      }
      float scaled[kLines];
      float out_sum = 0.0f;
      float mix_sum = 0.0f;
      for (size_t i = 0; i < kLines; ++i) {
        float tap = case_buf_[case_off_[i] + case_idx_[i]];
        // Diffuser ahead of the output tap: a unity-magnitude lattice allpass.
        if (case_ap_len_[i] != 0u) {
          const size_t p = case_ap_off_[i] + case_ap_idx_[i];
          const float stored = case_ap_buf_[p];
          const float v = tap + kCaseDiffuseG * stored;
          case_ap_buf_[p] = v;
          case_ap_idx_[i] = case_ap_idx_[i] + 1 < case_ap_len_[i] ? case_ap_idx_[i] + 1 : 0u;
          tap = stored - kCaseDiffuseG * v;
        }
        out_sum += kCaseInSign[i] * tap;
        // Damp, attenuate, then mix: the matrix must act on the vector actually fed back.
        case_lp_[i] += case_lp_a_ * (tap - case_lp_[i]);
        scaled[i] = case_g_[i] * case_lp_[i];
        mix_sum += scaled[i];
      }
      // Householder y = x - (2/N) * sum(x): orthogonal, so decay is entirely per-line gain and
      // damping.
      const float mix = (2.0f / static_cast<float>(kCaseLines)) * mix_sum;
      for (size_t i = 0; i < kLines; ++i) {
        case_buf_[case_off_[i] + case_idx_[i]] = scaled[i] - mix + kCaseInSign[i] * drive;
        case_idx_[i] = case_idx_[i] + 1 < case_len_[i] ? case_idx_[i] + 1 : 0u;
      }
      // Sum with the injection signs so the decorrelated copies recombine.
      case_hold_ = out_sum;
    }
    case_phase_ = case_phase_ + 1u < kCaseDecim ? case_phase_ + 1u : 0u;
    // Smooth the held sample rather than radiating its steps.
    case_out_lp_ += case_out_a_ * (case_hold_ - case_out_lp_);
    float held = case_out_lp_;
    for (rt::BiquadState& st : case_image_lp_) held = st.process(held);
    late = held * kCaseLevel;
  } else {
    // Nothing consumes the accumulator when the network is off, so it is spent here.
    case_strike_ = 0.0f;
  }
  float air = 0.0f;
  if (kAirGain != 0.0f) {
    float drive = d;
    if (kAirDriveBandMix != 0.0f) {
      // Same poles and corner as the radiated noise, so excitation and result are shaped alike.
      air_drv_lp_ += air_lp_a_ * (d - air_drv_lp_);
      air_drv_lp2_ += air_lp_a_ * (air_drv_lp_ - air_drv_lp2_);
      air_drv_hp_ += air_hp_a_ * (air_drv_lp2_ - air_drv_hp_);
      drive = d + kAirDriveBandMix * ((air_drv_lp2_ - air_drv_hp_) - d);
    }
    const float mag = drive >= 0.0f ? drive : -drive;
    air_env_ += (mag > air_env_ ? air_attack_ : air_release_) * (mag - air_env_);
    air_rng_ = air_rng_ * 1664525u + 1013904223u;
    const float white = static_cast<float>(air_rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
    air_lp_ += air_lp_a_ * (white - air_lp_);
    air_lp2_ += air_lp_a_ * (air_lp_ - air_lp2_);
    air_hp_ += air_hp_a_ * (air_lp2_ - air_hp_);
    air = kAirGain * air_env_ * (air_lp2_ - air_hp_);
  }
  // The late field goes through `out_gain_` with the banks; kCaseLevel is relative to the board's
  // return.
  const float out = (1.0f - kPianoDirectGain) * d + out_gain_ * (sum + late) + air;
  // Tested rather than multiplied out: at zero width the allpasses would circulate a zero, and
  // `0.0f * x` is not foldable.
  side_ = 0.0f;
  if (kBoardWidth != 0.0f) {
    float leg[2];
    for (int st = 0; st < 2; ++st) {
      float* buf = side_buf_[st].data();
      size_t& idx = side_idx_[st];
      const float v = out + kBoardWidthG * buf[idx];
      leg[st] = buf[idx] - kBoardWidthG * v;
      buf[idx] = v;
      idx = idx + 1 < side_len_[st] ? idx + 1 : 0;
    }
    side_ = kBoardWidth * 0.5f * (leg[0] - leg[1]);
  }
  return out;
}

}  // namespace sonare::midi::synth
