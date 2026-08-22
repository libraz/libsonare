#include "mastering/saturation/amp_sim.h"

#include <algorithm>
#include <cmath>

#include "core/resample.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/saturation/amp_physics.h"
#include "mastering/saturation/triode.h"
#include "rt/fractional_delay.h"
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
constexpr AmpVoicing kAmpVoxChime{3000.0f, 2.0f, 6.0f, -12.0f, 40.0f, 90.0f, 700.0f, 3400.0f};
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

// Circuit voicing: the constants of the kCircuit head. Separate from AmpVoicing
// so the voiced head's numbers stay exactly where they were.
struct CircuitVoicing {
  float coupling_hz;     // interstage coupling-cap corner: the "tight" control
  float cathode_hz;      // cathode-bypass shelf corner
  float cathode_db;      // gain lost below it (negative: local feedback)
  float bias_v;          // grid bias operating point
  ToneStackModel stack;  // which set of ladder component values
};

// A larger coupling corner is what makes a high-gain circuit tight rather than
// flubby: it keeps the bass out of the next stage's clip. The clean voicings sit
// near 20-25 Hz (essentially DC-coupled for audio), the modern ones well into
// the low mids.
constexpr CircuitVoicing kCircuitClassicCrunch{60.0f, 250.0f, -4.0f, -1.6f,
                                               ToneStackModel::kBritish};
constexpr CircuitVoicing kCircuitFenderClean{25.0f, 120.0f, -2.0f, -1.9f,
                                             ToneStackModel::kAmerican};
constexpr CircuitVoicing kCircuitModernHiGain{160.0f, 400.0f, -6.0f, -1.4f,
                                              ToneStackModel::kBritish};
constexpr CircuitVoicing kCircuitTweed{20.0f, 100.0f, -2.0f, -2.0f, ToneStackModel::kAmerican};
constexpr CircuitVoicing kCircuitVoxChime{40.0f, 200.0f, -3.0f, -1.7f, ToneStackModel::kBritish};
constexpr CircuitVoicing kCircuitRectifier{140.0f, 350.0f, -6.0f, -1.3f, ToneStackModel::kBritish};

CircuitVoicing circuit_voicing(AmpModel model) noexcept {
  switch (model) {
    case AmpModel::kFenderClean:
      return kCircuitFenderClean;
    case AmpModel::kModernHiGain:
      return kCircuitModernHiGain;
    case AmpModel::kTweed:
      return kCircuitTweed;
    case AmpModel::kVoxChime:
      return kCircuitVoxChime;
    case AmpModel::kRectifier:
      return kCircuitRectifier;
    default:
      return kCircuitClassicCrunch;
  }
}

/// Turns the extracted low band into a normalized cone excursion, where 1.0 is
/// a driver at its working travel. The proxy is tapped at the amp's own internal
/// level, which is far below unity — a 60 Hz tone lands around 0.02 at a musical
/// drive setting — so both cone stages scale it up first and work in this
/// normalized domain, the same way the power and transformer stages do. Without
/// it an absolute threshold would gut the low band at one drive setting and do
/// nothing at another.
constexpr float kConeExcursionGain = 60.0f;

/// Asymmetric-compliance coefficient at full `cone`: the weight of the squared
/// term in the suspension curve, i.e. how much stiffer the cone is travelling
/// one way than the other.
constexpr float kConeAsymmetry = 1.5f;

/// Widest grid-bias offset (V) a fully conducting grid produces at
/// `bias_shift == 1`. The triode law's grid current runs from about 2e-5 to
/// 6e-4 over the conduction region, so this is the scale that turns it into the
/// volt-or-two shift a real coupling cap charges to.
constexpr float kBiasShiftScaleV = 2000.0f;

/// Time constants of the grid-current tracker. These are deliberately
/// ASYMMETRIC, and the asymmetry is the whole effect rather than a refinement:
/// the coupling cap charges through the conducting grid, which is a low
/// resistance (22 nF into about a kilohm), and discharges through the grid leak,
/// which is three decades higher (22 nF into 470 k). A single symmetric time
/// constant tracks the MEAN grid current instead of its peak, and the loop then
/// self-limits at a shift of a fraction of a volt — measured at 0.15-0.27 V
/// against a 1.6 V bias, which gates nothing. Charging toward the peak is what
/// lets the cap hold the stage near cutoff after a loud attack, which is what
/// blocking distortion is.
constexpr float kBiasChargeTauS = 2.2e-5f;
constexpr float kBiasDischargeTauS = 0.0103f;

/// Plate-swing headroom of one cascade stage, in normalized grid units.
///
/// The triode law is evaluated open-loop, so its plate current grows without
/// bound as the grid goes positive (a real stage is bounded by its plate load
/// and supply rail, which an open-loop evaluation has no way to know). Left
/// alone a cascade would therefore run away. Bounding each stage's output is
/// both the fix and the physically right thing: the plate cannot swing past the
/// rail. Note the two limits are NOT symmetric and are not meant to be — the
/// negative-going grid runs into the tube's own cutoff, which caps the plate
/// swing the other way at the idle current, and that asymmetry is the triode's
/// character rather than something added on top.
constexpr float kPlateHeadroom = 3.0f;

/// Trim on the circuit head's output. The cascade ends after a tone stack that
/// loses 8-12 dB and a power stage, and every stage downstream (power, sag,
/// transformer, cone) is calibrated against the voiced head's internal level —
/// so the circuit head is measured against that level and trimmed to match,
/// rather than leaving each of those knobs to mean something different per
/// topology.
///
/// Measured, not chosen: the two heads' output ratio holds at 0.029 across the
/// lower two thirds of the drive range (both are then linear, and both scale
/// with the same drive law). Above that the circuit head compresses and the
/// voiced head does not, so they separate — which is the point of the circuit
/// head, not a calibration failure.
constexpr float kCircuitMakeup = 0.029f;

/// Step used to measure a stage's small-signal transconductance at its bias
/// point, so `preamp_gain_db` can mean the literal gain of the cascade rather
/// than an arbitrary scaling of a plate current.
constexpr float kTransconductanceStepV = 0.01f;

/// Crossover dead zone at `crossover == 1`, as a half-width in the power
/// stage's normalized domain, and how much sharper each half's turn-on gets
/// with it. Both are needed: a wide dead zone with a soft knee is only a gentle
/// expander. Measured against the power stage's own working range, so that a
/// driven signal traverses the zone on every cycle (the notch) while a quiet
/// one sits inside it (the squashed, gated small-signal behaviour of a cold
/// amp). The two are coupled: the slope through the origin is knee*sech^2(knee*
/// bias), so it takes a large PRODUCT to actually flatten the crossing, and
/// sharpening the knee alone would raise the small-signal gain rather than
/// lower it.
constexpr float kMaxCrossoverBias = 0.5f;
constexpr float kMaxCrossoverKnee = 5.0f;
/// Backstop on the modelled rail droop. Not a voicing choice: the physical
/// inputs already bound the droop below this, so reaching it means something
/// upstream is out of range.
constexpr float kMaxRailDroop = 0.6f;

/// Maps the tone-knob dB fields onto pot positions under kCircuit: 0 dB centres
/// every control and +-12 dB reaches the extremes.
constexpr float kToneStackDbSpan = 24.0f;

float tone_position(float db) noexcept {
  return std::clamp(0.5f + db / kToneStackDbSpan, 0.0f, 1.0f);
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
  config_.mic_axis = std::clamp(config_.mic_axis, 0.0f, 1.0f);
  config_.mic_b_axis = std::clamp(config_.mic_b_axis, 0.0f, 1.0f);
  config_.mic_blend = std::clamp(config_.mic_blend, 0.0f, 1.0f);
  config_.mic_distance_cm = std::clamp(config_.mic_distance_cm, 0.0f, kMaxMicDistanceCm);
  config_.mic_b_distance_cm = std::clamp(config_.mic_b_distance_cm, 0.0f, kMaxMicDistanceCm);
  config_.cone = std::clamp(config_.cone, 0.0f, 1.0f);
  config_.doppler = std::clamp(config_.doppler, 0.0f, 1.0f);
  config_.crossover = std::clamp(config_.crossover, 0.0f, 1.0f);
  config_.bias_shift = std::clamp(config_.bias_shift, 0.0f, 1.0f);
  config_.preamp_stages = std::clamp(config_.preamp_stages, 1, kMaxPreampStages);
}

namespace {
/// Push-pull class-AB power stage: two antiphase halves summed by the output
/// transformer, gain-compensated so the slope at the origin stays 1 (quiet
/// passages pass unchanged) while hard signals compress toward the rails and
/// pick up odd harmonics — even ones cancel in the pair. @p bias is how far
/// apart the two halves are biased, i.e. the crossover dead zone; at 0 the
/// composite is exactly the symmetric tanh this stage has always been.
/// @p drive_scale is the output-tube class. @p adaa antialiases the composite.
/// The power stage's own gain. Its reciprocal is the stage's output ceiling, so
/// this is also what turns a signal level into a fraction of full output — which
/// is how the supply knows how much current is being drawn. One definition, two
/// call sites: a second copy could drift and would silently decalibrate the sag.
float power_stage_gain(float power, float drive_scale) noexcept {
  return (1.0f + power * 40.0f) * drive_scale;
}

float power_stage(float x, float power, float crossover, float drive_scale,
                  rt::Adaa1<rt::PushPullNonlinearity>& adaa) noexcept {
  // The power amp has gain: the (quiet) preamp output is driven hard into the
  // tubes. g is large so the stage saturates at the amp's low internal level;
  // dividing by g keeps unity slope at the origin (quiet passages pass, loud
  // ones compress toward the rails).
  const float g = power_stage_gain(power, drive_scale);
  adaa.nonlinearity().bias = crossover * kMaxCrossoverBias;
  adaa.nonlinearity().knee = 1.0f + crossover * kMaxCrossoverKnee;
  return adaa.process(g * x) / g;
}
}  // namespace

void AmpSim::validate_config(const AmpSimConfig& config) {
  if (!std::isfinite(config.drive) || !std::isfinite(config.bass_db) ||
      !std::isfinite(config.mid_db) || !std::isfinite(config.treble_db) ||
      !std::isfinite(config.presence_db) || !std::isfinite(config.level_db) ||
      !std::isfinite(config.power) || !std::isfinite(config.sag) ||
      !std::isfinite(config.transformer) || !std::isfinite(config.nfb) ||
      !std::isfinite(config.mic_axis) || !std::isfinite(config.mic_b_axis) ||
      !std::isfinite(config.mic_blend) || !std::isfinite(config.mic_distance_cm) ||
      !std::isfinite(config.mic_b_distance_cm) || !std::isfinite(config.cone) ||
      !std::isfinite(config.doppler) || !std::isfinite(config.crossover) ||
      !std::isfinite(config.bias_shift)) {
    throw SonareException(ErrorCode::InvalidParameter, "amp-sim params must be finite");
  }
}

void AmpSim::prepare(double sample_rate, int max_block_size) {
  if (sample_rate <= 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = std::max(0, max_block_size);
  // Only the head the topology selects is prepared. Both own an oversampler
  // with per-channel streaming state, and preparing the unused one would double
  // this processor's resident memory for nothing. `topology` is not
  // automatable, so the choice cannot be invalidated later.
  if (config_.topology == AmpTopology::kCircuit) {
    allocate_circuit_scratch(static_cast<int>(dynamics::kRealtimePreparedChannels));
    circuit_latency_samples_ = oversampler_.streaming_round_trip_latency_samples();
  } else {
    tube_.prepare(sample_rate, max_block_size);
    circuit_latency_samples_ = 0;
  }
  chains_.assign(dynamics::kRealtimePreparedChannels, ChannelChain{});
  power_adaa_.assign(dynamics::kRealtimePreparedChannels, rt::Adaa1<rt::PushPullNonlinearity>{});
  design_chain();
  allocate_delay_lines();
  // Re-derived from the source, so preparing at a new rate resamples the IR
  // rather than reusing one built for the old rate.
  rebuild_cab_ir();
  allocate_cab_ir_history();
  prepared_ = true;
  reset();
}

void AmpSim::allocate_circuit_scratch(int num_channels) {
  const size_t base = static_cast<size_t>(max_block_size_);
  up_scratch_.assign(base * static_cast<size_t>(kCircuitOversample), 0.0f);
  down_scratch_.assign(base, 0.0f);
  // Sized to the channel count actually in play, not to a fixed cap: process()
  // grows chains_ past the prepared count when a caller hands over more channels
  // than prepare() expected, and the resampler state is indexed by the same
  // channel index, so it has to grow with it.
  oversampler_states_.resize(static_cast<size_t>(std::max(num_channels, 1)));
  for (auto& state : oversampler_states_) {
    oversampler_.prepare_streaming(&state, base);
  }
}

void AmpSim::allocate_cab_ir_history() {
  // A power-of-two ring so the FIR walk masks instead of dividing. Sized to the
  // loaded IR; with no IR the rings stay empty and the analytic cab runs.
  size_t ring = 0;
  if (!cab_ir_.empty()) {
    ring = 1;
    while (ring < cab_ir_.size()) ring <<= 1;
  }
  cab_ir_mask_ = ring > 0 ? ring - 1 : 0;
  for (ChannelChain& chain : chains_) {
    chain.cab_ir_history.assign(ring, 0.0f);
    chain.cab_ir_write = 0;
  }
}

void AmpSim::load_cab_ir(const float* impulse_response, int num_samples, double ir_sample_rate) {
  if (num_samples < 0 || (num_samples > 0 && impulse_response == nullptr)) {
    throw SonareException(ErrorCode::InvalidParameter, "load_cab_ir: invalid IR");
  }
  if (!std::isfinite(ir_sample_rate) || ir_sample_rate < 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, "load_cab_ir: invalid IR sample rate");
  }
  // Validate the whole source before truncation: a non-finite sample past the
  // budget still means the caller handed over a broken IR, and silently keeping
  // the part that happened to fit would hide it.
  for (int i = 0; i < num_samples; ++i) {
    if (!std::isfinite(impulse_response[i])) {
      throw SonareException(ErrorCode::InvalidParameter, "load_cab_ir: IR must be finite");
    }
  }
  cab_ir_source_.assign(impulse_response, impulse_response + num_samples);
  cab_ir_source_rate_ = ir_sample_rate;
  // A captured IR replaces a generated one: two cabinets in one convolution is
  // not a thing a caller can have meant.
  cab_ir_generated_ = false;
  // Safe before prepare(): with no rate yet there is nothing to resample against
  // or truncate to, and prepare() rebuilds from the source once there is.
  if (prepared_) {
    rebuild_cab_ir();
    allocate_cab_ir_history();
  } else {
    cab_ir_.clear();
    cab_ir_mask_ = 0;
  }
}

void AmpSim::load_cab_ir(const std::vector<float>& impulse_response, double ir_sample_rate) {
  load_cab_ir(impulse_response.empty() ? nullptr : impulse_response.data(),
              static_cast<int>(impulse_response.size()), ir_sample_rate);
}

void AmpSim::load_generated_cab_ir(const CabIrSpec& spec) {
  cab_ir_spec_ = spec;
  cab_ir_generated_ = true;
  cab_ir_source_.clear();
  cab_ir_source_rate_ = 0.0;
  if (prepared_) {
    rebuild_cab_ir();
    allocate_cab_ir_history();
  } else {
    cab_ir_.clear();
    cab_ir_mask_ = 0;
  }
}

void AmpSim::rebuild_cab_ir() {
  cab_ir_.clear();
  if (!(sample_rate_ > 0.0)) return;
  if (cab_ir_generated_) {
    // Generated at the processor's own rate, so this is the one IR path with no
    // resampling and no truncation: the generator already works to the budget.
    cab_ir_ = generate_cab_ir(cab_ir_spec_, sample_rate_);
    return;
  }
  if (cab_ir_source_.empty()) return;

  const int target_rate = static_cast<int>(std::lround(sample_rate_));
  const int source_rate =
      cab_ir_source_rate_ > 0.0 ? static_cast<int>(std::lround(cab_ir_source_rate_)) : target_rate;
  if (source_rate != target_rate) {
    // One-shot r8brain, the same conversion every other rate change in the
    // library goes through. Control thread only, which this is.
    cab_ir_ = resample(cab_ir_source_.data(), cab_ir_source_.size(), source_rate, target_rate);
  } else {
    cab_ir_ = cab_ir_source_;
  }

  // Truncate to the duration budget, not to a fixed count — see kMaxCabIrMs.
  const int budget =
      std::min(kMaxCabIrSamples,
               std::max(1, static_cast<int>(std::lround(kMaxCabIrMs * 0.001 * sample_rate_))));
  if (static_cast<int>(cab_ir_.size()) > budget) {
    cab_ir_.resize(static_cast<size_t>(budget));
  }
  // The resampler can ring slightly past the input's own range; a non-finite
  // sample here would reach the audio thread, so drop back to the analytic cab
  // rather than convolving with it.
  for (float v : cab_ir_) {
    if (!std::isfinite(v)) {
      cab_ir_.clear();
      return;
    }
  }
}

void AmpSim::ChannelChain::clear() noexcept {
  pre = {};
  bass = {};
  mid = {};
  treble = {};
  cab_a = {};
  cab_b = {};
  sag_env = 0.0f;
  xf_lp = 0.0f;
  nfb_shape = {};
  nfb_fb = 0.0f;
  cone_lp = 0.0f;
  cone_dc = 0.0f;
  for (PreampStage& stage : stages) {
    stage.hp_x1 = 0.0f;
    stage.hp_y1 = 0.0f;
    stage.cathode = {};
    stage.grid_env = 0.0f;
  }
  stack.clear();
  // The lines keep their capacity: reset() must not allocate.
  std::fill(cab_ir_history.begin(), cab_ir_history.end(), 0.0f);
  cab_ir_write = 0;
  std::fill(doppler_line.begin(), doppler_line.end(), 0.0f);
  doppler_write = 0;
  std::fill(mic_a_line.begin(), mic_a_line.end(), 0.0f);
  mic_a_write = 0;
  std::fill(mic_b_line.begin(), mic_b_line.end(), 0.0f);
  mic_b_write = 0;
}

void AmpSim::allocate_delay_lines() {
  // The Doppler line spans the full modulation swing plus the Lagrange stencil's
  // two-sample lookahead; the mic lines are sized from the configured path-length
  // difference, which cannot grow later because neither distance is automatable.
  const size_t doppler_size =
      config_.doppler > 0.0f ? static_cast<size_t>(2 * kDopplerBaseSamples + 8) : 0;
  const size_t mic_size = mic_line_capacity_ > 0 ? static_cast<size_t>(mic_line_capacity_) : 0;
  for (ChannelChain& chain : chains_) {
    chain.doppler_line.assign(doppler_size, 0.0f);
    chain.doppler_write = 0;
    chain.mic_a_line.assign(mic_a_delay_q8_ > 0 ? mic_size : 0, 0.0f);
    chain.mic_a_write = 0;
    chain.mic_b_line.assign(mic_b_delay_q8_ > 0 ? mic_size : 0, 0.0f);
    chain.mic_b_write = 0;
  }
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
  // One cabinet design per mic (see cab_voicing.h — the same tables the cabinet
  // IR generator reads). Without a capsule the three mic biquads stay unused and
  // the roll-off keeps the cab's own corner, which is what makes the default
  // path bit-identical to the original cab-only chain.
  cab_a_c_ = design_cab_stage(config_.cab_model, config_.mic_model, config_.mic_axis,
                              config_.mic_distance_cm, config_.presence_db, sample_rate_);
  cab_b_c_ = design_cab_stage(config_.cab_model, config_.mic_b_model, config_.mic_b_axis,
                              config_.mic_b_distance_cm, config_.presence_db, sample_rate_);
  // Mic pair: only the path-length DIFFERENCE is applied, on whichever mic is
  // farther, so the near mic stays at zero latency and the two comb exactly as a
  // real pair does. A single mic is left undelayed entirely (see mic_distance_cm).
  //
  // The taps and the line capacity come from the DISTANCES, not from mic_blend:
  // the blend is realtime-automatable, so deriving the capacity from it would let
  // a live automation move open the second mic onto a line that prepare() never
  // allocated. The distances cannot move at all, so this is stable. Only the
  // reported tail follows the blend, since an unused pair has no tail.
  mic_a_delay_q8_ = 0;
  mic_b_delay_q8_ = 0;
  mic_line_capacity_ = 0;
  mic_tail_samples_ = 0;
  if (config_.cab) {
    const float difference_cm = std::fabs(config_.mic_b_distance_cm - config_.mic_distance_cm);
    const float delay_samples =
        difference_cm / kSoundSpeedCmPerS * static_cast<float>(sample_rate_);
    const int delay_q8 = static_cast<int>(std::lround(delay_samples * 256.0f));
    if (delay_q8 > 0) {
      (config_.mic_b_distance_cm > config_.mic_distance_cm ? mic_b_delay_q8_ : mic_a_delay_q8_) =
          delay_q8;
      // Capacity covers the whole delay plus the Lagrange stencil's lookahead.
      mic_line_capacity_ = static_cast<int>(std::ceil(delay_samples)) + 8;
      if (config_.mic_blend > 0.0f) {
        mic_tail_samples_ = static_cast<int>(std::ceil(delay_samples));
      }
    }
  }
  level_gain_ = sonare::db_to_linear(config_.level_db);
  power_drive_scale_ = power_tube_scale(config_.power_tube);

  // Circuit head. Everything here is designed at the OVERSAMPLED rate, because
  // that is the rate the cascade, the ladder and the power stage all run at.
  if (config_.topology == AmpTopology::kCircuit) {
    const CircuitVoicing cv = circuit_voicing(config_.amp_model);
    const double os_rate = sample_rate_ * kCircuitOversample;
    active_stages_ = std::clamp(config_.preamp_stages, 1, kMaxPreampStages);
    stage_bias_v_ = cv.bias_v;
    // The cascade works in normalized grid units where 1.0 swings the grid from
    // its bias exactly up to the conduction threshold, so "the signal reached
    // 1.0" means the same thing whatever bias a voicing picks.
    stage_volts_per_unit_ = -cv.bias_v;
    stage_idle_ma_ = triode::plate_current_ma(stage_bias_v_, triode::kPlateVoltageV);
    // Measure the stage's transconductance at its own bias point rather than
    // assuming one, so preamp_gain_db is the literal small-signal gain of the
    // cascade for any bias the voicings pick.
    const float gm =
        (triode::plate_current_ma(stage_bias_v_ + kTransconductanceStepV, triode::kPlateVoltageV) -
         triode::plate_current_ma(stage_bias_v_ - kTransconductanceStepV, triode::kPlateVoltageV)) /
        (2.0f * kTransconductanceStepV);
    // The drive knob IS the cascade's total small-signal gain, on the same
    // voicing-specific dB law the voiced head's single stage uses. It is then
    // split evenly across the stages, so the stage count redistributes clipping
    // rather than piling gain on gain. (Only the TRIODE gain is held fixed: each
    // stage also brings a coupling cap and a cathode shelf, and those accumulate
    // — see preamp_stages.) Applying the drive as a gain rather than as an extra
    // input trim is
    // also what keeps the head's small-signal gain proportional to drive exactly
    // as the voiced head's is, which is why the makeup trim below can be a
    // single drive-independent constant.
    const float total_gain =
        sonare::db_to_linear(drive_to_db(config_.drive, amp_voicing(config_.amp_model)));
    const float per_stage = std::pow(total_gain, 1.0f / static_cast<float>(active_stages_));
    const float denom = gm * stage_volts_per_unit_;
    stage_scale_ = std::abs(denom) > 0.0f ? per_stage / denom : 0.0f;
    // Interstage coupling cap, as the single RC it physically is.
    coupling_alpha_ = std::clamp(
        1.0f / (1.0f + kTwoPi * cv.coupling_hz / static_cast<float>(os_rate)), 0.0f, 1.0f);
    cathode_c_ = rt::rbj_low_shelf(rt::frequency_to_w0(cv.cathode_hz, os_rate), kButterworthQ,
                                   cv.cathode_db);
    grid_charge_alpha_ = std::clamp(
        1.0f - std::exp(-1.0f / (kBiasChargeTauS * static_cast<float>(os_rate))), 0.0f, 1.0f);
    grid_discharge_alpha_ = std::clamp(
        1.0f - std::exp(-1.0f / (kBiasDischargeTauS * static_cast<float>(os_rate))), 0.0f, 1.0f);
    // The bass pot is logarithmic on the real circuit; squaring the position is
    // the usual audio-taper approximation.
    const float bass_pos = tone_position(config_.bass_db);
    stack_c_ = design_tone_stack(tone_stack_components(cv.stack), os_rate,
                                 tone_position(config_.treble_db), tone_position(config_.mid_db),
                                 static_cast<double>(bass_pos) * bass_pos);
    circuit_makeup_ = kCircuitMakeup;
  }

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
  // Designed at whichever rate the power stage runs at: base rate under
  // kVoiced, the oversampled rate under kCircuit (where the loop lives inside
  // the oversampling region, so its one-sample delay is a quarter sample at base
  // rate — closer to a real feedback loop than the base-rate version).
  constexpr float kNfbCentreHz = 800.0f;
  constexpr float kNfbQ = 0.5f;
  const double power_rate =
      config_.topology == AmpTopology::kCircuit ? sample_rate_ * kCircuitOversample : sample_rate_;
  nfb_shape_c_ = rt::rbj_bandpass(rt::frequency_to_w0(kNfbCentreHz, power_rate), kNfbQ);
  // Cone-excursion extractor: a ~90 Hz one-pole corner, so the proxy follows the
  // band that actually moves the cone.
  constexpr float kConeCornerHz = 90.0f;
  cone_alpha_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kConeCornerHz / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
  // Offset tracker for the cone's rectified term: slow enough to leave the
  // lowest musical fundamentals alone.
  constexpr float kConeDcCornerHz = 5.0f;
  cone_dc_alpha_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kConeDcCornerHz / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
  // Doppler: the modulation swings over the full base delay in each direction,
  // so 1.0 reaches +-kDopplerBaseSamples.
  doppler_mod_q8_ = static_cast<int>(std::lround(config_.doppler * kDopplerBaseSamples * 256.0f));
}

float AmpSim::process_cab(float x, CabStage& stage, const CabDesign& design) noexcept {
  float s = process_chain(x, stage.hp, design.hp);
  s = process_chain(s, stage.bump, design.bump);
  s = process_chain(s, stage.presence, design.presence);
  if (design.mic) {
    s = process_chain(s, stage.mic_prox, design.mic_prox);
    s = process_chain(s, stage.mic_presence, design.mic_presence);
    s = process_chain(s, stage.mic_top, design.mic_top);
  }
  s = process_chain(s, stage.lp1, design.lp1);
  s = process_chain(s, stage.lp2, design.lp2);
  return s;
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
    power_adaa_.assign(static_cast<size_t>(num_channels), rt::Adaa1<rt::PushPullNonlinearity>{});
    // The fresh chains carry no delay lines or IR history; without this the
    // mic/Doppler/cab-IR stages would read an empty buffer on the added
    // channels.
    allocate_delay_lines();
    allocate_cab_ir_history();
    if (config_.topology == AmpTopology::kCircuit) {
      allocate_circuit_scratch(num_channels);
    }
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }

  if (config_.topology == AmpTopology::kCircuit) {
    process_circuit_head(channels, num_channels, num_samples);
  } else {
    process_voiced_head(channels, num_channels, num_samples);
  }
  process_tail(channels, num_channels, num_samples);
}

float AmpSim::run_power_stage(float s, ChannelChain& chain, size_t channel) noexcept {
  // Off when power == 0 -> the ADAA state is untouched and the path is
  // bit-identical to a preamp-only amp.
  if (config_.power <= 0.0f) return s;
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
    s = power_stage(e, config_.power, config_.crossover, power_drive_scale_, power_adaa_[channel]);
    chain.nfb_fb = s;
    return s;
  }
  return power_stage(s, config_.power, config_.crossover, power_drive_scale_, power_adaa_[channel]);
}

void AmpSim::process_voiced_head(float* const* channels, int num_channels, int num_samples) {
  // Pre-emphasis in front of the (oversampled, block-based) tube stage.
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelChain& chain = chains_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_chain(channels[ch][i], chain.pre, pre_c_);
    }
  }
  tube_.process(channels, num_channels, num_samples);
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelChain& chain = chains_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      float s = channels[ch][i];
      s = process_chain(s, chain.bass, bass_c_);
      s = process_chain(s, chain.mid, mid_c_);
      s = process_chain(s, chain.treble, treble_c_);
      channels[ch][i] = run_power_stage(s, chain, static_cast<size_t>(ch));
    }
  }
}

void AmpSim::process_circuit_head(float* const* channels, int num_channels, int num_samples) {
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(kCircuitOversample);
  if (os_samples > up_scratch_.size() || static_cast<size_t>(num_samples) > down_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared AmpSim oversampling scratch");
  }
  constexpr float kInvPlateHeadroom = 1.0f / kPlateHeadroom;
  const float bias_shift = config_.bias_shift;
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelChain& chain = chains_[static_cast<size_t>(ch)];
    auto& state = oversampler_states_[static_cast<size_t>(ch)];
    oversampler_.upsample_to_streaming(channels[ch], static_cast<size_t>(num_samples),
                                       up_scratch_.data(), up_scratch_.size(), &state);
    for (size_t i = 0; i < os_samples; ++i) {
      float v = up_scratch_[i];
      for (int st = 0; st < active_stages_; ++st) {
        PreampStage& stage = chain.stages[static_cast<size_t>(st)];
        // Interstage coupling cap, as the single RC it is. Its corner is what
        // keeps the bass out of the next stage's clip, i.e. "tight" vs "flubby".
        const float hp = coupling_alpha_ * (stage.hp_y1 + v - stage.hp_x1);
        stage.hp_x1 = v;
        stage.hp_y1 = hp;
        v = hp;
        float vg = hp * stage_volts_per_unit_;
        if (bias_shift > 0.0f) {
          // Blocking distortion: the charge the grid drew on earlier samples is
          // still on the coupling cap, holding this stage more negative.
          vg -= bias_shift * stage.grid_env * kBiasShiftScaleV;
        }
        const float vg_total = stage_bias_v_ + vg;
        const float delta =
            triode::plate_current_ma(vg_total, triode::kPlateVoltageV) - stage_idle_ma_;
        // Inverting: plate current up means plate voltage down. This is not
        // cosmetic — it is why the next stage clips the half this one left
        // alone, which is the mechanism a single stage cannot reproduce.
        const float u = -delta * stage_scale_;
        v = kPlateHeadroom * std::tanh(u * kInvPlateHeadroom);
        if (bias_shift > 0.0f) {
          const float ig = triode::grid_current_ma(vg_total);
          const float alpha = ig > stage.grid_env ? grid_charge_alpha_ : grid_discharge_alpha_;
          stage.grid_env += alpha * (ig - stage.grid_env);
        }
        v = process_chain(v, stage.cathode, cathode_c_);
      }
      // The passive ladder, then the trim that lands the head at the voiced
      // head's internal level so every downstream knob keeps its calibration.
      v = chain.stack.process(v, stack_c_) * circuit_makeup_;
      up_scratch_[i] = run_power_stage(v, chain, static_cast<size_t>(ch));
    }
    oversampler_.downsample_to_streaming(up_scratch_.data(), os_samples, down_scratch_.data(),
                                         down_scratch_.size(), &state);
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = down_scratch_[static_cast<size_t>(i)];
    }
  }
}

void AmpSim::process_tail(float* const* channels, int num_channels, int num_samples) {
  const bool use_ir = config_.cab && !cab_ir_.empty();
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelChain& chain = chains_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      float s = channels[ch][i];
      // Power-supply sag: a lagged envelope of the draw pulls the rail down, so
      // the attack punches through and the sustain compresses. Off -> the
      // envelope is untouched and the path is bit-identical.
      if (config_.sag > 0.0f && config_.power > 0.0f) {
        chain.sag_env += sag_alpha_ * (std::fabs(s) - chain.sag_env);
        // Ohm's law on the supply: the rail falls by (draw x internal
        // resistance), so `sag` is the fractional droop AT FULL OUTPUT and the
        // draw scales it. The power stage saturates at 1/g, so multiplying the
        // envelope by g expresses the level as a fraction of full output — which
        // is why this is calibrated rather than a sensitivity constant chosen to
        // taste. kMaxRailDroop is a backstop: sag <= 1 and draw <= 1 already
        // bound the product, so a physical setting never reaches it.
        const float draw = chain.sag_env * power_stage_gain(config_.power, power_drive_scale_);
        const float droop = std::min(kMaxRailDroop, config_.sag * draw);
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
      // Speaker cone. The excursion proxy is the low band the driver is actually
      // moving with; both stages are skipped when off, leaving the state
      // untouched and the path bit-identical.
      if (config_.cone > 0.0f || config_.doppler > 0.0f) {
        chain.cone_lp += cone_alpha_ * (s - chain.cone_lp);
      }
      if (config_.cone > 0.0f) {
        // Suspension nonlinearity, replacing the low band it came from. The
        // squared term is the asymmetric compliance — a spider is stiffer one way
        // than the other, and that asymmetry is what yields the even harmonics an
        // amp stage cannot. It vanishes with the excursion, so a cone that is
        // barely moving stays linear; the tanh is the travel limit beyond it.
        const float excursion = chain.cone_lp * kConeExcursionGain;
        const float asymmetry = config_.cone * kConeAsymmetry;
        const float compliant =
            std::tanh(excursion + asymmetry * excursion * excursion) / kConeExcursionGain;
        // A squared term also rectifies, so it carries a slow offset — real for
        // the cone's rest position, not something a cab radiates. Subtracting the
        // tracked offset keeps it out of the audio without touching the harmonics.
        const float correction = compliant - chain.cone_lp;
        chain.cone_dc += cone_dc_alpha_ * (correction - chain.cone_dc);
        s += config_.cone * (correction - chain.cone_dc);
      }
      if (config_.doppler > 0.0f) {
        // The cone radiating the treble is the cone the bass is moving, so the
        // path length to the mic wobbles with the excursion. Clamped, so a hotter
        // signal pins the travel at its limit rather than running away with it.
        const float excursion = std::clamp(chain.cone_lp * kConeExcursionGain, -1.0f, 1.0f);
        const int delay_q8 =
            kDopplerBaseSamples * 256 + static_cast<int>(std::lround(doppler_mod_q8_ * excursion));
        s = rt::lagrange3_fractional_delay(chain.doppler_line.data(), chain.doppler_line.size(),
                                           chain.doppler_write, delay_q8, s);
      }
      if (use_ir) {
        // A measured cab IR already contains the cabinet, the microphone and
        // where it was placed, so it replaces the whole analytic chain rather
        // than sitting alongside it — otherwise the render would carry two
        // microphones in front of one speaker. Direct FIR: zero latency, which
        // for a guitar amp is worth more than tail length.
        chain.cab_ir_history[chain.cab_ir_write] = s;
        float acc = 0.0f;
        size_t idx = chain.cab_ir_write;
        for (size_t k = 0; k < cab_ir_.size(); ++k) {
          acc += cab_ir_[k] * chain.cab_ir_history[idx];
          idx = (idx + cab_ir_mask_) & cab_ir_mask_;
        }
        chain.cab_ir_write = (chain.cab_ir_write + 1) & cab_ir_mask_;
        s = acc;
      } else if (config_.cab) {
        if (config_.mic_blend > 0.0f) {
          // Two mics on the same cab. The delay is applied AFTER the filters,
          // which is equivalent (both stages are LTI) and costs one delay line
          // per mic instead of two.
          float a = process_cab(s, chain.cab_a, cab_a_c_);
          float b = process_cab(s, chain.cab_b, cab_b_c_);
          if (mic_a_delay_q8_ > 0) {
            a = rt::lagrange3_fractional_delay(chain.mic_a_line.data(), chain.mic_a_line.size(),
                                               chain.mic_a_write, mic_a_delay_q8_, a);
          }
          if (mic_b_delay_q8_ > 0) {
            b = rt::lagrange3_fractional_delay(chain.mic_b_line.data(), chain.mic_b_line.size(),
                                               chain.mic_b_write, mic_b_delay_q8_, b);
          }
          if (config_.mic_b_invert) b = -b;
          s = (1.0f - config_.mic_blend) * a + config_.mic_blend * b;
        } else {
          s = process_cab(s, chain.cab_a, cab_a_c_);
        }
      }
      channels[ch][i] = s * level_gain_;
    }
  }
}

void AmpSim::reset() {
  tube_.reset();
  for (ChannelChain& chain : chains_) chain.clear();
  for (auto& adaa : power_adaa_) adaa.reset();
  // Keeps the scratch and the resampler histories, so reset() never allocates.
  std::fill(up_scratch_.begin(), up_scratch_.end(), 0.0f);
  std::fill(down_scratch_.begin(), down_scratch_.end(), 0.0f);
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
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
    case 10:
      config_.mic_axis = std::clamp(value, 0.0f, 1.0f);
      break;
    case 11:
      config_.mic_b_axis = std::clamp(value, 0.0f, 1.0f);
      break;
    case 12:
      // Only the blend gain moves; the mic delays are fixed at construction, so
      // design_chain() cannot resize a delay line from the audio thread.
      config_.mic_blend = std::clamp(value, 0.0f, 1.0f);
      break;
    case 13:
      config_.cone = std::clamp(value, 0.0f, 1.0f);
      break;
    case 14:
      config_.crossover = std::clamp(value, 0.0f, 1.0f);
      break;
    case 15:
      config_.bias_shift = std::clamp(value, 0.0f, 1.0f);
      break;
    default:
      return false;
  }
  design_chain();
  return true;
}

std::vector<rt::ParamDescriptor> AmpSim::parameter_descriptors() const {
  return {{"drive", 0},       {"bassDb", 1},  {"midDb", 2},      {"trebleDb", 3},
          {"presenceDb", 4},  {"levelDb", 5}, {"power", 6},      {"sag", 7},
          {"transformer", 8}, {"nfb", 9},     {"micAxis", 10},   {"micBAxis", 11},
          {"micBlend", 12},   {"cone", 13},   {"crossover", 14}, {"biasShift", 15}};
}

}  // namespace sonare::mastering::saturation
