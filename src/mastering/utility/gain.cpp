#include "mastering/utility/gain.h"

#include "mastering/common/prepare_args.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::utility {

Gain::Gain(GainConfig config) : config_(config) { validate_config(config_); }

void Gain::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);
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
  // Every field is read per sample; nothing is rebuilt.
  rt::apply_config_diff(config_, config, [](const rt::ConfigDiff<GainConfig>&) {});
}

bool Gain::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!numeric::finite(value) || !numeric::finite(db_to_linear(value))) return false;
      config_.level_db = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Gain::parameter_descriptors() const { return {{"levelDb", 0}}; }

void Gain::validate_config(const GainConfig& config) {
  // Allow any level whose linear multiplier is finite, including underflow to zero.
  if (!numeric::finite(config.level_db) || !numeric::finite(db_to_linear(config.level_db))) {
    throw SonareException(ErrorCode::InvalidParameter, "levelDb must produce a finite linear gain");
  }
}

}  // namespace sonare::mastering::utility
