#pragma once

/// @file spectral_edit.h
/// @brief Region-based spectral editing (offline gain/attenuate/mute/heal).

#include <cstddef>
#include <cstdint>

#include "core/audio.h"
#include "util/types.h"

namespace sonare {

/// @brief How a spectral region op modifies the masked bins.
enum class SpectralEditMode : uint8_t {
  Gain = 0,       ///< Multiply masked magnitude by 10^(gain_db/20); phase preserved.
  Attenuate = 1,  ///< Gain with a (typically negative) gain_db; phase preserved.
  Mute = 2,       ///< Hard zero of the masked bins (gain_db ignored).
  Heal = 3,       ///< Replace masked bins by interpolation from neighbouring time frames.
};

/// @brief A single time x frequency rectangle edit op.
/// @details Region edges are clamped to the valid sample/bin ranges. Ops apply
/// sequentially over a single mutable STFT buffer, so a later op observes the
/// result of earlier ops (e.g. attenuate-then-heal over overlapping regions).
struct SpectralRegionOp {
  int64_t start_sample = 0;  ///< Region time start (input sample domain). Clamped to [0, length].
  int64_t end_sample = 0;    ///< Region time end, exclusive. Clamped to [0, length].
  float low_hz = 0.0f;       ///< Region frequency low edge (Hz). Clamped to [0, nyquist].
  float high_hz = 0.0f;      ///< Region frequency high edge (Hz). <=0 or >= nyquist => nyquist.
  float gain_db = 0.0f;      ///< Linear gain in dB for Gain/Attenuate; ignored by Mute/Heal.
  SpectralEditMode mode = SpectralEditMode::Gain;  ///< Edit mode.
};

/// @brief STFT + heal parameters for a spectral edit pass.
struct SpectralEditConfig {
  int n_fft = 2048;      ///< FFT size; must be a power of two (>= 2). Mirrors StftConfig default.
  int hop_length = 512;  ///< Hop length; must satisfy 0 < hop <= n_fft/2 (COLA).
  WindowType window = WindowType::Hann;  ///< Analysis + synthesis window.
  int heal_radius_frames = 2;            ///< Neighbour frames each side used by Heal (>= 1).
};

/// @brief Applies region-based spectral edits to a mono signal and resynthesizes.
/// @details Pipeline: STFT (Spectrogram::compute) -> per-op bin/frame masking on
/// a mutable copy of the complex spectrum -> inverse STFT (Spectrogram::to_audio).
/// Gain/Attenuate/Mute scale the complex bin by a real factor (phase-coherent), so
/// an empty op list (or gain_db == 0 over a full band) reconstructs the input
/// within the iSTFT's own tolerance. The output length equals the input length.
///
/// Pure and deterministic: no RNG, no wall-clock. Same audio + config + ops yields
/// bit-identical output.
///
/// @param audio Input audio (mono).
/// @param config STFT + heal configuration.
/// @param ops Pointer to @p n_ops region ops; may be null iff @p n_ops == 0.
/// @param n_ops Number of region ops.
/// @return Edited audio, same length and sample rate as @p audio.
/// @throws SonareException(InvalidParameter) if @p audio is empty, @p n_fft is not
///         a power of two >= 2, @p hop_length is out of (0, n_fft/2], or
///         @p ops is null with @p n_ops > 0.
Audio spectral_edit(const Audio& audio, const SpectralEditConfig& config,
                    const SpectralRegionOp* ops, std::size_t n_ops);

}  // namespace sonare
