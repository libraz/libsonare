#pragma once

/// @file oscillator.h
/// @brief Antialiased virtual-analog oscillator for the NativeSynth engine:
///        PolyBLEP-corrected saw/square, a leaky-integrated (BLAMP-equivalent)
///        triangle, a pure sine and a seeded deterministic noise source.
///
/// Naive trivially-sampled saw/square waveforms alias badly above a few
/// hundred Hz; the 2-sample polynomial band-limited step (PolyBLEP) residual
/// applied at each discontinuity suppresses the folded partials far below
/// audibility at a fraction of the cost of BLIT/wavetable schemes. The
/// triangle integrates the PolyBLEP square through a leaky integrator, which
/// is the classic BLAMP-equivalent corner smoothing.
///
/// RT contract: start()/set_frequency()/next() are allocation-free and run on
/// the audio thread. Determinism: no RNG, no wall clock — kNoise draws from
/// the counter-based voice_random hash so identical (seed, sample index)
/// streams are bit-identical.

#include <cstdint>

namespace sonare::midi::synth {

/// Waveform selector for VaOscillator. kNoise ignores the oscillator
/// frequency and emits seeded white noise (drum/SFX fallbacks).
enum class VaWaveform : int {
  kSine = 0,
  kSaw = 1,
  kSquare = 2,
  kTriangle = 3,
  kNoise = 4,
};

class VaOscillator {
 public:
  /// Start the oscillator at @p phase01 in [0,1). @p noise_seed feeds the
  /// deterministic kNoise stream (derive it from the voice seed).
  void start(double sample_rate, VaWaveform waveform, float phase01, uint64_t noise_seed) noexcept;

  /// Per-sample frequency update (pitch modulation); clamped to [0, Nyquist).
  void set_frequency(float freq_hz) noexcept;

  /// Render one sample in [-1, 1].
  float next() noexcept;

 private:
  VaWaveform waveform_ = VaWaveform::kSaw;
  float sample_rate_ = 48000.0f;
  float phase_ = 0.0f;  // [0,1)
  float inc_ = 0.0f;    // cycles per sample
  /// Leaky-integrator state for the triangle (integrated PolyBLEP square).
  float tri_state_ = 0.0f;
  uint64_t noise_seed_ = 0;
  uint64_t noise_counter_ = 0;
};

}  // namespace sonare::midi::synth
