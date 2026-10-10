/// @file chain_params.cpp
/// @brief Flat-parameter bridge for the high-level mastering chain.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/param_field_tables.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::api {

// Coverage guard for the two chain-only tables, whose fields sit directly on a
// chain stage struct rather than on a processor config. Asserted here because
// this is where those structs are visible; the processor tables are asserted in
// processor_params.h. Both stages carry one field the table does not: `enabled`,
// which the chain surface reads as its own `<stage>.enabled` key so a stage can
// be switched off without touching its parameters.
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_EQ_TILT, TiltStage, 1);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_LOUDNESS, LoudnessStage, 1);

namespace {

// ---------------------------------------------------------------------------
// Flat-params helpers
// ---------------------------------------------------------------------------

struct StageFlags {
  bool any_key_seen = false;
  bool enabled_explicit = false;
  bool enabled_value = false;
};

void mark_field(StageFlags& flags) { flags.any_key_seen = true; }

void mark_enabled(StageFlags& flags, double value) {
  flags.any_key_seen = true;
  flags.enabled_explicit = true;
  flags.enabled_value = value != 0.0;
}

bool resolve_enabled(const StageFlags& flags) {
  if (flags.enabled_explicit) {
    return flags.enabled_value;
  }
  return flags.any_key_seen;
}

// Color stages (tape, exciter) must not auto-engage on the mere presence of a
// field: an explicit `enabled` still wins, but otherwise the stage engages only
// when its parameters would actually impart coloration. `meaningful` carries
// that per-stage predicate (tape_engages_color / exciter_engages_color).
bool resolve_color_enabled(const StageFlags& flags, bool meaningful) {
  if (flags.enabled_explicit) {
    return flags.enabled_value;
  }
  return flags.any_key_seen && meaningful;
}
// ---------------------------------------------------------------------------
// Per-key dispatch shared by parse_chain_config_params and
// apply_chain_config_overrides.
// ---------------------------------------------------------------------------

struct StageFlagsSet {
  StageFlags declick;
  StageFlags declip;
  StageFlags decrackle;
  StageFlags dehum;
  StageFlags dereverb;
  StageFlags denoise;
  StageFlags tilt;
  StageFlags deesser;
  StageFlags transient_shaper;
  StageFlags compressor;
  StageFlags multiband_comp;
  StageFlags tape;
  StageFlags exciter;
  StageFlags air_band;
  StageFlags imager;
  StageFlags mono_maker;
  StageFlags true_peak;
  StageFlags loudness;
};

// ---------------------------------------------------------------------------
// Multiband vocabulary. Indices are plain decimal with no leading zero, so one
// field has exactly one canonical spelling; anything else is an unknown key.
// ---------------------------------------------------------------------------

constexpr std::string_view kMultibandPrefix = "dynamics.multibandComp.";
constexpr std::string_view kCutoffListPrefix = "crossover.cutoffsHz.";
// Beyond any list length the chain accepts; a longer index saturates here and is refused as out
// of range rather than wrapping.
constexpr std::uint64_t kIndexSaturation = 1'000'000'000u;

bool parse_index(std::string_view digits, std::size_t* index) {
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
  std::uint64_t value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
    value = std::min(value * 10u + static_cast<std::uint64_t>(c - '0'), kIndexSaturation);
  }
  *index = static_cast<std::size_t>(value);
  return true;
}

// `<head><i><tail>`, e.g. `cutoff3Hz`.
bool parse_indexed(std::string_view text, std::string_view head, std::string_view tail,
                   std::size_t* index) {
  if (text.size() <= head.size() + tail.size() || text.substr(0, head.size()) != head ||
      text.substr(text.size() - tail.size()) != tail) {
    return false;
  }
  return parse_index(text.substr(head.size(), text.size() - head.size() - tail.size()), index);
}

// `<head><i>.<field>`, e.g. `band3.ratio`.
bool parse_band_field(std::string_view text, std::string_view head, std::size_t* index,
                      std::string_view* field) {
  if (text.substr(0, head.size()) != head) return false;
  const std::size_t dot = text.find('.', head.size());
  if (dot == std::string_view::npos || dot + 1 >= text.size()) return false;
  if (!parse_index(text.substr(head.size(), dot - head.size()), index)) return false;
  *field = text.substr(dot + 1);
  return true;
}

// The part of @p key after the multiband prefix, or false when @p key is not a multiband key.
bool multiband_rest(std::string_view key, std::string_view* rest) {
  if (key.substr(0, kMultibandPrefix.size()) != kMultibandPrefix) return false;
  *rest = key.substr(kMultibandPrefix.size());
  return true;
}

bool is_cutoff_list_key(std::string_view canonical, std::size_t* index) {
  std::string_view rest;
  return multiband_rest(canonical, &rest) &&
         rest.substr(0, kCutoffListPrefix.size()) == kCutoffListPrefix &&
         parse_index(rest.substr(kCutoffListPrefix.size()), index);
}

bool is_scalar_cutoff_key(std::string_view canonical) {
  std::string_view rest;
  std::size_t index = 0;
  return multiband_rest(canonical, &rest) && parse_indexed(rest, "cutoff", "Hz", &index);
}

// Rewrites the multiband array spellings onto the scalar ones; false when nothing changes.
bool canonical_multiband_key(std::string_view key, std::string* storage) {
  std::string_view rest;
  if (!multiband_rest(key, &rest)) return false;
  constexpr std::string_view kCrossoverPrefix = "crossover.";
  if (rest.substr(0, kCrossoverPrefix.size()) == kCrossoverPrefix) {
    const std::string_view leaf = rest.substr(kCrossoverPrefix.size());
    if (leaf == "slope" || leaf == "mode" || leaf == "firKernelSize") {
      *storage = std::string(kMultibandPrefix) + std::string(leaf);
      return true;
    }
  }
  std::size_t index = 0;
  std::string_view field;
  if (parse_band_field(rest, "bands.", &index, &field)) {
    *storage =
        std::string(kMultibandPrefix) + "band" + std::to_string(index) + "." + std::string(field);
    return true;
  }
  return false;
}

}  // namespace

const std::string& canonical_chain_param_key(const std::string& key, std::string* storage) {
  const char* canonical = nullptr;
  if (key == "repair.denoise") canonical = "repair.denoise.enabled";
  if (key == "repair.nFft") canonical = "repair.denoise.nFft";
  if (key == "repair.hopLength") canonical = "repair.denoise.hopLength";
  if (key == "repair.ddAlpha") canonical = "repair.denoise.ddAlpha";
  if (key == "repair.reductionDb") canonical = "repair.denoise.reductionDb";
  if (key == "repair.gainFloor") canonical = "repair.denoise.gainFloor";
  if (key == "eq.tiltDb") canonical = "eq.tilt.tiltDb";
  if (key == "eq.pivotHz") canonical = "eq.tilt.pivotHz";
  // The multiband shorthand: the default split's two cutoffs and bands 0..2.
  if (key == "dynamics.multibandComp.lowCutoffHz") canonical = "dynamics.multibandComp.cutoff0Hz";
  if (key == "dynamics.multibandComp.highCutoffHz") canonical = "dynamics.multibandComp.cutoff1Hz";
  if (key == "dynamics.multibandComp.lowThresholdDb") {
    canonical = "dynamics.multibandComp.band0.thresholdDb";
  }
  if (key == "dynamics.multibandComp.lowRatio") canonical = "dynamics.multibandComp.band0.ratio";
  if (key == "dynamics.multibandComp.lowAttackMs") {
    canonical = "dynamics.multibandComp.band0.attackMs";
  }
  if (key == "dynamics.multibandComp.lowReleaseMs") {
    canonical = "dynamics.multibandComp.band0.releaseMs";
  }
  if (key == "dynamics.multibandComp.midThresholdDb") {
    canonical = "dynamics.multibandComp.band1.thresholdDb";
  }
  if (key == "dynamics.multibandComp.midRatio") canonical = "dynamics.multibandComp.band1.ratio";
  if (key == "dynamics.multibandComp.midAttackMs") {
    canonical = "dynamics.multibandComp.band1.attackMs";
  }
  if (key == "dynamics.multibandComp.midReleaseMs") {
    canonical = "dynamics.multibandComp.band1.releaseMs";
  }
  if (key == "dynamics.multibandComp.highThresholdDb") {
    canonical = "dynamics.multibandComp.band2.thresholdDb";
  }
  if (key == "dynamics.multibandComp.highRatio") canonical = "dynamics.multibandComp.band2.ratio";
  if (key == "dynamics.multibandComp.highAttackMs") {
    canonical = "dynamics.multibandComp.band2.attackMs";
  }
  if (key == "dynamics.multibandComp.highReleaseMs") {
    canonical = "dynamics.multibandComp.band2.releaseMs";
  }
  if (canonical != nullptr) {
    *storage = canonical;
    return *storage;
  }
  if (canonical_multiband_key(key, storage)) return *storage;
  return key;
}

std::optional<double> multiband_parameter_value(const MultibandCompStage& stage,
                                                const std::string& canonical_key) {
  std::string_view rest;
  if (!multiband_rest(canonical_key, &rest)) return std::nullopt;
  const auto& config = stage.config;
  if (rest == "enabled") return detail::field_as_double(stage.enabled);
  if (rest == "slope") return detail::field_as_double(config.crossover.slope);
  if (rest == "mode") return detail::field_as_double(config.crossover.mode);
  if (rest == "firKernelSize") return detail::field_as_double(config.crossover.fir_kernel_size);
  std::size_t index = 0;
  if (parse_indexed(rest, "cutoff", "Hz", &index) || is_cutoff_list_key(canonical_key, &index)) {
    if (index >= config.crossover.cutoffs_hz.size()) return std::nullopt;
    return detail::field_as_double(config.crossover.cutoffs_hz[index]);
  }
  std::string_view field;
  if (!parse_band_field(rest, "band", &index, &field) || index >= config.bands.size()) {
    return std::nullopt;
  }
  const auto& band = config.bands[index];
#define X(jkey, member, meta) \
  if (field == jkey) return detail::field_as_double(band.member);
  SONARE_FIELDS_COMPRESSOR(X)
#undef X
  return std::nullopt;
}

namespace {

// Each per-stage helper handles one cluster of keys and returns true if the key
// was recognized (and applied). A flat sequence of independent early-return
// `if` blocks keeps the block-nesting depth at 1, which avoids MSVC error
// C1061 ("blocks nested too deeply") that a long else-if chain triggers.

// ---- repair.* (declick, declip, decrackle, dehum, dereverb, denoise) ----
bool apply_repair_param(MasteringChainConfig& cfg, const std::string& key, double v,
                        StageFlagsSet& flags) {
  // Repair stages keep bespoke parsing (size_t / enum-clamping quirks), so they
  // still need the float / int views of the value.
  const float vf = static_cast<float>(v);
  const auto vi = [&]() {
    int converted = 0;
    if (numeric::checked_integral_cast(v, &converted)) return converted;
    // The key is in hand here, so the refusal names the field the caller wrote
    // rather than leaving them to find it among the rest of the bag.
    reject_integer_param(key, v);
  };
  // ---- repair.declick ----
  if (key == "repair.declick.enabled") {
    mark_enabled(flags.declick, v);
    return true;
  }
  if (key == "repair.declick.threshold") {
    cfg.repair.declick.config.threshold = vf;
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.neighborRatio") {
    cfg.repair.declick.config.neighbor_ratio = vf;
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.maxClickSamples") {
    cfg.repair.declick.config.max_click_samples = checked_nonnegative_size(vi(), key);
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.lpcOrder") {
    cfg.repair.declick.config.lpc_order = vi();
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.residualRatio") {
    cfg.repair.declick.config.residual_ratio = vf;
    mark_field(flags.declick);
    return true;
  }

  // ---- repair.declip ----
  if (key == "repair.declip.enabled") {
    mark_enabled(flags.declip, v);
    return true;
  }
  if (key == "repair.declip.clipThreshold") {
    cfg.repair.declip.config.clip_threshold = vf;
    mark_field(flags.declip);
    return true;
  }
  if (key == "repair.declip.lpcOrder") {
    cfg.repair.declip.config.lpc_order = vi();
    mark_field(flags.declip);
    return true;
  }
  if (key == "repair.declip.iterations") {
    cfg.repair.declip.config.iterations = vi();
    mark_field(flags.declip);
    return true;
  }
  if (key == "repair.declip.lpcBlend") {
    cfg.repair.declip.config.lpc_blend = vf;
    mark_field(flags.declip);
    return true;
  }

  // ---- repair.decrackle ----
  if (key == "repair.decrackle.enabled") {
    mark_enabled(flags.decrackle, v);
    return true;
  }
  if (key == "repair.decrackle.threshold") {
    cfg.repair.decrackle.config.threshold = vf;
    mark_field(flags.decrackle);
    return true;
  }
  if (key == "repair.decrackle.mode") {
    cfg.repair.decrackle.config.mode = vi() == 1
                                           ? mastering::repair::DecrackleMode::WaveletShrinkage
                                           : mastering::repair::DecrackleMode::Median;
    mark_field(flags.decrackle);
    return true;
  }
  if (key == "repair.decrackle.levels") {
    cfg.repair.decrackle.config.levels = vi();
    mark_field(flags.decrackle);
    return true;
  }

  // ---- repair.dehum ----
  if (key == "repair.dehum.enabled") {
    mark_enabled(flags.dehum, v);
    return true;
  }
  if (key == "repair.dehum.fundamentalHz") {
    cfg.repair.dehum.config.fundamental_hz = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.harmonics") {
    cfg.repair.dehum.config.harmonics = vi();
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.q") {
    cfg.repair.dehum.config.q = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.adaptive") {
    cfg.repair.dehum.config.adaptive = v != 0.0;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.searchRangeHz") {
    cfg.repair.dehum.config.search_range_hz = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.adaptation") {
    cfg.repair.dehum.config.adaptation = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.frameSize") {
    cfg.repair.dehum.config.frame_size = vi();
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.pllBandwidth") {
    cfg.repair.dehum.config.pll_bandwidth = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.mode") {
    const int mode = vi();
    // The upper bound names the last enumerator, so it moves whenever one is added.
    if (mode < static_cast<int>(repair::DehumMode::Subtract) ||
        mode > static_cast<int>(repair::DehumMode::Notch)) {
      throw SonareException(ErrorCode::InvalidParameter, "unknown dehum mode");
    }
    cfg.repair.dehum.config.mode = static_cast<repair::DehumMode>(mode);
    mark_field(flags.dehum);
    return true;
  }

  // ---- repair.dereverb ----
  if (key == "repair.dereverb.enabled") {
    mark_enabled(flags.dereverb, v);
    return true;
  }
  if (key == "repair.dereverb.threshold") {
    cfg.repair.dereverb.config.threshold = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.attenuation") {
    cfg.repair.dereverb.config.attenuation = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.nFft") {
    cfg.repair.dereverb.config.n_fft = vi();
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.hopLength") {
    cfg.repair.dereverb.config.hop_length = vi();
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.t60Sec") {
    cfg.repair.dereverb.config.t60_sec = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.lateDelayMs") {
    cfg.repair.dereverb.config.late_delay_ms = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.overSubtraction") {
    cfg.repair.dereverb.config.over_subtraction = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.spectralFloor") {
    cfg.repair.dereverb.config.spectral_floor = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeEnabled") {
    cfg.repair.dereverb.config.wpe_enabled = v != 0.0;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeIterations") {
    cfg.repair.dereverb.config.wpe_iterations = vi();
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeTaps") {
    cfg.repair.dereverb.config.wpe_taps = vi();
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeStrength") {
    cfg.repair.dereverb.config.wpe_strength = vf;
    mark_field(flags.dereverb);
    return true;
  }

  // ---- repair.denoise ----
  if (key == "repair.denoise.enabled") {
    mark_enabled(flags.denoise, v);
    return true;
  }
  if (key == "repair.denoise.mode") {
    const int mode = vi();
    if (mode < static_cast<int>(repair::DenoiseMode::LogMmse) ||
        mode > static_cast<int>(repair::DenoiseMode::SpectralSubtraction)) {
      throw SonareException(ErrorCode::InvalidParameter, "unknown denoise mode");
    }
    cfg.repair.denoise.config.mode = static_cast<repair::DenoiseMode>(mode);
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.noiseEstimator") {
    const int estimator = vi();
    // The upper bound names the last enumerator, so it moves whenever one is added.
    if (estimator < static_cast<int>(repair::DenoiseNoiseEstimator::Quantile) ||
        estimator > static_cast<int>(repair::DenoiseNoiseEstimator::Spp)) {
      throw SonareException(ErrorCode::InvalidParameter, "unknown denoise noise estimator");
    }
    cfg.repair.denoise.config.noise_estimator =
        static_cast<repair::DenoiseNoiseEstimator>(estimator);
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.nFft") {
    cfg.repair.denoise.config.n_fft = vi();
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.hopLength") {
    cfg.repair.denoise.config.hop_length = vi();
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.ddAlpha") {
    cfg.repair.denoise.config.dd_alpha = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.reductionDb") {
    cfg.repair.denoise.config.reduction_db = vf;
    mark_field(flags.denoise);
    return true;
  }
  // Documents written while the knob was a linear floor carry that value, so
  // convert it rather than drop it. The conversion carries the old validity
  // range with it: a floor above 1 becomes a negative depth and is refused.
  if (key == "repair.denoise.gainFloor") {
    cfg.repair.denoise.config.reduction_db = -linear_to_db(vf);
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.overSubtraction") {
    cfg.repair.denoise.config.over_subtraction = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.spectralFloor") {
    cfg.repair.denoise.config.spectral_floor = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.noiseEstimationQuantile") {
    cfg.repair.denoise.config.noise_estimation_quantile = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.speechPresenceGain") {
    cfg.repair.denoise.config.speech_presence_gain = v != 0.0;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.gainSmoothing") {
    cfg.repair.denoise.config.gain_smoothing = v != 0.0;
    mark_field(flags.denoise);
    return true;
  }

  return false;
}

// ---- eq.tilt + dynamics.* (deesser, transientShaper, compressor) ----
bool apply_eq_dynamics_param(MasteringChainConfig& cfg, const std::string& key, double v,
                             StageFlagsSet& flags) {
  // ---- eq.tilt ----
  if (key == "eq.tilt.enabled") {
    mark_enabled(flags.tilt, v);
    return true;
  }
#define X(jkey, member, meta)                    \
  if (key == "eq.tilt." jkey) {                  \
    detail::assign_field(cfg.eq.tilt.member, v); \
    mark_field(flags.tilt);                      \
    return true;                                 \
  }
  SONARE_FIELDS_EQ_TILT(X)
#undef X

  // ---- dynamics.deesser ----
  if (key == "dynamics.deesser.enabled") {
    mark_enabled(flags.deesser, v);
    return true;
  }
#define X(jkey, member, meta)                                    \
  if (key == "dynamics.deesser." jkey) {                         \
    detail::assign_field(cfg.dynamics.deesser.config.member, v); \
    mark_field(flags.deesser);                                   \
    return true;                                                 \
  }
  SONARE_FIELDS_DEESSER(X)
#undef X

  // ---- dynamics.transientShaper ----
  if (key == "dynamics.transientShaper.enabled") {
    mark_enabled(flags.transient_shaper, v);
    return true;
  }
#define X(jkey, member, meta)                                             \
  if (key == "dynamics.transientShaper." jkey) {                          \
    detail::assign_field(cfg.dynamics.transient_shaper.config.member, v); \
    mark_field(flags.transient_shaper);                                   \
    return true;                                                          \
  }
  SONARE_FIELDS_TRANSIENT_SHAPER(X)
#undef X

  // ---- dynamics.compressor ----
  // `detector` is an enum restored via the enum assign_field overload, matching
  // the integer it was serialized as.
  if (key == "dynamics.compressor.enabled") {
    mark_enabled(flags.compressor, v);
    return true;
  }
#define X(jkey, member, meta)                                       \
  if (key == "dynamics.compressor." jkey) {                         \
    detail::assign_field(cfg.dynamics.compressor.config.member, v); \
    mark_field(flags.compressor);                                   \
    return true;                                                    \
  }
  SONARE_FIELDS_COMPRESSOR(X)
#undef X

  return false;
}

// ---- dynamics.multibandComp ----
// @p key is canonical (see canonical_chain_param_key); @p spelled is the caller's own key, which
// is what an out-of-range refusal names. A `crossover.cutoffsHz.<i>` element never reaches here:
// the pre-pass in apply_chain_params consumes it.
bool apply_multiband_param(MasteringChainConfig& cfg, const std::string& key,
                           const std::string& spelled, double v, StageFlagsSet& flags) {
  if (key == "dynamics.multibandComp.enabled") {
    mark_enabled(flags.multiband_comp, v);
    return true;
  }
  std::string_view rest;
  if (!multiband_rest(key, &rest)) return false;
  auto& config = cfg.dynamics.multiband_comp.config;
  // Read as an integer so an out-of-range ordinal reaches validate_chain_multiband_config, whose
  // message names the crossover field.
  if (rest == "slope") {
    int slope = 0;
    detail::assign_field(slope, v);
    config.crossover.slope = static_cast<multiband::CrossoverSlope>(slope);
    mark_field(flags.multiband_comp);
    return true;
  }
  if (rest == "mode") {
    int mode = 0;
    detail::assign_field(mode, v);
    config.crossover.mode = static_cast<multiband::CrossoverMode>(mode);
    mark_field(flags.multiband_comp);
    return true;
  }
  if (rest == "firKernelSize") {
    detail::assign_field(config.crossover.fir_kernel_size, v);
    mark_field(flags.multiband_comp);
    return true;
  }
  // An index past the list is refused rather than dropped while reporting success.
  std::size_t index = 0;
  if (parse_indexed(rest, "cutoff", "Hz", &index)) {
    if (index >= config.crossover.cutoffs_hz.size()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "multiband cutoff index out of range: " + spelled);
    }
    detail::assign_field(config.crossover.cutoffs_hz[index], v);
    mark_field(flags.multiband_comp);
    return true;
  }
  std::string_view field;
  if (!parse_band_field(rest, "band", &index, &field)) return false;
#define X(jkey, member, meta)                                                 \
  if (field == jkey) {                                                        \
    if (index >= config.bands.size()) {                                       \
      throw SonareException(ErrorCode::InvalidParameter,                      \
                            "multiband band index out of range: " + spelled); \
    }                                                                         \
    detail::assign_field(config.bands[index].member, v);                      \
    mark_field(flags.multiband_comp);                                         \
    return true;                                                              \
  }
  SONARE_FIELDS_COMPRESSOR(X)
#undef X
  return false;
}

// ---- saturation.* (tape, exciter) ----
bool apply_saturation_param(MasteringChainConfig& cfg, const std::string& key, double v,
                            StageFlagsSet& flags) {
  // ---- saturation.tape ----
  if (key == "saturation.tape.enabled") {
    mark_enabled(flags.tape, v);
    return true;
  }
#define X(jkey, member, meta)                                   \
  if (key == "saturation.tape." jkey) {                         \
    detail::assign_field(cfg.saturation.tape.config.member, v); \
    mark_field(flags.tape);                                     \
    return true;                                                \
  }
  SONARE_FIELDS_TAPE(X)
#undef X

  // ---- saturation.exciter ----
  if (key == "saturation.exciter.enabled") {
    mark_enabled(flags.exciter, v);
    return true;
  }
#define X(jkey, member, meta)                                      \
  if (key == "saturation.exciter." jkey) {                         \
    detail::assign_field(cfg.saturation.exciter.config.member, v); \
    mark_field(flags.exciter);                                     \
    return true;                                                   \
  }
  SONARE_FIELDS_EXCITER(X)
#undef X

  return false;
}

// ---- spectral.airBand + stereo.* (imager, monoMaker) ----
bool apply_spectral_stereo_param(MasteringChainConfig& cfg, const std::string& key, double v,
                                 StageFlagsSet& flags) {
  // ---- spectral.airBand ----
  if (key == "spectral.airBand.enabled") {
    mark_enabled(flags.air_band, v);
    return true;
  }
#define X(jkey, member, meta)                                     \
  if (key == "spectral.airBand." jkey) {                          \
    detail::assign_field(cfg.spectral.air_band.config.member, v); \
    mark_field(flags.air_band);                                   \
    return true;                                                  \
  }
  SONARE_FIELDS_AIR_BAND(X)
#undef X

  // ---- stereo.imager ----
  if (key == "stereo.imager.enabled") {
    mark_enabled(flags.imager, v);
    return true;
  }
#define X(jkey, member, meta)                                 \
  if (key == "stereo.imager." jkey) {                         \
    detail::assign_field(cfg.stereo.imager.config.member, v); \
    mark_field(flags.imager);                                 \
    return true;                                              \
  }
  SONARE_FIELDS_IMAGER(X)
#undef X

  // ---- stereo.monoMaker ----
  if (key == "stereo.monoMaker.enabled") {
    mark_enabled(flags.mono_maker, v);
    return true;
  }
#define X(jkey, member, meta)                                     \
  if (key == "stereo.monoMaker." jkey) {                          \
    detail::assign_field(cfg.stereo.mono_maker.config.member, v); \
    mark_field(flags.mono_maker);                                 \
    return true;                                                  \
  }
  SONARE_FIELDS_MONO_MAKER(X)
#undef X

  return false;
}

// ---- maximizer.truePeakLimiter + loudness ----
bool apply_maximizer_loudness_param(MasteringChainConfig& cfg, const std::string& key, double v,
                                    StageFlagsSet& flags) {
  // ---- maximizer.truePeakLimiter ----
  if (key == "maximizer.truePeakLimiter.enabled") {
    mark_enabled(flags.true_peak, v);
    return true;
  }
#define X(jkey, member, meta)                                               \
  if (key == "maximizer.truePeakLimiter." jkey) {                           \
    detail::assign_field(cfg.maximizer.true_peak_limiter.config.member, v); \
    mark_field(flags.true_peak);                                            \
    return true;                                                            \
  }
  SONARE_FIELDS_TRUE_PEAK_LIMITER(X)
#undef X

  // ---- loudness ----
  if (key == "loudness.enabled") {
    mark_enabled(flags.loudness, v);
    return true;
  }
#define X(jkey, member, meta)                     \
  if (key == "loudness." jkey) {                  \
    detail::assign_field(cfg.loudness.member, v); \
    mark_field(flags.loudness);                   \
    return true;                                  \
  }
  SONARE_FIELDS_LOUDNESS(X)
#undef X

  return false;
}

void apply_one_param_to_config(MasteringChainConfig& cfg, const std::string& key,
                               const std::string& canonical, double v, StageFlagsSet& flags) {
  // Every chain parameter is ultimately stored as float, int, bool, or enum.
  // Reject non-finite and non-float-representable values before bespoke parsing
  // can narrow them. Integer fields add a lazy checked conversion above.
  float validated = 0.0f;
  detail::assign_field(validated, v);
  (void)validated;
  if (apply_repair_param(cfg, canonical, v, flags)) return;
  if (apply_eq_dynamics_param(cfg, canonical, v, flags)) return;
  if (apply_multiband_param(cfg, canonical, key, v, flags)) return;
  if (apply_saturation_param(cfg, canonical, v, flags)) return;
  if (apply_spectral_stereo_param(cfg, canonical, v, flags)) return;
  if (apply_maximizer_loudness_param(cfg, canonical, v, flags)) return;
  throw SonareException(ErrorCode::InvalidParameter, "unknown chain config key: " + key);
}

// Two phases so the result never depends on key order: a `crossover.cutoffsHz.<i>` set first
// replaces the cutoff list and resizes the bands to match, then every other key applies in order
// against the resized list. Shared by both entry points.
void apply_chain_params(MasteringChainConfig& cfg, const Param* params, std::size_t count,
                        StageFlagsSet& flags) {
  std::vector<std::string> canonical(count);
  std::vector<bool> consumed(count, false);
  std::unordered_map<std::string, std::size_t> multiband_spellings;
  std::map<std::size_t, std::size_t> cutoff_list;  // list index -> param position, last wins
  const std::string* scalar_cutoff = nullptr;
  for (std::size_t i = 0; i < count; ++i) {
    std::string storage;
    canonical[i] = canonical_chain_param_key(params[i].key, &storage);
    std::string_view rest;
    if (!multiband_rest(canonical[i], &rest)) continue;
    const auto [seen, inserted] = multiband_spellings.emplace(canonical[i], i);
    if (!inserted && params[seen->second].key != params[i].key) {
      throw SonareException(
          ErrorCode::InvalidParameter,
          params[seen->second].key + " and " + params[i].key + " name the same multiband field");
    }
    std::size_t index = 0;
    if (is_cutoff_list_key(canonical[i], &index)) {
      cutoff_list[index] = i;
      consumed[i] = true;
    } else if (scalar_cutoff == nullptr && is_scalar_cutoff_key(canonical[i])) {
      scalar_cutoff = &params[i].key;
    }
  }

  if (!cutoff_list.empty()) {
    if (scalar_cutoff != nullptr) {
      throw SonareException(ErrorCode::InvalidParameter,
                            *scalar_cutoff +
                                " cannot be combined with dynamics.multibandComp.crossover."
                                "cutoffsHz, which replaces the cutoff list");
    }
    std::vector<float> cutoffs;
    cutoffs.reserve(cutoff_list.size());
    for (const auto& [index, position] : cutoff_list) {
      if (index != cutoffs.size()) {
        throw SonareException(
            ErrorCode::InvalidParameter,
            "crossover cutoff indices must be contiguous from 0: " + params[position].key);
      }
      float cutoff = 0.0f;
      detail::assign_field(cutoff, params[position].value);
      cutoffs.push_back(cutoff);
    }
    // Existing bands keep their index; a band the list adds starts from CompressorConfig{}.
    auto& config = cfg.dynamics.multiband_comp.config;
    config.crossover.cutoffs_hz = std::move(cutoffs);
    config.bands.resize(config.crossover.cutoffs_hz.size() + 1);
    mark_field(flags.multiband_comp);
  }

  for (std::size_t i = 0; i < count; ++i) {
    if (consumed[i]) continue;
    apply_one_param_to_config(cfg, params[i].key, canonical[i], params[i].value, flags);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// parse_chain_config_params
// ---------------------------------------------------------------------------

namespace {

MasteringChainConfig parse_chain_config_params_over(MasteringChainConfig cfg, const Param* params,
                                                    std::size_t count) {
  validate_params(params, count);
  StageFlagsSet flags;

  apply_chain_params(cfg, params, count, flags);

  cfg.repair.declick.enabled = resolve_enabled(flags.declick);
  cfg.repair.declip.enabled = resolve_enabled(flags.declip);
  cfg.repair.decrackle.enabled = resolve_enabled(flags.decrackle);
  cfg.repair.dehum.enabled = resolve_enabled(flags.dehum);
  cfg.repair.dereverb.enabled = resolve_enabled(flags.dereverb);
  cfg.repair.denoise.enabled = resolve_enabled(flags.denoise);
  cfg.eq.tilt.enabled = resolve_enabled(flags.tilt);
  cfg.dynamics.deesser.enabled = resolve_enabled(flags.deesser);
  cfg.dynamics.transient_shaper.enabled = resolve_enabled(flags.transient_shaper);
  cfg.dynamics.compressor.enabled = resolve_enabled(flags.compressor);
  cfg.dynamics.multiband_comp.enabled = resolve_enabled(flags.multiband_comp);
  cfg.saturation.tape.enabled =
      resolve_color_enabled(flags.tape, saturation::tape_engages_color(cfg.saturation.tape.config));
  cfg.saturation.exciter.enabled = resolve_color_enabled(
      flags.exciter, saturation::exciter_engages_color(cfg.saturation.exciter.config));
  cfg.spectral.air_band.enabled = resolve_enabled(flags.air_band);
  cfg.stereo.imager.enabled = resolve_enabled(flags.imager);
  cfg.stereo.mono_maker.enabled = resolve_enabled(flags.mono_maker);
  cfg.maximizer.true_peak_limiter.enabled = resolve_enabled(flags.true_peak);
  cfg.loudness.enabled = resolve_enabled(flags.loudness);

  validate_mastering_chain_config(cfg);
  return cfg;
}

}  // namespace

MasteringChainConfig parse_chain_config_params(const Param* params, std::size_t count) {
  return parse_chain_config_params_over(MasteringChainConfig{}, params, count);
}

MasteringChainConfig parse_streaming_chain_config_params(const Param* params, std::size_t count) {
  MasteringChainConfig defaults;
  defaults.repair.denoise.config.noise_estimator = repair::DenoiseNoiseEstimator::Spp;
  return parse_chain_config_params_over(std::move(defaults), params, count);
}

// ---------------------------------------------------------------------------
// apply_chain_config_overrides
// In-place differential update: dispatch each key through the shared helper,
// then for any module whose key was touched, update its `enabled` flag.
// Modules not mentioned in the overrides keep their existing `enabled` value.
// ---------------------------------------------------------------------------

void apply_chain_config_overrides(MasteringChainConfig& cfg, const Param* params,
                                  std::size_t count) {
  validate_params(params, count);
  StageFlagsSet flags;

  apply_chain_params(cfg, params, count, flags);

  if (flags.declick.any_key_seen) {
    cfg.repair.declick.enabled = resolve_enabled(flags.declick);
  }
  if (flags.declip.any_key_seen) {
    cfg.repair.declip.enabled = resolve_enabled(flags.declip);
  }
  if (flags.decrackle.any_key_seen) {
    cfg.repair.decrackle.enabled = resolve_enabled(flags.decrackle);
  }
  if (flags.dehum.any_key_seen) {
    cfg.repair.dehum.enabled = resolve_enabled(flags.dehum);
  }
  if (flags.dereverb.any_key_seen) {
    cfg.repair.dereverb.enabled = resolve_enabled(flags.dereverb);
  }
  if (flags.denoise.any_key_seen) {
    cfg.repair.denoise.enabled = resolve_enabled(flags.denoise);
  }
  if (flags.tilt.any_key_seen) {
    cfg.eq.tilt.enabled = resolve_enabled(flags.tilt);
  }
  if (flags.deesser.any_key_seen) {
    cfg.dynamics.deesser.enabled = resolve_enabled(flags.deesser);
  }
  if (flags.transient_shaper.any_key_seen) {
    cfg.dynamics.transient_shaper.enabled = resolve_enabled(flags.transient_shaper);
  }
  if (flags.compressor.any_key_seen) {
    cfg.dynamics.compressor.enabled = resolve_enabled(flags.compressor);
  }
  if (flags.multiband_comp.any_key_seen) {
    cfg.dynamics.multiband_comp.enabled = resolve_enabled(flags.multiband_comp);
  }
  // Color stages (tape/exciter): in the OVERRIDES path we only honor an
  // explicit `enabled`. Recomputing from any_key_seen && meaningful here would
  // let a single param override silently disable a stage the preset enabled
  // (the meaningful predicate can be false even though the preset opted in).
  // The parse-from-scratch path keeps the auto-engage behavior.
  if (flags.tape.enabled_explicit) {
    cfg.saturation.tape.enabled = resolve_color_enabled(
        flags.tape, saturation::tape_engages_color(cfg.saturation.tape.config));
  }
  if (flags.exciter.enabled_explicit) {
    cfg.saturation.exciter.enabled = resolve_color_enabled(
        flags.exciter, saturation::exciter_engages_color(cfg.saturation.exciter.config));
  }
  if (flags.air_band.any_key_seen) {
    cfg.spectral.air_band.enabled = resolve_enabled(flags.air_band);
  }
  if (flags.imager.any_key_seen) {
    cfg.stereo.imager.enabled = resolve_enabled(flags.imager);
  }
  if (flags.mono_maker.any_key_seen) {
    cfg.stereo.mono_maker.enabled = resolve_enabled(flags.mono_maker);
  }
  if (flags.true_peak.any_key_seen) {
    cfg.maximizer.true_peak_limiter.enabled = resolve_enabled(flags.true_peak);
  }
  if (flags.loudness.any_key_seen) {
    cfg.loudness.enabled = resolve_enabled(flags.loudness);
  }
  // Only the multiband stage is validated here, since its list edits can leave a shape no stage
  // accepts; the message matches the one parse_chain_config_params gives.
  validate_chain_multiband_config(cfg.dynamics.multiband_comp.config);
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

MonoChainResult run_chain_mono_params(const Param* params, std::size_t param_count,
                                      const float* samples, std::size_t length, int sample_rate) {
  MasteringChain chain(parse_chain_config_params(params, param_count));
  return chain.process_mono(samples, length, sample_rate);
}

StereoChainResult run_chain_stereo_params(const Param* params, std::size_t param_count,
                                          const float* left, const float* right, std::size_t length,
                                          int sample_rate) {
  MasteringChain chain(parse_chain_config_params(params, param_count));
  return chain.process_stereo(left, right, length, sample_rate);
}

}  // namespace sonare::mastering::api
