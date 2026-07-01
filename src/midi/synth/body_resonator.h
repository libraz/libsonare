#pragma once

/// @file body_resonator.h
/// @brief Fixed body/formant resonance bank for NativeSynth voices — the
///        cheap end of commuted synthesis (Smith & Van Duyne): a handful of
///        low-Q two-pole resonators approximating an instrument body's
///        dominant modes, mixed over the dry voice.
///
/// Three data-free voicings:
///   - kGuitar: dreadnought-ish air + plate modes (~100/200/400/550 Hz).
///   - kViolin: A0 air + T1 plate region (~275/450/560/700 Hz).
///   - kWoodTube: the tuned pipe under a marimba/xylophone bar — note-tracked
///     (the tube is cut for its bar), one strong fundamental resonance plus a
///     faint upper mode.
///   - kBrassBell: the flaring bell's radiation resonance — a broad "brass
///     formant" near 1.2 kHz with brilliance modes above it. Fixed by bell
///     geometry (note-independent), tuned for the trumpet family; larger-bore
///     brass keeps kNone.
///
/// RT contract: start()/process() are allocation-free; determinism: fixed
/// tables, no RNG.

#include <algorithm>
#include <array>
#include <cmath>

namespace sonare::midi::synth {

/// Body voicing selector (patch field; kNone = bypass).
enum class BodyType : int {
  kNone = 0,
  kGuitar = 1,
  kViolin = 2,
  kWoodTube = 3,
  kBrassBell = 4,
};

class BodyResonator {
 public:
  static constexpr int kMaxModes = 4;

  /// One resonant body mode: a low-Q bandpass at @p freq_hz with reverb time
  /// @p t60_s, mixed in at @p weight.
  struct Spec {
    float freq_hz;
    float t60_s;
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
      const float w = 6.28318530718f * spec.freq_hz / static_cast<float>(sr);
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
        specs = {{{275.0f, 0.08f, 1.0f},
                  {450.0f, 0.07f, 0.8f},
                  {560.0f, 0.06f, 0.6f},
                  {700.0f, 0.05f, 0.4f}}};
        count = 4;
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
