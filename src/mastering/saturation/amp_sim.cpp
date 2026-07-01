#include "mastering/saturation/amp_sim.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

namespace {

// Voicing centres (Hz). Fixed circuit positions, not user parameters.
constexpr float kPreEmphasisHz = 750.0f;  // bright-cap shelf before the clip
constexpr float kBassHz = 120.0f;         // tone stack
constexpr float kMidHz = 550.0f;
constexpr float kTrebleHz = 3000.0f;
constexpr float kCabHighpassHz = 75.0f;  // cab voicing
constexpr float kCabBumpHz = 110.0f;
constexpr float kPresenceHz = 3800.0f;
constexpr float kCabRolloffHz = 4800.0f;

/// drive [0,1] -> triode drive in dB. The low end stays a clean preamp; the
/// top lands in saturated-lead territory.
float drive_to_db(float drive) noexcept { return -10.0f + 44.0f * drive; }

float process_chain(float x, rt::BiquadState& state, const rt::BiquadCoeffs& coeffs) noexcept {
  state.c = coeffs;
  return state.process(x);
}

}  // namespace

AmpSim::AmpSim(AmpSimConfig config)
    : config_(config),
      tube_(TubeConfig{drive_to_db(std::clamp(config.drive, 0.0f, 1.0f)),
                       /*bias=*/0.15f, /*mix=*/1.0f, /*oversample_factor=*/4,
                       /*bias_v=*/-1.6f, /*harmonic_drive=*/1.0f}) {
  validate_config(config_);
  config_.drive = std::clamp(config_.drive, 0.0f, 1.0f);
  config_.power = std::clamp(config_.power, 0.0f, 1.0f);
  config_.sag = std::clamp(config_.sag, 0.0f, 1.0f);
  config_.transformer = std::clamp(config_.transformer, 0.0f, 1.0f);
}

namespace {
/// Push-pull class-AB power stage: a symmetric, gain-compensated soft
/// saturation. Slope 1 at the origin (quiet passages pass unchanged), hard
/// signals compress toward the rails and pick up odd harmonics. @p adaa
/// antialiases the tanh.
float power_stage(float x, float power, rt::Adaa1<rt::TanhNonlinearity>& adaa) noexcept {
  // The power amp has gain: the (quiet) preamp output is driven hard into the
  // tubes. g is large so the stage saturates at the amp's low internal level;
  // dividing by g keeps unity slope at the origin (quiet passages pass, loud
  // ones compress toward the rails).
  const float g = 1.0f + power * 40.0f;
  return adaa.process(g * x) / g;
}
}  // namespace

void AmpSim::validate_config(const AmpSimConfig& config) {
  if (!std::isfinite(config.drive) || !std::isfinite(config.bass_db) ||
      !std::isfinite(config.mid_db) || !std::isfinite(config.treble_db) ||
      !std::isfinite(config.presence_db) || !std::isfinite(config.level_db) ||
      !std::isfinite(config.power) || !std::isfinite(config.sag) ||
      !std::isfinite(config.transformer)) {
    throw SonareException(ErrorCode::InvalidParameter, "amp-sim params must be finite");
  }
}

void AmpSim::prepare(double sample_rate, int max_block_size) {
  if (sample_rate <= 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  sample_rate_ = sample_rate;
  tube_.prepare(sample_rate, max_block_size);
  chains_.assign(dynamics::kRealtimePreparedChannels, ChannelChain{});
  power_adaa_.assign(dynamics::kRealtimePreparedChannels, rt::Adaa1<rt::TanhNonlinearity>{});
  design_chain();
  prepared_ = true;
  reset();
}

void AmpSim::design_chain() {
  // Pre-emphasis: more drive pushes more top end into the clip stage.
  const float pre_db = 2.0f + 6.0f * config_.drive;
  pre_c_ = rt::rbj_high_shelf(rt::frequency_to_w0(kPreEmphasisHz, sample_rate_), 0.707f, pre_db);
  bass_c_ = rt::rbj_low_shelf(rt::frequency_to_w0(kBassHz, sample_rate_), 0.707f, config_.bass_db);
  mid_c_ = rt::rbj_peak(rt::frequency_to_w0(kMidHz, sample_rate_), 0.7f, config_.mid_db);
  treble_c_ =
      rt::rbj_high_shelf(rt::frequency_to_w0(kTrebleHz, sample_rate_), 0.707f, config_.treble_db);
  hp_c_ = rt::rbj_highpass(rt::frequency_to_w0(kCabHighpassHz, sample_rate_), 0.707f);
  bump_c_ = rt::rbj_peak(rt::frequency_to_w0(kCabBumpHz, sample_rate_), 1.0f, 2.0f);
  presence_c_ =
      rt::rbj_peak(rt::frequency_to_w0(kPresenceHz, sample_rate_), 1.0f, config_.presence_db);
  // 4th-order Butterworth roll-off: the steep top-end cut is the single
  // strongest "cabinet" cue.
  lp1_c_ = rt::rbj_lowpass(rt::frequency_to_w0(kCabRolloffHz, sample_rate_),
                           rt::butterworth_stage_q(4, 0));
  lp2_c_ = rt::rbj_lowpass(rt::frequency_to_w0(kCabRolloffHz, sample_rate_),
                           rt::butterworth_stage_q(4, 1));
  level_gain_ = sonare::db_to_linear(config_.level_db);
  // Sag envelope: ~40 ms reservoir-cap time constant.
  constexpr float kSagTauS = 0.04f;
  sag_alpha_ = std::clamp(1.0f - std::exp(-1.0f / (kSagTauS * static_cast<float>(sample_rate_))),
                          0.0f, 1.0f);
  // Transformer low-band extractor: a ~120 Hz one-pole corner.
  constexpr float kXfCornerHz = 120.0f;
  xf_alpha_ = std::clamp(
      1.0f - std::exp(-2.0f * 3.14159265358979f * kXfCornerHz / static_cast<float>(sample_rate_)),
      0.0f, 1.0f);
}

void AmpSim::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "AmpSim");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  }
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  if (chains_.size() < static_cast<size_t>(num_channels)) {
    // Control-thread growth only, mirroring Tube::ensure_state.
    chains_.assign(static_cast<size_t>(num_channels), ChannelChain{});
    power_adaa_.assign(static_cast<size_t>(num_channels), rt::Adaa1<rt::TanhNonlinearity>{});
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }

  // Pre-emphasis in front of the (oversampled, block-based) tube stage.
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelChain& chain = chains_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_chain(channels[ch][i], chain.pre, pre_c_);
    }
  }
  tube_.process(channels, num_channels, num_samples);
  // Tone stack + cab voicing + level.
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelChain& chain = chains_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      float s = channels[ch][i];
      s = process_chain(s, chain.bass, bass_c_);
      s = process_chain(s, chain.mid, mid_c_);
      s = process_chain(s, chain.treble, treble_c_);
      // Push-pull power amp (after the tone stack, before the cab). Off when
      // power == 0 -> the ADAA state is untouched and the path is bit-identical.
      if (config_.power > 0.0f) {
        s = power_stage(s, config_.power, power_adaa_[static_cast<size_t>(ch)]);
      }
      // Power-supply sag: a lagged envelope of the draw pulls the rail down, so
      // the attack punches through and the sustain compresses. Off -> the
      // envelope is untouched and the path is bit-identical.
      if (config_.sag > 0.0f) {
        chain.sag_env += sag_alpha_ * (std::fabs(s) - chain.sag_env);
        // The rail droops with the draw. The envelope tracks the amp's low
        // internal level, so the sensitivity is scaled up to reach a musical
        // droop by the sustain.
        const float droop = std::min(0.6f, config_.sag * 12.0f * chain.sag_env);
        s *= 1.0f - droop;
      }
      // Output transformer: the core saturates with the flux, i.e. at low
      // frequencies. Saturate the extracted low band only and pass the highs
      // linearly. Off -> the lowpass state is untouched and the path is
      // bit-identical.
      if (config_.transformer > 0.0f) {
        chain.xf_lp += xf_alpha_ * (s - chain.xf_lp);
        // Drive the extracted low band hard so the core saturates at the amp's
        // low internal level; the gain-compensated soft clip keeps unity slope.
        const float g = 1.0f + config_.transformer * 30.0f;
        const float sat_low = std::tanh(g * chain.xf_lp) / g;
        s += sat_low - chain.xf_lp;  // replace the low band with its saturated copy
      }
      if (config_.cab) {
        s = process_chain(s, chain.hp, hp_c_);
        s = process_chain(s, chain.bump, bump_c_);
        s = process_chain(s, chain.presence, presence_c_);
        s = process_chain(s, chain.lp1, lp1_c_);
        s = process_chain(s, chain.lp2, lp2_c_);
      }
      channels[ch][i] = s * level_gain_;
    }
  }
}

void AmpSim::reset() {
  tube_.reset();
  for (ChannelChain& chain : chains_) chain = ChannelChain{};
  for (auto& adaa : power_adaa_) adaa.reset();
}

bool AmpSim::set_parameter(unsigned int param_id, float value) {
  if (!std::isfinite(value)) return false;
  switch (param_id) {
    case 0:
      config_.drive = std::clamp(value, 0.0f, 1.0f);
      tube_.set_parameter(0, drive_to_db(config_.drive));
      break;
    case 1:
      config_.bass_db = value;
      break;
    case 2:
      config_.mid_db = value;
      break;
    case 3:
      config_.treble_db = value;
      break;
    case 4:
      config_.presence_db = value;
      break;
    case 5:
      config_.level_db = value;
      break;
    case 6:
      config_.power = std::clamp(value, 0.0f, 1.0f);
      break;
    case 7:
      config_.sag = std::clamp(value, 0.0f, 1.0f);
      break;
    case 8:
      config_.transformer = std::clamp(value, 0.0f, 1.0f);
      break;
    default:
      return false;
  }
  design_chain();
  return true;
}

std::vector<rt::ParamDescriptor> AmpSim::parameter_descriptors() const {
  return {{"drive", 0},   {"bassDb", 1}, {"midDb", 2}, {"trebleDb", 3},   {"presenceDb", 4},
          {"levelDb", 5}, {"power", 6},  {"sag", 7},   {"transformer", 8}};
}

}  // namespace sonare::mastering::saturation
