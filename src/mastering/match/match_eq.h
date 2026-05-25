#pragma once

/// @file match_eq.h
/// @brief Convert source/reference spectrum differences into EQ bands.

#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/parametric.h"
#include "mastering/match/reference_spectrum.h"

namespace sonare::mastering::match {

struct MatchEqConfig {
  size_t max_bands = 8;
  float max_gain_db = 12.0f;
  float min_frequency_hz = 40.0f;
  float max_frequency_hz = 18000.0f;
  float q = 1.0f;
  int smoothing_bins = 2;
};

enum class MatchEqFirPhase {
  LinearPhase,
  MinimumPhase,
};

struct MatchEqCurve {
  std::vector<float> frequencies;
  std::vector<float> gain_db;
};

struct MatchEqFirConfig {
  int fft_size = 2048;
  int kernel_size = 513;
  MatchEqFirPhase phase = MatchEqFirPhase::LinearPhase;
  /// 0 selects a reasonable default for offline partitioned convolution.
  int partition_size = 0;
};

MatchEqCurve match_eq_curve(const ReferenceSpectrum& source, const ReferenceSpectrum& reference,
                            const MatchEqConfig& config = {});
std::vector<float> match_eq_fir_kernel(const MatchEqCurve& curve, int sample_rate,
                                       const MatchEqFirConfig& config = {});
Audio apply_match_eq(const Audio& audio, const ReferenceSpectrum& source,
                     const ReferenceSpectrum& reference, const MatchEqConfig& match_config = {},
                     const MatchEqFirConfig& fir_config = {});
float estimate_reference_delay_samples(const Audio& source, const Audio& reference,
                                       int max_abs_delay);
Audio align_reference_to_source(const Audio& source, const Audio& reference, int max_abs_delay);
std::vector<eq::EqBand> match_eq_bands(const ReferenceSpectrum& source,
                                       const ReferenceSpectrum& reference,
                                       const MatchEqConfig& config = {});
std::vector<eq::EqBand> match_eq_bands_from_curve(const MatchEqCurve& curve,
                                                  const MatchEqConfig& config = {});
void configure_equalizer_from_match(eq::EqualizerProcessor& equalizer,
                                    const ReferenceSpectrum& source,
                                    const ReferenceSpectrum& reference,
                                    const MatchEqConfig& config = {});

}  // namespace sonare::mastering::match
