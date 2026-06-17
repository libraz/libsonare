#include "mastering/eq/cut_filter.h"

#include <algorithm>
#include <cmath>

#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::eq {
namespace {

using sonare::constants::kButterworthQ;
using sonare::constants::kPiD;

float clamp_frequency(float frequency_hz, double sample_rate) {
  return std::clamp(frequency_hz, 1.0e-3f, static_cast<float>(sample_rate * 0.5) - 1.0e-3f);
}

int slope_db_oct(CutFilterSlope slope) {
  switch (slope) {
    case CutFilterSlope::Db6PerOct:
      return 6;
    case CutFilterSlope::Db12PerOct:
      return 12;
    case CutFilterSlope::Db18PerOct:
      return 18;
    case CutFilterSlope::Db24PerOct:
      return 24;
    case CutFilterSlope::Db30PerOct:
      return 30;
    case CutFilterSlope::Db36PerOct:
      return 36;
    case CutFilterSlope::Db42PerOct:
      return 42;
    case CutFilterSlope::Db48PerOct:
      return 48;
    case CutFilterSlope::Db54PerOct:
      return 54;
    case CutFilterSlope::Db60PerOct:
      return 60;
    case CutFilterSlope::Db66PerOct:
      return 66;
    case CutFilterSlope::Db72PerOct:
      return 72;
    case CutFilterSlope::Db78PerOct:
      return 78;
    case CutFilterSlope::Db84PerOct:
      return 84;
    case CutFilterSlope::Db90PerOct:
      return 90;
    case CutFilterSlope::Db96PerOct:
      return 96;
    case CutFilterSlope::Brickwall:
      return 0;
  }
  throw SonareException(ErrorCode::InvalidParameter, "unsupported cut filter slope");
}

}  // namespace

void CutFilter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  brickwall_.prepare(sample_rate, max_block_size);
  prepared_ = true;
  prepare_channels(2);
  apply_high_pass();
  apply_low_pass();
}

void CutFilter::prepare_channels(int num_channels) {
  if (num_channels < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "num_channels must be non-negative");
  }
  num_channels_ = num_channels;
  for (auto& states : high_pass_states_) {
    states.assign(static_cast<size_t>(num_channels), {});
  }
  for (auto& states : low_pass_states_) {
    states.assign(static_cast<size_t>(num_channels), {});
  }
  brickwall_.prepare_channels(num_channels);
}

void CutFilter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "CutFilter");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  ensure_channel_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
    process_stage(high_pass_sections_, high_pass_states_, channels[ch], ch, num_samples);
    process_stage(low_pass_sections_, low_pass_states_, channels[ch], ch, num_samples);
  }
  if (high_pass_is_brickwall() || low_pass_is_brickwall()) {
    brickwall_.process(channels, num_channels, num_samples);
  }
}

void CutFilter::reset() {
  for (auto& states : high_pass_states_) {
    for (auto& state : states) {
      state = {};
    }
  }
  for (auto& states : low_pass_states_) {
    for (auto& state : states) {
      state = {};
    }
  }
  brickwall_.reset();
}

int CutFilter::latency_samples() const noexcept {
  return (high_pass_is_brickwall() || low_pass_is_brickwall()) ? brickwall_.latency_samples() : 0;
}

void CutFilter::set_high_pass(float frequency_hz, float q, CutFilterSlope slope, bool enabled) {
  high_pass_ = {EqBandType::HighPass, frequency_hz, 0.0f, q, enabled};
  high_pass_slope_ = slope;
  apply_high_pass();
}

void CutFilter::set_low_pass(float frequency_hz, float q, CutFilterSlope slope, bool enabled) {
  low_pass_ = {EqBandType::LowPass, frequency_hz, 0.0f, q, enabled};
  low_pass_slope_ = slope;
  apply_low_pass();
}

void CutFilter::clear_high_pass() {
  high_pass_.enabled = false;
  apply_high_pass();
}

void CutFilter::clear_low_pass() {
  low_pass_.enabled = false;
  apply_low_pass();
}

void CutFilter::clear() {
  high_pass_.enabled = false;
  low_pass_.enabled = false;
  apply_high_pass();
  apply_low_pass();
}

bool CutFilter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      high_pass_.frequency_hz = clamp_frequency(value, sample_rate_);
      apply_high_pass();
      return true;
    case 1:
      high_pass_.q = std::max(value, 1.0e-6f);
      apply_high_pass();
      return true;
    case 2:
      low_pass_.frequency_hz = clamp_frequency(value, sample_rate_);
      apply_low_pass();
      return true;
    case 3:
      low_pass_.q = std::max(value, 1.0e-6f);
      apply_low_pass();
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> CutFilter::parameter_descriptors() const {
  return {{"highPassFrequencyHz", 0}, {"highPassQ", 1}, {"lowPassFrequencyHz", 2}, {"lowPassQ", 3}};
}

void CutFilter::apply_high_pass() {
  high_pass_.frequency_hz = clamp_frequency(high_pass_.frequency_hz, sample_rate_);
  build_sections(high_pass_sections_, EqBandType::HighPass, high_pass_.frequency_hz, high_pass_.q,
                 high_pass_.enabled, high_pass_slope_);
  rebuild_brickwall();
}

void CutFilter::apply_low_pass() {
  low_pass_.frequency_hz = clamp_frequency(low_pass_.frequency_hz, sample_rate_);
  build_sections(low_pass_sections_, EqBandType::LowPass, low_pass_.frequency_hz, low_pass_.q,
                 low_pass_.enabled, low_pass_slope_);
  rebuild_brickwall();
}

void CutFilter::build_sections(std::array<Section, kMaxSections>& sections, EqBandType type,
                               float frequency_hz, float q, bool enabled, CutFilterSlope slope) {
  for (auto& section : sections) {
    section = {};
  }
  if (!enabled || slope == CutFilterSlope::Brickwall) {
    return;
  }

  const int order = slope_db_oct(slope) / 6;
  const float w0 =
      static_cast<float>(2.0 * kPiD * static_cast<double>(frequency_hz) / sample_rate_);
  size_t section_index = 0;
  if ((order % 2) != 0) {
    sections[section_index++] = {type == EqBandType::HighPass ? sonare::rt::first_order_highpass(w0)
                                                              : sonare::rt::first_order_lowpass(w0),
                                 true};
  }

  const int pair_count = order / 2;
  for (int pair = pair_count - 1; pair >= 0; --pair) {
    float stage_q = rt::butterworth_stage_q(order, pair);
    if (pair == pair_count - 1 && std::abs(q - kButterworthQ) > 1.0e-6f) {
      stage_q = std::max(q, 1.0e-6f);
    }
    sections[section_index++] = {type == EqBandType::HighPass
                                     ? sonare::rt::rbj_highpass(w0, stage_q)
                                     : sonare::rt::rbj_lowpass(w0, stage_q),
                                 true};
  }
}

void CutFilter::rebuild_brickwall() {
  EqBand hp{EqBandType::HighPass, high_pass_.frequency_hz, 0.0f, 1.0f,
            high_pass_.enabled && high_pass_is_brickwall()};
  hp.slope_db_oct = 0;
  EqBand lp{EqBandType::LowPass, low_pass_.frequency_hz, 0.0f, 1.0f,
            low_pass_.enabled && low_pass_is_brickwall()};
  lp.slope_db_oct = 0;
  brickwall_.set_band(0, hp);
  brickwall_.set_band(1, lp);
}

void CutFilter::ensure_channel_state(int num_channels) {
  if (num_channels_ >= num_channels) {
    return;
  }
  prepare_channels(num_channels);
}

bool CutFilter::high_pass_is_brickwall() const noexcept {
  return high_pass_slope_ == CutFilterSlope::Brickwall && high_pass_.enabled;
}

bool CutFilter::low_pass_is_brickwall() const noexcept {
  return low_pass_slope_ == CutFilterSlope::Brickwall && low_pass_.enabled;
}

void CutFilter::process_stage(const std::array<Section, kMaxSections>& sections,
                              std::array<std::vector<State>, kMaxSections>& states, float* samples,
                              int channel, int num_samples) const {
  for (size_t section_index = 0; section_index < kMaxSections; ++section_index) {
    if (!sections[section_index].enabled) {
      continue;
    }
    const auto c = sections[section_index].coeffs;
    auto& state = states[section_index][static_cast<size_t>(channel)];
    for (int i = 0; i < num_samples; ++i) {
      const float x = samples[i];
      const float y = c.b0 * x + state.z1;
      state.z1 = c.b1 * x - c.a1 * y + state.z2;
      state.z2 = c.b2 * x - c.a2 * y;
      samples[i] = y;
    }
  }
}

}  // namespace sonare::mastering::eq
