#include "analysis/acoustic_analyzer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "util/exception.h"

namespace sonare {

using namespace acoustic_detail;

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

  static constexpr float kOctaveCenters[] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
  const int n_bands =
      std::min<int>(config_.n_octave_bands, static_cast<int>(std::size(kOctaveCenters)));
  parameters_.rt60_bands.reserve(n_bands);
  parameters_.edt_bands.reserve(n_bands);
  parameters_.c50_bands.reserve(n_bands);
  parameters_.c80_bands.reserve(n_bands);

  for (int i = 0; i < n_bands; ++i) {
    const std::vector<float> filtered = filter_octave_band(ir, kOctaveCenters[i]);
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

  static constexpr float kOctaveCenters[] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
  const int n_bands =
      std::min<int>(config_.n_octave_bands, static_cast<int>(std::size(kOctaveCenters)));
  parameters_.rt60_bands.reserve(n_bands);
  parameters_.edt_bands.reserve(n_bands);

  for (int i = 0; i < n_bands; ++i) {
    const std::vector<float> filtered = filter_octave_band(audio, kOctaveCenters[i]);
    if (filtered.empty()) {
      parameters_.rt60_bands.push_back(nan_value());
      parameters_.edt_bands.push_back(nan_value());
      continue;
    }

    const float ratio = kSqrt2;
    BlindRt60Estimate band = weighted_subband_average(subband_estimates, kOctaveCenters[i] / ratio,
                                                      kOctaveCenters[i] * ratio);
    const BlindRt60Estimate low_frequency_anchor =
        std::isfinite(parameters_.rt60) && parameters_.confidence > 0.0f ? global : subband;
    if (kOctaveCenters[i] < 1000.0f &&
        (band.confidence <= 0.0f ||
         (std::isfinite(low_frequency_anchor.rt60) && std::isfinite(band.rt60) &&
          band.rt60 < low_frequency_anchor.rt60 * 0.7f))) {
      band = estimate_from_frequency_model(frequency_model, kOctaveCenters[i]);
      if (band.confidence <= 0.0f) {
        band = extrapolate_low_frequency_rt60(low_frequency_anchor, kOctaveCenters[i]);
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
