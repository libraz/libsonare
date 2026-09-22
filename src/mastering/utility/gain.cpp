#include "mastering/utility/gain.h"

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::utility {

Gain::Gain(GainConfig config) : config_(config) { validate_config(config_); }

void Gain::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  prepared_ = true;
}

void Gain::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Gain");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  const float gain = db_to_linear(config_.level_db);
  // Unity multiplies every float to itself exactly, so skipping the pass changes
  // no sample; a trim left at its default is the common case in a long chain.
  if (gain == 1.0f) {
    return;
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] *= gain;
    }
  }
}

void Gain::reset() {}

void Gain::set_config(const GainConfig& config) {
  validate_config(config);
  config_ = config;
}

bool Gain::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!numeric::finite(value)) return false;
      config_.level_db = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Gain::parameter_descriptors() const { return {{"levelDb", 0}}; }

void Gain::validate_config(const GainConfig& config) {
  // No window: a gain of -200 dB is silence and one of +200 dB is loud, and both
  // are answers. A non-finite one is not -- it reaches every later stage.
  if (!numeric::finite(config.level_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "levelDb must be finite");
  }
}

}  // namespace sonare::mastering::utility
