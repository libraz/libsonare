#pragma once

/// @file body_resonator.h
/// @brief Fixed body/formant resonance bank for NativeSynth voices — the
///        cheap end of commuted synthesis (Smith & Van Duyne): a handful of
///        low-Q two-pole resonators approximating an instrument body's
///        dominant modes, mixed over the dry voice.
///
/// Data-free voicings:
///   - kGuitar: dreadnought-ish air + plate modes (~100/200/400/550 Hz).
///   - kViolin: a measured violin-corpus modal bank — the A0 Helmholtz air
///     mode, the CBR / B1-/B1+ signature corpus modes, a mid-range wood-mode
///     cluster, and the broad 2-3 kHz "bridge hill" (Dünnwald, Jansson,
///     Bissinger, Gough). Shared across the whole bowed family.
///   - kWoodTube: the tuned pipe under a marimba/xylophone bar — note-tracked
///     (the tube is cut for its bar), one strong fundamental resonance plus a
///     faint upper mode.
///   - kBrassBell: the flaring bell's radiation resonance — a broad "brass
///     formant" near 1.2 kHz with brilliance modes above it. Fixed by bell
///     geometry (note-independent), tuned for the trumpet family; larger-bore
///     brass keeps kNone.
///   - kVocal: an open-vowel vocal tract ("aah") — F1..F4 of a mixed choir
///     (~700/1080/2650/3500 Hz), note-independent like a real tract.
///
/// RT contract: start()/process() are allocation-free; determinism: fixed
/// tables, no RNG.

#include <algorithm>
#include <array>
#include <cmath>

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
  void start_specs(const Spec* specs, int count, double sample_rate, float mix) noexcept {
    const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
    mix_ = std::clamp(mix, 0.0f, 1.0f);
    num_modes_ = 0;
    if (mix_ <= 0.0f || count <= 0 || specs == nullptr) return;
    count = std::min(count, kMaxModes);
    for (int k = 0; k < count; ++k) {
      const Spec& spec = specs[static_cast<size_t>(k)];
      if (spec.freq_hz <= 0.0f || spec.freq_hz >= 0.45f * static_cast<float>(sr)) continue;
      Mode& mode = modes_[static_cast<size_t>(num_modes_)];
      const float w = sonare::constants::kTwoPi * spec.freq_hz / static_cast<float>(sr);
      const float r = std::exp(-6.907755279f / (static_cast<float>(sr) * spec.t60_s));
      mode.a1 = 2.0f * r * std::cos(w);
      mode.a2 = -r * r;
      // Bandpass form (zeros at z = +-1): phase-aligned with the dry path at
      // resonance, so the mix is a clean magnitude peak rather than a phasey
      // quadrature sum. Normalized to peak gain = weight.
      const float re = 1.0f - r * std::cos(2.0f * w);
      const float im = r * std::sin(2.0f * w);
      mode.gain = spec.weight * (1.0f - r) * std::sqrt(re * re + im * im) / (2.0f * std::sin(w));
      mode.y1 = 0.0f;
      mode.y2 = 0.0f;
      ++num_modes_;
    }
    x1_ = 0.0f;
    x2_ = 0.0f;
  }

  /// Configures the bank. @p note_hz tracks the played note (kWoodTube);
  /// @p mix in [0,1] blends the resonated path over the dry voice.
  void start(BodyType type, double sample_rate, float note_hz, float mix) noexcept {
    if (type == BodyType::kNone) {
      mix_ = std::clamp(mix, 0.0f, 1.0f);
      num_modes_ = 0;
      return;
    }
    std::array<Spec, kMaxModes> specs{};
    int count = 0;
    switch (type) {
      case BodyType::kGuitar:
        specs = {{{100.0f, 0.12f, 1.0f},
                  {200.0f, 0.08f, 0.7f},
                  {400.0f, 0.06f, 0.5f},
                  {550.0f, 0.05f, 0.35f}}};
        count = 4;
        break;
      case BodyType::kViolin:
        count = fill_from_modal(specs, kViolinBank, kViolinLevel);
        break;
      case BodyType::kWoodTube:
        specs = {{{std::max(20.0f, note_hz), 0.08f, 1.2f},
                  {std::max(20.0f, note_hz) * 4.0f, 0.04f, 0.3f},
                  {0.0f, 0.0f, 0.0f},
                  {0.0f, 0.0f, 0.0f}}};
        count = 2;
        break;
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

    start_specs(specs.data(), count, sample_rate, mix);
  }

  bool active() const noexcept { return num_modes_ > 0; }

  /// One sample through the bank: dry + mixed body response.
  float process(float x) noexcept {
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
    return x + mix_ * body;
  }

  void reset() noexcept {
    for (Mode& mode : modes_) {
      mode.y1 = 0.0f;
      mode.y2 = 0.0f;
    }
    x1_ = 0.0f;
    x2_ = 0.0f;
    num_modes_ = 0;
  }

 private:
  // t60 (s) of a two-pole resonator of quality factor q at frequency f:
  // Q = f / BW and BW ≈ ln(1000) / (pi * t60), so t60 = (ln(1000)/pi) * q / f.
  static constexpr float kT60SecPerQHz = 2.19848f;  // ln(1000) / pi

  static float q_to_t60(float freq_hz, float q) noexcept { return kT60SecPerQHz * q / freq_hz; }

  /// Expands a literature modal table (freq/Q/weight) into resonator Specs,
  /// scaling every weight by @p level so the bank's broadband contribution
  /// stays matched to the earlier hand-placed voicing (the per-preset body_mix
  /// was calibrated against it). Returns the number of Specs written.
  template <size_t N>
  static int fill_from_modal(std::array<Spec, kMaxModes>& out, const std::array<ModeQ, N>& bank,
                             float level) noexcept {
    const int count = static_cast<int>(std::min<size_t>(N, kMaxModes));
    for (int k = 0; k < count; ++k) {
      const ModeQ& m = bank[static_cast<size_t>(k)];
      out[static_cast<size_t>(k)] = {m.freq_hz, q_to_t60(m.freq_hz, m.q), m.weight * level};
    }
    return count;
  }

  // Violin corpus, from measured mobility/radiativity surveys (Dünnwald,
  // Jansson, Bissinger, Gough). A0 is the Helmholtz air resonance; CBR the
  // centre-bout rhomboid; B1-/B1+ the two main corpus bending modes (B1+ the
  // strongest radiator). 700 Hz-2 kHz is the wood-mode cluster; the broad,
  // low-Q pair near 2.4/2.7 kHz is the "bridge hill" that gives the violin its
  // carrying brilliance; above ~3 kHz the corpus rolls off.
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

  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<Mode, kMaxModes> modes_{};
  // Shared bandpass input history (the zeros are common to every mode).
  float x1_ = 0.0f;
  float x2_ = 0.0f;
  int num_modes_ = 0;
  float mix_ = 0.0f;
};

}  // namespace sonare::midi::synth
