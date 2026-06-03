#include "analysis/acoustic_analyzer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "util/exception.h"

namespace sonare {

using namespace acoustic_detail;

namespace {

// Octave-band centre frequency for band index i, matching the synthesizer's
// convention in src/acoustic/late_reverb.cpp (octave_center_hz): band 0 = 125
// Hz, each subsequent band one octave higher. Generating the centres from the
// same formula (instead of a fixed 6-entry table) lets the analyzer honor
// n_octave_bands > 6 instead of silently truncating; bands whose passband
// exceeds Nyquist fall out as NaN inside filter_octave_band, which is reported
// rather than dropped. This keeps analyzer and synth band axes aligned so an
// estimate->synthesize round-trip does not lose high bands.
float octave_band_center_hz(int band) { return 125.0f * std::pow(2.0f, static_cast<float>(band)); }

}  // namespace

AcousticAnalyzer::AcousticAnalyzer(const Audio& audio, const AcousticConfig& config)
    : AcousticAnalyzer(
          audio, config,
          config.mode == AcousticConfig::Mode::ImpulseResponse ||
              (config.mode == AcousticConfig::Mode::Auto && looks_like_impulse_response(audio))) {}

AcousticAnalyzer AcousticAnalyzer::from_impulse_response(const Audio& ir,
                                                         const AcousticConfig& config) {
  AcousticConfig ir_config = config;
  ir_config.mode = AcousticConfig::Mode::ImpulseResponse;
  return AcousticAnalyzer(ir, ir_config, true);
}

AcousticAnalyzer::AcousticAnalyzer(const Audio& audio, const AcousticConfig& config,
                                   bool impulse_response)
    : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(audio.sample_rate() > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.n_octave_bands >= 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.min_decay_db > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.noise_floor_margin_db >= 0.0f, ErrorCode::InvalidParameter);

  if (impulse_response) {
    analyze_impulse_response(audio);
  } else {
    analyze_blind(audio);
  }
}

void AcousticAnalyzer::analyze_impulse_response(const Audio& ir) {
  parameters_ = analyze_band(ir.data(), ir.size(), ir.sample_rate(), config_.min_decay_db);
  parameters_.is_blind = false;

  // Generate as many octave centres as requested (125 Hz, 250 Hz, ...) rather
  // than capping at a fixed 6-entry table; centres above Nyquist resolve to NaN
  // bands via filter_octave_band instead of being silently dropped.
  const int n_bands = config_.n_octave_bands;
  parameters_.rt60_bands.reserve(n_bands);
  parameters_.edt_bands.reserve(n_bands);
  parameters_.c50_bands.reserve(n_bands);
  parameters_.c80_bands.reserve(n_bands);

  for (int i = 0; i < n_bands; ++i) {
    const std::vector<float> filtered = filter_octave_band(ir, octave_band_center_hz(i));
    if (filtered.empty()) {
      parameters_.rt60_bands.push_back(nan_value());
      parameters_.edt_bands.push_back(nan_value());
      parameters_.c50_bands.push_back(nan_value());
      parameters_.c80_bands.push_back(nan_value());
      continue;
    }

    const AcousticParameters band =
        analyze_band(filtered.data(), filtered.size(), ir.sample_rate(), config_.min_decay_db);
    parameters_.rt60_bands.push_back(band.rt60);
    parameters_.edt_bands.push_back(band.edt);
    parameters_.c50_bands.push_back(band.c50);
    parameters_.c80_bands.push_back(band.c80);
  }
}

void AcousticAnalyzer::analyze_blind(const Audio& audio) {
  parameters_.rt60 = nan_value();
  parameters_.edt = nan_value();
  parameters_.c50 = nan_value();
  parameters_.c80 = nan_value();
  parameters_.d50 = nan_value();
  parameters_.confidence = 0.0f;
  parameters_.is_blind = true;

  const std::vector<BlindRt60Estimate> subband_estimates =
      estimate_third_octave_rt60(audio, config_);
  const FrequencyRtModel frequency_model =
      fit_frequency_dependent_rt_model(subband_estimates, 1000.0f, 10000.0f);
  BlindRt60Estimate subband = weighted_subband_average(subband_estimates, 1000.0f, 4000.0f);
  if (subband.confidence <= 0.0f) {
    subband = weighted_subband_average(subband_estimates, 125.0f, 10000.0f);
  }

  const BlindRt60Estimate fullband =
      estimate_blind_rt60_from_decay(audio.data(), audio.size(), audio.sample_rate(),
                                     config_.min_decay_db, config_.noise_floor_margin_db);
  BlindRt60Estimate global = subband;
  if (subband.confidence > 0.0f && fullband.confidence > 0.0f) {
    const float ratio = subband.rt60 / fullband.rt60;
    if (ratio < 0.75f || ratio > 1.35f) {
      global = fullband;
    }
  } else if (fullband.confidence > 0.0f) {
    global = fullband;
  }

  if (global.confidence > 0.0f) {
    parameters_.rt60 = global.rt60;
    parameters_.edt = global.rt60;
    parameters_.confidence = global.confidence;
  }

  // Generate as many octave centres as requested (125 Hz, 250 Hz, ...) rather
  // than capping at a fixed 6-entry table; centres above Nyquist resolve to NaN
  // bands via filter_octave_band instead of being silently dropped.
  const int n_bands = config_.n_octave_bands;
  parameters_.rt60_bands.reserve(n_bands);
  parameters_.edt_bands.reserve(n_bands);

  for (int i = 0; i < n_bands; ++i) {
    const float center = octave_band_center_hz(i);
    const std::vector<float> filtered = filter_octave_band(audio, center);
    if (filtered.empty()) {
      parameters_.rt60_bands.push_back(nan_value());
      parameters_.edt_bands.push_back(nan_value());
      continue;
    }

    const float ratio = kSqrt2;
    BlindRt60Estimate band =
        weighted_subband_average(subband_estimates, center / ratio, center * ratio);
    const BlindRt60Estimate low_frequency_anchor =
        std::isfinite(parameters_.rt60) && parameters_.confidence > 0.0f ? global : subband;
    if (center < 1000.0f &&
        (band.confidence <= 0.0f ||
         (std::isfinite(low_frequency_anchor.rt60) && std::isfinite(band.rt60) &&
          band.rt60 < low_frequency_anchor.rt60 * 0.7f))) {
      band = estimate_from_frequency_model(frequency_model, center);
      if (band.confidence <= 0.0f) {
        band = extrapolate_low_frequency_rt60(low_frequency_anchor, center);
      }
    }
    if (band.confidence <= 0.0f) {
      band = estimate_blind_rt60_from_decay(filtered.data(), filtered.size(), audio.sample_rate(),
                                            config_.min_decay_db, config_.noise_floor_margin_db,
                                            false);
    }
    parameters_.rt60_bands.push_back(band.rt60);
    parameters_.edt_bands.push_back(band.confidence > 0.0f ? band.rt60 : nan_value());
  }
}

void AcousticAnalyzer::set_unsupported_blind_result() {
  parameters_.rt60 = nan_value();
  parameters_.edt = nan_value();
  parameters_.c50 = nan_value();
  parameters_.c80 = nan_value();
  parameters_.d50 = nan_value();
  parameters_.confidence = 0.0f;
  parameters_.is_blind = true;
}

AcousticParameters detect_acoustic(const Audio& audio, const AcousticConfig& config) {
  return AcousticAnalyzer(audio, config).parameters();
}

AcousticParameters analyze_impulse_response(const Audio& ir, const AcousticConfig& config) {
  return AcousticAnalyzer::from_impulse_response(ir, config).parameters();
}

}  // namespace sonare
