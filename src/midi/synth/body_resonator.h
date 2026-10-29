#pragma once

/// @file body_resonator.h
/// @brief Fixed body/formant resonance bank for NativeSynth voices: a handful
///        of low-Q two-pole resonators approximating an instrument body's
///        dominant modes, run in parallel after the voice and mixed over it.
///
/// Voicings:
///   - kGuitar: dreadnought-ish air + plate modes (~100/200/400/550 Hz).
///   - kViolin: measured violin-corpus modes (A0, CBR, B1-/B1+, wood cluster,
///     2-3 kHz bridge hill); shared across the bowed family.
///   - kWoodTube: the tuned pipe under a mallet bar, note-tracked.
///   - kBrassBell: the bell's ~1.2 kHz radiation formant, note-independent;
///     trumpet family only.
///   - kVocal: open-vowel ("aah") F1..F4 of a mixed choir, note-independent.
///
/// The bowed family can instead run its member's bank in SERIES (start_series): the whole
/// signal passes a 4th-order roll-off at the air mode, where the plate and f-hole volume
/// velocities cancel, then the bank's broadband floor plus its modes — a bridge admittance
/// rather than a resonant halo over a flat dry path — levelled per note so a harmonic series
/// keeps its RMS through the stage.
///
/// RT contract: start()/process() are allocation-free; determinism: fixed
/// tables, no RNG.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// Body voicing selector (patch field; kNone = bypass).
enum class BodyType : int {
  kNone = 0,
  kGuitar = 1,
  kViolin = 2,
  kWoodTube = 3,
  kBrassBell = 4,
  kVocal = 5,
};

/// The violin family's corpora for the series body (bowed_string.corpus_series); kNone keeps the
/// wrapper's parallel bank.
enum class BowedCorpus : int {
  kNone = 0,
  kViolin = 1,
  kViola = 2,
  kCello = 3,
  kContrabass = 4,
};

class BodyResonator {
 public:
  // Modal banks (kViolin) span the corpus signature modes, a mid wood-mode
  // cluster, and the bridge hill; a physical instrument body has more than a
  // handful of dominant modes, so the shared cap has to accommodate them.
  static constexpr int kMaxModes = 16;

  /// One resonant body mode: a low-Q bandpass at @p freq_hz with reverb time
  /// @p t60_s, mixed in at @p weight.
  struct Spec {
    float freq_hz;
    float t60_s;
    float weight;
  };

  /// A body mode as reported in the instrument-acoustics literature: centre
  /// frequency, quality factor Q, and relative amplitude. Converted to a Spec
  /// (t60) at note-on so the fixed tables read like the source measurements.
  struct ModeQ {
    float freq_hz;
    float q;
    float weight;
  };

  /// Configures the bank from an explicit mode list (drum shells and other
  /// data-free voicings own their specs). @p mix in [0,1] blends the
  /// resonated path over the dry voice. Up to kMaxModes specs are used.
  /// @p corpus_tilt_hz is a one-pole cutoff on the dry floor; 0 (default)
  /// bypasses it so every existing caller stays bit-identical.
  void start_specs(const Spec* specs, int count, double sample_rate, float mix,
                   float corpus_tilt_hz = 0.0f) noexcept {
    const double sr = sample_rate > 0.0 ? sample_rate : ::sonare::constants::kDefaultDawSampleRate;
    series_ = false;
    mix_ = std::clamp(mix, 0.0f, 1.0f);
    num_modes_ = 0;
    strike_ = 0.0f;
    configure_tilt(sr, corpus_tilt_hz);
    if (mix_ <= 0.0f || count <= 0 || specs == nullptr) return;
    count = std::min(count, kMaxModes);
    for (int k = 0; k < count; ++k) {
      const Spec& spec = specs[static_cast<size_t>(k)];
      if (spec.freq_hz <= 0.0f || spec.freq_hz >= 0.45f * static_cast<float>(sr)) continue;
      Mode& mode = modes_[static_cast<size_t>(num_modes_)];
      configure_mode(mode, spec, static_cast<float>(sr));
      mode.y1 = 0.0f;
      mode.y2 = 0.0f;
      ++num_modes_;
    }
    x1_ = 0.0f;
    x2_ = 0.0f;
  }

  /// Configures the bank. @p note_hz tracks the voiced note (kWoodTube);
  /// @p mix in [0,1] blends the resonated path over the dry voice.
  /// @p corpus_scale uniformly scales the mode bank's centre frequencies
  /// (kViolin only; identity at 1.0, its default). @p corpus_tilt_hz is the
  /// dry-floor cutoff passed through to start_specs(). @p air_hz and
  /// @p air_t60_s replace kGuitar's air mode (mode 0); 0, their default, keeps the bank's.
  void start(BodyType type, double sample_rate, float note_hz, float mix, float corpus_scale = 1.0f,
             float corpus_tilt_hz = 0.0f, float air_hz = 0.0f, float air_t60_s = 0.0f) noexcept {
    if (type == BodyType::kNone) {
      series_ = false;
      mix_ = std::clamp(mix, 0.0f, 1.0f);
      num_modes_ = 0;
      configure_tilt(sample_rate > 0.0 ? sample_rate : ::sonare::constants::kDefaultDawSampleRate,
                     corpus_tilt_hz);
      return;
    }
    std::array<Spec, kMaxModes> specs{};
    int count = 0;
    switch (type) {
      case BodyType::kGuitar:
        specs = {{{air_hz > 0.0f ? air_hz : 100.0f, air_t60_s > 0.0f ? air_t60_s : 0.12f, 1.0f},
                  {200.0f, 0.08f, 0.7f},
                  {400.0f, 0.06f, 0.5f},
                  {550.0f, 0.05f, 0.35f}}};
        count = 4;
        break;
      case BodyType::kViolin:
        count = fill_from_modal(specs, kViolinBank, kViolinLevel, corpus_scale);
        break;
      case BodyType::kWoodTube: {
        const std::array<Spec, 2> wood = wood_tube_specs(std::max(20.0f, note_hz));
        specs = {{wood[0], wood[1], {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}};
        count = 2;
        break;
      }
      case BodyType::kBrassBell:
        // Broad radiation formants of the bell; short t60 = wide bandwidth so
        // the mix lifts a region, not a pitch. Note-independent (bell geometry).
        specs = {{{1200.0f, 0.014f, 1.0f},
                  {2400.0f, 0.010f, 0.6f},
                  {3400.0f, 0.008f, 0.4f},
                  {0.0f, 0.0f, 0.0f}}};
        count = 3;
        break;
      case BodyType::kVocal:
        // Open-vowel tract formants of a mixed choir "aah". F1/F2 carry the
        // vowel; F3/F4 the presence. Bandwidths widen up the series.
        specs = {{{700.0f, 0.030f, 1.0f},
                  {1080.0f, 0.024f, 0.7f},
                  {2650.0f, 0.014f, 0.45f},
                  {3500.0f, 0.010f, 0.3f}}};
        count = 4;
        break;
      case BodyType::kNone:
        break;
    }

    start_specs(specs.data(), count, sample_rate, mix, corpus_tilt_hz);
  }

  /// Configures @p corpus as a series bridge admittance for a note at @p note_hz: a 4th-order
  /// Butterworth highpass at the (scaled) air mode, then the bank's floor and modes, levelled so
  /// a 1/k harmonic series at @p note_hz keeps its RMS. @p mix blends the result over the dry
  /// voice; @p corpus_scale and @p corpus_tilt_hz as in start(), the tilt on the floor path.
  void start_series(BowedCorpus corpus, double sample_rate, float note_hz, float mix,
                    float corpus_scale, float corpus_tilt_hz) noexcept {
    const double sr = sample_rate > 0.0 ? sample_rate : ::sonare::constants::kDefaultDawSampleRate;
    const Corpus bank = corpus_of(corpus);
    std::array<Spec, kMaxModes> specs{};
    const int count = fill_from_modal(specs, bank.modes, bank.count, 1.0f, corpus_scale);
    start_specs(specs.data(), count, sample_rate, mix, corpus_tilt_hz);
    if (num_modes_ <= 0) return;
    series_ = true;
    floor_gain_ = bank.floor;
    const float srf = static_cast<float>(sr);
    const float cancel_hz = std::min(bank.air_hz * corpus_scale, 0.45f * srf);
    for (size_t k = 0; k < hp_.size(); ++k) {
      hp_[k].reset();
      hp_[k].set(rt::rbj_highpass(sonare::constants::kTwoPi * cancel_hz / srf, kButterworthQ4[k]));
    }
    series_gain_ = series_level(std::max(note_hz, 1.0f), sr);
  }

  /// Retunes an active note-tracked bank without clearing its resonator state.
  /// This is used by legato voices, where restarting the bank would erase the
  /// body tail and make a carried note sound like a new strike.
  void retune(BodyType type, double sample_rate, float note_hz) noexcept {
    if (type != BodyType::kWoodTube || (num_modes_ <= 0 && mix_ <= 0.0f)) return;
    const float sr = static_cast<float>(
        sample_rate > 0.0 ? sample_rate : ::sonare::constants::kDefaultDawSampleRate);
    const float fundamental = std::max(20.0f, note_hz);
    const std::array<Spec, 2> specs = wood_tube_specs(fundamental);
    const int old_count = num_modes_;
    int new_count = 0;
    for (const Spec& spec : specs) {
      if (spec.freq_hz <= 0.0f || spec.freq_hz >= 0.45f * sr) continue;
      Mode& mode = modes_[static_cast<size_t>(new_count)];
      if (new_count >= old_count) {
        mode.y1 = 0.0f;
        mode.y2 = 0.0f;
      }
      configure_mode(mode, spec, sr);
      ++new_count;
    }
    num_modes_ = new_count;
  }

  bool active() const noexcept { return num_modes_ > 0; }

  /// Strikes the bank's first mode (kGuitar's air mode) so the mixed output gains a decaying
  /// sine of peak @p amplitude at the mode's frequency and t60, starting in sine phase on the
  /// next process() sample. A later set_mix() rescales what remains of it by new/old mix.
  void strike_air(float amplitude) noexcept {
    if (num_modes_ <= 0 || mix_ <= 0.0f || series_) return;
    // The impulse response of y = a1 y1 + a2 y2 + d is d r^n sin((n+1)w) / sin(w).
    strike_ += amplitude * modes_[0].sin_w / mix_;
  }

  /// Rescales the body path of a running bank; a bank that is not running
  /// stays off (start the bank first).
  void set_mix(float mix) noexcept { mix_ = std::clamp(mix, 0.0f, 1.0f); }

  /// Drops the bank and its state: the voice reads as a mix-0 note afterwards.
  void stop() noexcept {
    mix_ = 0.0f;
    reset();
  }

  /// One sample through the bank: (tilted) dry floor + mixed body response, or the series
  /// admittance blended over the dry voice.
  float process(float x) noexcept {
    if (series_) return process_series(x);
    // Real radiativity keeps a broadband floor between the modes, but that
    // floor falls above a few kHz where this dry path was flat. Branch past
    // the filter at the identity (tilt off) rather than run it at alpha = 1:
    // a one-pole update is not bit-identical to a plain pass-through.
    float floor_x = x;
    if (tilt_active_) {
      floor_y_ += tilt_alpha_ * (x - floor_y_);
      floor_x = floor_y_;
    }
    const float bp_in = x - x2_;
    x2_ = x1_;
    x1_ = x;
    float body = 0.0f;
    for (int k = 0; k < num_modes_; ++k) {
      Mode& mode = modes_[static_cast<size_t>(k)];
      const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * bp_in;
      mode.y2 = mode.y1;
      mode.y1 = y;
      body += y;
    }
    if (strike_ != 0.0f) {
      // A pending strike joins mode 0's recursion on this sample.
      modes_[0].y1 += strike_;
      body += strike_;
      strike_ = 0.0f;
    }
    return floor_x + mix_ * body;
  }

  void reset() noexcept {
    for (Mode& mode : modes_) {
      mode.y1 = 0.0f;
      mode.y2 = 0.0f;
    }
    x1_ = 0.0f;
    x2_ = 0.0f;
    floor_y_ = 0.0f;
    num_modes_ = 0;
    strike_ = 0.0f;
    for (rt::BiquadState& hp : hp_) hp.reset();
    series_ = false;
  }

 private:
  /// One sample of the series admittance: highpass, then floor + modes, blended by mix_.
  float process_series(float x) noexcept {
    float h = hp_[0].process(x);
    h = hp_[1].process(h);
    float floor_x = h;
    if (tilt_active_) {
      floor_y_ += tilt_alpha_ * (h - floor_y_);
      floor_x = floor_y_;
    }
    const float bp_in = h - x2_;
    x2_ = x1_;
    x1_ = h;
    float body = 0.0f;
    for (int k = 0; k < num_modes_; ++k) {
      Mode& mode = modes_[static_cast<size_t>(k)];
      const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * bp_in;
      mode.y2 = mode.y1;
      mode.y1 = y;
      body += y;
    }
    const float wet = series_gain_ * (floor_gain_ * floor_x + body);
    return x + mix_ * (wet - x);
  }

  // Section Qs of a 4th-order Butterworth highpass: 1 / (2 cos(pi/8)) and 1 / (2 cos(3 pi/8)).
  static constexpr std::array<float, 2> kButterworthQ4 = {sonare::constants::kButterworth4QLow,
                                                          sonare::constants::kButterworth4QHigh};

  /// |H| of the series stage at angular frequency @p w (radians per sample).
  double series_magnitude(double w) const noexcept {
    const std::complex<double> z = std::polar(1.0, -w);
    const std::complex<double> z2 = z * z;
    std::complex<double> h = 1.0;
    for (const rt::BiquadState& bq : hp_)
      h *= std::complex<double>(rt::biquad_response(bq.c, static_cast<float>(w)));
    std::complex<double> bank = 0.0;
    for (int k = 0; k < num_modes_; ++k) {
      const Mode& m = modes_[static_cast<size_t>(k)];
      bank += static_cast<double>(m.gain) * (1.0 - z2) /
              (1.0 - static_cast<double>(m.a1) * z - static_cast<double>(m.a2) * z2);
    }
    std::complex<double> floor = static_cast<double>(floor_gain_);
    if (tilt_active_) {
      const double alpha = tilt_alpha_;
      floor *= alpha / (1.0 - (1.0 - alpha) * z);
    }
    return std::abs(h * (floor + bank));
  }

  // Harmonics the level normalisation reads (a 1/k series) and the gain it may reach: a note
  // whose whole series sits under the air mode is lifted no more than this.
  static constexpr int kSeriesLevelHarmonics = 32;
  static constexpr float kSeriesGainMax = 10.0f;

  /// Gain holding the RMS of a 1/k harmonic series at @p f0_hz through the stage at unity.
  float series_level(float f0_hz, double sample_rate) const noexcept {
    double weight = 0.0;
    double power = 0.0;
    for (int k = 1; k <= kSeriesLevelHarmonics; ++k) {
      const double hz = static_cast<double>(k) * f0_hz;
      if (hz >= 0.45 * sample_rate) break;
      const double wk = 1.0 / (static_cast<double>(k) * k);
      const double mag = series_magnitude(sonare::constants::kTwoPiD * hz / sample_rate);
      weight += wk;
      power += wk * mag * mag;
    }
    if (!(power > 0.0) || !(weight > 0.0)) return 1.0f;
    return std::min(static_cast<float>(std::sqrt(weight / power)), kSeriesGainMax);
  }
  /// Sets up the dry-floor one-pole from a cutoff in Hz; 0 leaves it inactive
  /// so process() takes the branch that skips it entirely.
  void configure_tilt(double sample_rate, float corpus_tilt_hz) noexcept {
    tilt_active_ = corpus_tilt_hz > 0.0f;
    tilt_alpha_ = tilt_active_ ? 1.0f - std::exp(-sonare::constants::kTwoPi * corpus_tilt_hz /
                                                 static_cast<float>(sample_rate))
                               : 0.0f;
    floor_y_ = 0.0f;
  }

  // t60 (s) of a two-pole resonator of quality factor q at frequency f:
  // Q = f / BW and BW ≈ ln(1000) / (pi * t60), so t60 = (ln(1000)/pi) * q / f.
  static constexpr float kT60SecPerQHz = 2.19848f;  // ln(1000) / pi

  static float q_to_t60(float freq_hz, float q) noexcept { return kT60SecPerQHz * q / freq_hz; }

  /// Wood-tube modes: the fundamental and its 4x partial.
  static std::array<Spec, 2> wood_tube_specs(float fundamental) noexcept {
    return {{{fundamental, 0.08f, 1.2f}, {fundamental * 4.0f, 0.04f, 0.3f}}};
  }

  /// Expands a literature modal table (freq/Q/weight) into resonator Specs:
  /// weights scaled by @p level, centre frequencies by @p scale (a dimension
  /// ratio that can only place A0), Q held so t60 follows the scaled
  /// frequency. Returns the number of Specs written.
  static int fill_from_modal(std::array<Spec, kMaxModes>& out, const ModeQ* bank, int bank_count,
                             float level, float scale) noexcept {
    const int count = std::min(bank_count, kMaxModes);
    for (int k = 0; k < count; ++k) {
      const ModeQ& m = bank[static_cast<size_t>(k)];
      const float freq_hz = m.freq_hz * scale;
      out[static_cast<size_t>(k)] = {freq_hz, q_to_t60(freq_hz, m.q), m.weight * level};
    }
    return count;
  }
  template <size_t N>
  static int fill_from_modal(std::array<Spec, kMaxModes>& out, const std::array<ModeQ, N>& bank,
                             float level, float scale) noexcept {
    return fill_from_modal(out, bank.data(), static_cast<int>(N), level, scale);
  }

  /// One member's series corpus: the air mode (Hz), below which the plate and f-hole cancel,
  /// the modes that radiate, and the broadband floor between them relative to a unit-weight peak.
  struct Corpus {
    float air_hz;
    const ModeQ* modes;
    int count;
    float floor;
  };
  static Corpus corpus_of(BowedCorpus corpus) noexcept {
    switch (corpus) {
      case BowedCorpus::kViola:
        return {kViolaBank[0].freq_hz, kViolaBank.data(), static_cast<int>(kViolaBank.size()),
                kViolaFloor};
      case BowedCorpus::kCello:
        return {kCelloAirHz, kCelloBank.data(), static_cast<int>(kCelloBank.size()), kCelloFloor};
      case BowedCorpus::kContrabass:
        return {kContrabassBank[0].freq_hz, kContrabassBank.data(),
                static_cast<int>(kContrabassBank.size()), kContrabassFloor};
      case BowedCorpus::kViolin:
      case BowedCorpus::kNone:
        break;
    }
    return {kViolinBank[0].freq_hz, kViolinBank.data(), static_cast<int>(kViolinBank.size()),
            kViolinFloor};
  }

  // Violin corpus from measured mobility/radiativity surveys (Dünnwald,
  // Jansson, Bissinger, Gough). CBR is the centre-bout rhomboid; B1-/B1+ the
  // main corpus bending modes, B1+ the strongest radiator.
  static constexpr std::array<ModeQ, 14> kViolinBank = {{
      {275.0f, 24.0f, 0.90f},   // A0 (air / Helmholtz)
      {405.0f, 20.0f, 0.50f},   // CBR
      {460.0f, 22.0f, 0.85f},   // B1-
      {550.0f, 22.0f, 1.00f},   // B1+
      {700.0f, 15.0f, 0.55f},   // wood-mode cluster
      {870.0f, 14.0f, 0.45f},   //
      {1100.0f, 13.0f, 0.50f},  //
      {1350.0f, 12.0f, 0.40f},  //
      {1600.0f, 12.0f, 0.45f},  //
      {1950.0f, 11.0f, 0.40f},  //
      {2350.0f, 6.0f, 0.75f},   // bridge hill (broad)
      {2750.0f, 6.0f, 0.55f},   //
      {3400.0f, 8.0f, 0.28f},   // high rolloff
      {4200.0f, 8.0f, 0.16f},   //
  }};
  // Broadband normalization holding the richer bank within ~1 dB of the old
  // 4-mode voicing at the same body_mix, so calibrated presets need no change.
  static constexpr float kViolinLevel = 0.42f;
  // Series floors: the dense-mode region a bank does not resolve. The cello's is measured on
  // its reference between the hump and the modes; the others are estimates.
  static constexpr float kViolinFloor = 0.25f;
  static constexpr float kViolaFloor = 0.25f;
  static constexpr float kCelloFloor = 0.62f;
  static constexpr float kContrabassFloor = 0.25f;

  // Viola: the violin corpus at the measured A0 ratio (0.88, its body-length ratio), as an
  // estimate; no per-mode survey of a viola is used here.
  static constexpr std::array<ModeQ, 14> kViolaBank = {{
      {242.0f, 24.0f, 0.90f},   // A0
      {356.0f, 20.0f, 0.50f},   // CBR
      {405.0f, 22.0f, 0.85f},   // B1-
      {484.0f, 22.0f, 1.00f},   // B1+
      {616.0f, 15.0f, 0.55f},   // wood-mode cluster
      {766.0f, 14.0f, 0.45f},   //
      {968.0f, 13.0f, 0.50f},   //
      {1188.0f, 12.0f, 0.40f},  //
      {1408.0f, 12.0f, 0.45f},  //
      {1716.0f, 11.0f, 0.40f},  //
      {2068.0f, 6.0f, 0.75f},   // bridge hill
      {2420.0f, 6.0f, 0.55f},   //
      {2992.0f, 8.0f, 0.28f},   //
      {3696.0f, 8.0f, 0.16f},   //
  }};
  // Cello: air mode, B1-, A1 and B1+ from modal surveys (Bynum & Rossing 1997); CBR, the
  // cluster and the hill are estimates. The air mode only sets the corner; weights and Qs
  // are fitted to the reference's radiativity with string loss and vibrato matched first.
  static constexpr float kCelloAirHz = 100.0f;
  static constexpr std::array<ModeQ, 15> kCelloBank = {{
      {125.0f, 10.0f, 0.54f},  // CBR (estimate)
      {150.0f, 10.0f, 1.00f},  // B1-
      {180.0f, 10.0f, 0.85f},  // A1 (second air mode)
      {200.0f, 10.0f, 0.93f},  // B1+
      {255.0f, 8.0f, 0.94f},   // wood-mode cluster (estimate)
      {320.0f, 8.0f, 0.68f},   //
      {400.0f, 8.0f, 0.94f},   //
      {490.0f, 6.0f, 0.24f},   //
      {580.0f, 6.0f, 0.29f},   //
      {710.0f, 6.0f, 0.46f},   //
      {860.0f, 5.0f, 0.37f},   //
      {1000.0f, 5.0f, 0.15f},  //
      {1250.0f, 5.0f, 0.32f},  //
      {1550.0f, 5.0f, 0.79f},  //
      {2000.0f, 5.0f, 1.00f},  // bridge hill (estimate)
  }};
  // Contrabass: A0 near 60 Hz and the first corpus modes at 100-120 Hz, estimates from the
  // instrument's scale (no survey checked); above is the violin cluster scaled to that B1+.
  static constexpr std::array<ModeQ, 12> kContrabassBank = {{
      {60.0f, 10.0f, 0.90f},   // A0
      {80.0f, 10.0f, 0.50f},   // CBR (estimate)
      {100.0f, 10.0f, 0.85f},  // B1-
      {120.0f, 10.0f, 1.00f},  // B1+
      {150.0f, 8.0f, 0.55f},   // wood-mode cluster (estimate)
      {190.0f, 8.0f, 0.45f},   //
      {240.0f, 8.0f, 0.50f},   //
      {300.0f, 8.0f, 0.40f},   //
      {370.0f, 6.0f, 0.45f},   //
      {450.0f, 6.0f, 0.40f},   //
      {520.0f, 5.0f, 0.75f},   // bridge hill (estimate)
      {620.0f, 5.0f, 0.55f},   //
  }};

  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
    float sin_w = 0.0f;
  };
  static void configure_mode(Mode& mode, const Spec& spec, float sample_rate) noexcept {
    const float w = sonare::constants::kTwoPi * spec.freq_hz / sample_rate;
    const float r =
        std::exp(-static_cast<float>(::sonare::constants::kLn1000D) / (sample_rate * spec.t60_s));
    mode.a1 = 2.0f * r * std::cos(w);
    mode.a2 = -r * r;
    mode.sin_w = std::sin(w);
    // Bandpass form (zeros at z = +-1): phase-aligned with the dry path at
    // resonance, so the mix is a clean magnitude peak rather than a phasey
    // quadrature sum. Normalized to peak gain = weight.
    const float re = 1.0f - r * std::cos(2.0f * w);
    const float im = r * std::sin(2.0f * w);
    mode.gain = spec.weight * (1.0f - r) * std::sqrt(re * re + im * im) / (2.0f * std::sin(w));
  }
  std::array<Mode, kMaxModes> modes_{};
  // Shared bandpass input history (the zeros are common to every mode).
  float x1_ = 0.0f;
  float x2_ = 0.0f;
  int num_modes_ = 0;
  float mix_ = 0.0f;
  // strike_air() amount waiting for the next process() sample.
  float strike_ = 0.0f;
  // Dry-floor one-pole (corpus_tilt_hz): state and coefficient, inactive
  // unless configure_tilt() was given a positive cutoff.
  float floor_y_ = 0.0f;
  float tilt_alpha_ = 0.0f;
  bool tilt_active_ = false;
  // Series admittance (start_series): the air-mode highpass, the floor and the per-note level.
  std::array<rt::BiquadState, 2> hp_{};
  bool series_ = false;
  float floor_gain_ = 0.0f;
  float series_gain_ = 1.0f;
};

}  // namespace sonare::midi::synth
