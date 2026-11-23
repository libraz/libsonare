#include "mastering/maximizer/maximizer.h"

#include <cmath>

#include "mastering/common/parameter_domain.h"
#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/dynamics/lookahead_validation.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::maximizer {

namespace check = sonare::util::check;
namespace {

bool valid_time_ms(float ms) noexcept { return std::isfinite(ms) && ms >= 0.0f; }

}  // namespace

Maximizer::Maximizer(MaximizerConfig config) : config_(config) { validate_config(config_); }

void Maximizer::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void Maximizer::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "Maximizer");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_, max_channels);
  prepared_ = true;
}

void Maximizer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Maximizer");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  const float gain = db_to_linear(config_.input_gain_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) channels[ch][i] *= gain;
  }
  limiter_.run(detector_excluded_channel(num_channels), channels, num_channels, num_samples);
}

void Maximizer::reset() { limiter_.reset(); }

void Maximizer::set_config(const MaximizerConfig& config) {
  validate_config(config);
  // The limiter's own refusals, checked before anything is committed.
  dynamics::BrickwallLimiter::validate_config(
      {config.ceiling_db, config.lookahead_ms, config.release_ms});
  if (prepared_) (void)dynamics::checked_lookahead_samples(sample_rate_, config.lookahead_ms);
  // The input gain is read per block; the limiter takes the rest and rebuilds
  // only what its own changed fields need.
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<MaximizerConfig>& diff) {
    if (diff.changed(&MaximizerConfig::ceiling_db, &MaximizerConfig::lookahead_ms,
                     &MaximizerConfig::release_ms)) {
      limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
    }
  });
}

bool Maximizer::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Applied per block as a linear gain; no coefficients to recompute.
      if (!numeric::finite(db_to_linear(value))) return false;
      config_.input_gain_db = value;
      return true;
    case 1:
      if (!common::valid_ceiling_db(value)) return false;
      config_.ceiling_db = value;
      // In-place on the inner limiter: no snapshot publish, no allocation.
      if (prepared_) limiter_.set_parameter(0, config_.ceiling_db);
      return true;
    case 2:
      if (!valid_time_ms(value)) return false;
      config_.release_ms = value;
      // RT-safe in-place release coefficient update: recomputes the inner
      // limiter's release coefficient without publishing a snapshot (no
      // allocation), preserving the limiter audio state. Matches the
      // parameter_is_realtime_safe(2) == true contract.
      if (prepared_) {
        limiter_.set_release_ms_in_place(config_.release_ms);
      }
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Maximizer::parameter_descriptors() const {
  return {{"inputGainDb", 0}, {"ceilingDb", 1}, {"releaseMs", 2}};
}

bool Maximizer::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id <= 2u;
}

void Maximizer::validate_config(const MaximizerConfig& config) {
  check::at_most(config.ceiling_db, 0.0, "ceiling_db");
  check::at_least(config.lookahead_ms, 0.0, "lookahead_ms");
  check::at_least(config.release_ms, 0.0, "release_ms");
  check::at_most(config.input_gain_db, sonare::kMaxFiniteGainDb, "input_gain_db");
}

}  // namespace sonare::mastering::maximizer
