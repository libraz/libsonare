#pragma once

#include <sonare/sonare_c_mixing.h>

#include "mixing/meter.h"

namespace sonare_c_mixing_detail {

inline void set_meter_measurement_defaults(int32_t& measure_lufs, int32_t& measure_true_peak,
                                           int32_t& true_peak_oversample) {
  const auto& defaults = sonare::mixing::kPublishedMeterConfig;
  measure_lufs = defaults.measure_lufs ? 1 : 0;
  measure_true_peak = defaults.measure_true_peak ? 1 : 0;
  true_peak_oversample = defaults.true_peak_oversample;
}

inline void initialize_strip_meter_options(SonareMixerAddStripOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->enable_metering = 1;
  set_meter_measurement_defaults(options->measure_lufs, options->measure_true_peak,
                                 options->true_peak_oversample);
}

inline void initialize_master_meter_config(SonareMixerMasterMeterConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  config->enabled = 1;
  set_meter_measurement_defaults(config->measure_lufs, config->measure_true_peak,
                                 config->true_peak_oversample);
}

}  // namespace sonare_c_mixing_detail
