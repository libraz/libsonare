#include "mastering/saturation/amp_sim.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

using constants::kButterworthQ;
using constants::kTwoPi;

namespace {

// Amp voicing: the fixed preamp/tone circuit positions per AmpModel. Not user
// parameters — the drive/tone knobs ride on top of the selected profile.
struct AmpVoicing {
  float pre_emphasis_hz;  // bright-cap shelf centre before the clip
  float pre_db_base;      // pre-emphasis shelf gain (dB) at drive 0
  float pre_db_drive;     // added shelf gain (dB) per unit drive
  float drive_db_base;    // triode drive (dB) at drive 0
  float drive_db_range;   // added triode drive (dB) per unit drive
  float bass_hz;          // tone stack low shelf
  float mid_hz;           // tone stack mid peak
  float treble_hz;        // tone stack high shelf
};

// Classic crunch (default): the original AmpSim voicing. amp_model 0 selects it,
// so an unset field is bit-identical to the original.
constexpr AmpVoicing kAmpClassicCrunch{750.0f, 2.0f, 6.0f, -10.0f, 44.0f, 120.0f, 550.0f, 3000.0f};
// American clean: less bright-cap grit and lower gain (breaks up later), a
// darker mid and an airier top.
constexpr AmpVoicing kAmpFenderClean{650.0f, 1.0f, 4.0f, -16.0f, 36.0f, 100.0f, 500.0f, 3200.0f};
// Modern high-gain: more pre-emphasis into the clip and a much hotter triode
// drive (saturates early), with an upper-mid focus that keeps it articulate.
constexpr AmpVoicing kAmpModernHiGain{900.0f, 3.0f, 9.0f, -4.0f, 52.0f, 110.0f, 650.0f, 3000.0f};
// Vintage tweed: little bright-cap grit, an early spongy breakup and a warm,
// dark voice (low top, full low-mid).
constexpr AmpVoicing kAmpTweed{600.0f, 2.0f, 5.0f, -8.0f, 40.0f, 130.0f, 500.0f, 2600.0f};
// British class-A chime: a bright, upper-mid-forward voice that stays fairly
// clean, with an airy top.
constexpr AmpVoicing kAmpVoxChime{1000.0f, 2.0f, 6.0f, -12.0f, 40.0f, 90.0f, 700.0f, 3400.0f};
// Modern rectifier: the hottest triode drive, a thick low end, a scooped mid
// and a darker top.
constexpr AmpVoicing kAmpRectifier{850.0f, 3.0f, 10.0f, -2.0f, 56.0f, 90.0f, 480.0f, 2800.0f};

AmpVoicing amp_voicing(AmpModel model) noexcept {
  switch (model) {
    case AmpModel::kFenderClean:
      return kAmpFenderClean;
    case AmpModel::kModernHiGain:
      return kAmpModernHiGain;
    case AmpModel::kTweed:
      return kAmpTweed;
    case AmpModel::kVoxChime:
      return kAmpVoxChime;
    case AmpModel::kRectifier:
      return kAmpRectifier;
    default:
      return kAmpClassicCrunch;
  }
}

// Cab voicing: fixed EQ centres per cabinet model (see CabModel). The guitar
// values are the original AmpSim voicing; the bass values model a big 8x10.
struct CabVoicing {
  float highpass_hz;  // low cut
  float bump_hz;      // body bump centre
  float bump_db;      // body bump gain
  float presence_hz;  // presence peak centre
  float rolloff_hz;   // top-end roll-off corner
};

constexpr CabVoicing kCabGuitar4x12{75.0f, 110.0f, 2.0f, 3800.0f, 4800.0f};
constexpr CabVoicing kCabBass8x10{40.0f, 80.0f, 3.0f, 2200.0f, 3500.0f};

CabVoicing cab_voicing(CabModel model) noexcept {
  return model == CabModel::kBass8x10 ? kCabBass8x10 : kCabGuitar4x12;
}

/// drive [0,1] -> triode drive in dB for a voicing. The low end stays a clean
/// preamp; the top lands in saturated-lead territory. The hotter the voicing,
/// the earlier and harder it saturates.
float drive_to_db(float drive, const AmpVoicing& v) noexcept {
  return v.drive_db_base + v.drive_db_range * drive;
}

float process_chain(float x, rt::BiquadState& state, const rt::BiquadCoeffs& coeffs) noexcept {
  state.c = coeffs;
  return state.process(x);
}

}  // namespace

AmpSim::AmpSim(AmpSimConfig config)
    : config_(config),
      tube_(TubeConfig{
          drive_to_db(std::clamp(config.drive, 0.0f, 1.0f), amp_voicing(config.amp_model)),
          /*bias=*/0.15f, /*mix=*/1.0f, /*oversample_factor=*/4,
          /*bias_v=*/-1.6f, /*harmonic_drive=*/1.0f}) {
  validate_config(config_);
  config_.drive = std::clamp(config_.drive, 0.0f, 1.0f);
  config_.power = std::clamp(config_.power, 0.0f, 1.0f);
  config_.sag = std::clamp(config_.sag, 0.0f, 1.0f);
  config_.transformer = std::clamp(config_.transformer, 0.0f, 1.0f);
  config_.nfb = std::clamp(config_.nfb, 0.0f, 1.0f);
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
      !std::isfinite(config.transformer) || !std::isfinite(config.nfb)) {
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
  const AmpVoicing v = amp_voicing(config_.amp_model);
  // Pre-emphasis: more drive pushes more top end into the clip stage.
  const float pre_db = v.pre_db_base + v.pre_db_drive * config_.drive;
  pre_c_ = rt::rbj_high_shelf(rt::frequency_to_w0(v.pre_emphasis_hz, sample_rate_), kButterworthQ,
                              pre_db);
  bass_c_ = rt::rbj_low_shelf(rt::frequency_to_w0(v.bass_hz, sample_rate_), kButterworthQ,
                              config_.bass_db);
  mid_c_ = rt::rbj_peak(rt::frequency_to_w0(v.mid_hz, sample_rate_), 0.7f, config_.mid_db);
  treble_c_ = rt::rbj_high_shelf(rt::frequency_to_w0(v.treble_hz, sample_rate_), kButterworthQ,
                                 config_.treble_db);
  const CabVoicing cab = cab_voicing(config_.cab_model);
  hp_c_ = rt::rbj_highpass(rt::frequency_to_w0(cab.highpass_hz, sample_rate_), kButterworthQ);
  bump_c_ = rt::rbj_peak(rt::frequency_to_w0(cab.bump_hz, sample_rate_), 1.0f, cab.bump_db);
  presence_c_ =
      rt::rbj_peak(rt::frequency_to_w0(cab.presence_hz, sample_rate_), 1.0f, config_.presence_db);
  // 4th-order Butterworth roll-off: the steep top-end cut is the single
  // strongest "cabinet" cue.
  lp1_c_ = rt::rbj_lowpass(rt::frequency_to_w0(cab.rolloff_hz, sample_rate_),
                           rt::butterworth_stage_q(4, 0));
  lp2_c_ = rt::rbj_lowpass(rt::frequency_to_w0(cab.rolloff_hz, sample_rate_),
                           rt::butterworth_stage_q(4, 1));
  level_gain_ = sonare::db_to_linear(config_.level_db);
  // Sag envelope: ~40 ms reservoir-cap time constant.
  constexpr float kSagTauS = 0.04f;
  sag_alpha_ = std::clamp(1.0f - std::exp(-1.0f / (kSagTauS * static_cast<float>(sample_rate_))),
                          0.0f, 1.0f);
  // Transformer low-band extractor: a ~120 Hz one-pole corner.
  constexpr float kXfCornerHz = 120.0f;
  xf_alpha_ = std::clamp(1.0f - std::exp(-kTwoPi * kXfCornerHz / static_cast<float>(sample_rate_)),
                         0.0f, 1.0f);
  // NFB feedback path: a wide mid-band band-pass (0 dB peak), so the midrange is
  // fed back hard (tight, flat) and the extremes escape the loop (the top and
  // bottom "open up").
  constexpr float kNfbCentreHz = 800.0f;
  constexpr float kNfbQ = 0.5f;
  nfb_shape_c_ = rt::rbj_bandpass(rt::frequency_to_w0(kNfbCentreHz, sample_rate_), kNfbQ);
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
        if (config_.nfb > 0.0f) {
          // Global negative feedback around the power stage: a one-sample-delayed
          // copy of the output, shaped by the mid-band filter, is subtracted from
          // the input. The mid is fed back hard (tightened); the extremes are not.
          // beta stays < 1 (with the 0 dB-peak band-pass and the <=1 power-stage
          // slope) so the loop is contractive and bounded. nfb == 0 skips this
          // whole branch -> the open-loop path is bit-identical.
          constexpr float kNfbBeta = 0.7f;
          const float shaped = process_chain(chain.nfb_fb, chain.nfb_shape, nfb_shape_c_);
          const float e = s - kNfbBeta * config_.nfb * shaped;
          s = power_stage(e, config_.power, power_adaa_[static_cast<size_t>(ch)]);
          chain.nfb_fb = s;
        } else {
          s = power_stage(s, config_.power, power_adaa_[static_cast<size_t>(ch)]);
        }
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
      tube_.set_parameter(0, drive_to_db(config_.drive, amp_voicing(config_.amp_model)));
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
    case 9:
      config_.nfb = std::clamp(value, 0.0f, 1.0f);
      break;
    default:
      return false;
  }
  design_chain();
  return true;
}

std::vector<rt::ParamDescriptor> AmpSim::parameter_descriptors() const {
  return {{"drive", 0},   {"bassDb", 1}, {"midDb", 2}, {"trebleDb", 3},    {"presenceDb", 4},
          {"levelDb", 5}, {"power", 6},  {"sag", 7},   {"transformer", 8}, {"nfb", 9}};
}

}  // namespace sonare::mastering::saturation
