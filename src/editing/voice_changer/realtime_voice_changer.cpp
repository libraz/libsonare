#include "editing/voice_changer/realtime_voice_changer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/json_schema.h"

// The C ABI ↔ C++ ABI version consistency check (kVoiceChangerAbiVersion ==
// SONARE_VOICE_CHANGER_ABI_VERSION) lives in src/sonare_c_daw.cpp. Keeping it
// there preserves the layer rule "editing/ must not depend on the public C
// API header sonare_c.h" while still failing the build the moment the two
// constants drift.

namespace sonare::editing::voice_changer {
namespace {

constexpr float kDeessGainSmoothingHz = 200.0f;  ///< Smoothing of the deesser gain reduction.
constexpr float kDeessEnvelopeHz = 100.0f;       ///< Smoothing of the sibilance envelope.
constexpr float kFastDetectorHz = 200.0f;        ///< Gate/comp envelope detector cutoff (~0.8 ms).
constexpr float kLimiterAttackMs = 0.1f;  ///< Sub-millisecond limiter attack (~5 samples @48k).

float db_to_gain(float db) noexcept { return sonare::db_to_linear(db); }

float amp_to_db(float amp) noexcept {
  return 20.0f * std::log10(std::max(std::abs(amp), sonare::constants::kAmpEpsilon));
}

/// Bidirectional one-pole follower used by the gate/comp/limiter gain stages.
/// Switches between @p attack_alpha and @p release_alpha based on the direction
/// of motion toward @p target. With @p attack_when_decreasing = true, attack
/// applies when the gain ramps *down* (compressor / limiter onset). With
/// @p attack_when_decreasing = false, attack applies when the gain ramps *up*
/// (gate opening). Coefficients are the step ratios returned by
/// @c rt::one_pole_alpha_from_time_ms.
inline float smooth_attack_release(float& state, float target, float attack_alpha,
                                   float release_alpha, bool attack_when_decreasing) noexcept {
  const bool use_attack = attack_when_decreasing ? target < state : target > state;
  const float alpha = use_attack ? attack_alpha : release_alpha;
  state += alpha * (target - state);
  return state;
}

float finite_or(float value, float fallback) noexcept {
  return std::isfinite(value) ? value : fallback;
}

float object_number(const sonare::util::json::Value& object, const char* key, float fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_number()) return fallback;
  return finite_or(value->as_float(), fallback);
}

int object_int(const sonare::util::json::Value& object, const char* key, int fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_number()) return fallback;
  const double n = value->as_number();
  if (!std::isfinite(n)) return fallback;
  return static_cast<int>(n);
}

bool object_bool(const sonare::util::json::Value& object, const char* key, bool fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_bool()) return fallback;
  return value->as_bool();
}

void dump_number(std::ostringstream& out, float value) {
  // max_digits10 (17 for IEEE-754 double) ensures lossless roundtrip — matches
  // util/json::dump_value. Stream's locale is classic (imbued by the callers),
  // so the decimal separator is always "." regardless of LC_NUMERIC.
  out << std::setprecision(std::numeric_limits<double>::max_digits10) << static_cast<double>(value);
}

void dump_field(std::ostringstream& out, const char* key, float value, bool last = false) {
  out << '"' << key << "\":";
  dump_number(out, value);
  if (!last) out << ',';
}

void dump_int_field(std::ostringstream& out, const char* key, int value, bool last = false) {
  out << '"' << key << "\":" << value;
  if (!last) out << ',';
}

void dump_bool_field(std::ostringstream& out, const char* key, bool value, bool last = false) {
  out << '"' << key << "\":" << (value ? "true" : "false");
  if (!last) out << ',';
}

void dump_dsp_section(std::ostringstream& out, const RealtimeVoiceChangerConfig& c) {
  out << "\"dsp\":{";
  dump_field(out, "inputGainDb", c.input_gain_db);
  dump_field(out, "outputGainDb", c.output_gain_db);
  dump_field(out, "wetMix", c.wet_mix);
  out << "\"retune\":{";
  dump_field(out, "semitones", c.retune.semitones);
  dump_field(out, "mix", c.retune.mix);
  dump_int_field(out, "grainSize", c.retune.grain_size, true);
  out << "},\"formant\":{";
  dump_field(out, "factor", c.formant.factor);
  dump_field(out, "amount", c.formant.amount);
  dump_field(out, "body", c.formant.body);
  dump_field(out, "brightness", c.formant.brightness);
  dump_field(out, "nasal", c.formant.nasal, true);
  out << "},\"eq\":{";
  dump_field(out, "highpassHz", c.eq.highpass_hz);
  dump_field(out, "bodyDb", c.eq.body_db);
  dump_field(out, "presenceDb", c.eq.presence_db);
  dump_field(out, "airDb", c.eq.air_db, true);
  out << "},\"gate\":{";
  dump_field(out, "thresholdDb", c.gate.threshold_db);
  dump_field(out, "attackMs", c.gate.attack_ms);
  dump_field(out, "releaseMs", c.gate.release_ms);
  dump_field(out, "rangeDb", c.gate.range_db, true);
  out << "},\"compressor\":{";
  dump_field(out, "thresholdDb", c.compressor.threshold_db);
  dump_field(out, "ratio", c.compressor.ratio);
  dump_field(out, "attackMs", c.compressor.attack_ms);
  dump_field(out, "releaseMs", c.compressor.release_ms);
  dump_field(out, "makeupGainDb", c.compressor.makeup_gain_db, true);
  out << "},\"deesser\":{";
  dump_field(out, "frequencyHz", c.deesser.frequency_hz);
  dump_field(out, "thresholdDb", c.deesser.threshold_db);
  dump_field(out, "ratio", c.deesser.ratio);
  dump_field(out, "rangeDb", c.deesser.range_db, true);
  out << "},\"reverb\":{";
  dump_field(out, "mix", c.reverb.mix);
  dump_field(out, "timeMs", c.reverb.time_ms);
  dump_field(out, "damping", c.reverb.damping);
  dump_int_field(out, "seed", c.reverb.seed, true);
  out << "},\"limiter\":{";
  dump_field(out, "ceilingDb", c.limiter.ceiling_db);
  dump_field(out, "releaseMs", c.limiter.release_ms);
  dump_bool_field(out, "enableIspLimiter", c.limiter.enable_isp_limiter);
  dump_field(out, "ispCeilingDbtp", c.limiter.isp_ceiling_dbtp, true);
  out << "}}";
}

std::string trim_copy(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  std::size_t last = text.size();
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  return std::string(text.substr(first, last - first));
}

// Validator helpers (has_allowed_keys, require_key, require_number,
// require_string) now live in util/json_schema.h. Pull them into this anonymous
// namespace under their old unqualified names so the validate_*() helpers below
// stay readable.
using sonare::util::json::schema::has_allowed_keys;
using sonare::util::json::schema::require_key;
using sonare::util::json::schema::require_number;
using sonare::util::json::schema::require_string;

/// Verifies the preset id matches the schema regex `^[a-z0-9][a-z0-9._-]*$` and
/// is within the 1..96 length bound. Kept inline because @c require_string in
/// util/json_schema.h is intentionally regex-free — only this one field needs
/// the slug constraint, so embedding a generic regex matcher there would be
/// dead weight.
inline bool is_valid_preset_id(std::string_view s) noexcept {
  if (s.empty() || s.size() > 96) return false;
  const auto first = static_cast<unsigned char>(s.front());
  const bool first_ok = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
  if (!first_ok) return false;
  for (std::size_t i = 1; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

/// Single source of truth for preset metadata. Adding a new preset is a
/// one-row table edit + a switch arm in @ref realtime_voice_changer_preset
/// (which carries the divergent default values); the id/display-name
/// accessors below all derive from this table so they cannot drift.
struct PresetMetadata {
  VoiceCharacterPreset preset;
  std::string_view id;            ///< Stable identifier exposed across all bindings.
  std::string_view display_name;  ///< Human-readable label used in UI surfaces.
};

constexpr std::array<PresetMetadata, 6> kPresetMetadata = {{
    {VoiceCharacterPreset::NeutralMonitor, "neutral-monitor", "Neutral Monitor"},
    {VoiceCharacterPreset::BrightIdol, "bright-idol", "Bright Idol"},
    {VoiceCharacterPreset::SoftWhisper, "soft-whisper", "Soft Whisper"},
    {VoiceCharacterPreset::DeepNarrator, "deep-narrator", "Deep Narrator"},
    {VoiceCharacterPreset::RobotMascot, "robot-mascot", "Robot Mascot"},
    {VoiceCharacterPreset::DarkVillain, "dark-villain", "Dark Villain"},
}};

// Compile-time check that every enumerator gets a metadata row. Adding a new
// preset value to VoiceCharacterPreset without updating the table now fails
// the build rather than silently falling back to NeutralMonitor at runtime.
static_assert(kPresetMetadata.size() ==
                  static_cast<std::size_t>(VoiceCharacterPreset::DarkVillain) + 1,
              "kPresetMetadata is out of sync with VoiceCharacterPreset");

const PresetMetadata& preset_metadata(VoiceCharacterPreset preset) noexcept {
  for (const auto& row : kPresetMetadata) {
    if (row.preset == preset) return row;
  }
  // Unreachable for any value of the enum (every case is in the table).
  return kPresetMetadata[0];
}

const char* realtime_voice_changer_preset_display_name(VoiceCharacterPreset preset) noexcept {
  return preset_metadata(preset).display_name.data();
}

bool validate_dsp_section(const sonare::util::json::Value& dsp, std::string* error) {
  if (!has_allowed_keys(dsp,
                        {"inputGainDb", "outputGainDb", "wetMix", "retune", "formant", "eq", "gate",
                         "compressor", "deesser", "reverb", "limiter"},
                        "dsp", error)) {
    return false;
  }
  for (const char* key :
       {"retune", "formant", "eq", "gate", "compressor", "deesser", "reverb", "limiter"}) {
    if (!require_key(dsp, key, "dsp", error)) return false;
  }
  if (dsp.find("inputGainDb") && !require_number(dsp, "inputGainDb", -24.0, 24.0, "dsp", error)) {
    return false;
  }
  if (dsp.find("outputGainDb") && !require_number(dsp, "outputGainDb", -36.0, 12.0, "dsp", error)) {
    return false;
  }
  if (dsp.find("wetMix") && !require_number(dsp, "wetMix", 0.0, 1.0, "dsp", error)) return false;

  const auto& retune = *dsp.find("retune");
  if (!has_allowed_keys(retune, {"semitones", "mix", "grainSize"}, "dsp.retune", error)) {
    return false;
  }
  for (const char* key : {"semitones", "mix", "grainSize"}) {
    if (!require_key(retune, key, "dsp.retune", error)) return false;
  }
  if (!require_number(retune, "semitones", -24.0, 24.0, "dsp.retune", error)) return false;
  if (!require_number(retune, "mix", 0.0, 1.0, "dsp.retune", error)) return false;
  if (!require_number(retune, "grainSize", 0.0, 8192.0, "dsp.retune", error, true)) return false;

  const auto& formant = *dsp.find("formant");
  if (!has_allowed_keys(formant, {"factor", "amount", "body", "brightness", "nasal"}, "dsp.formant",
                        error)) {
    return false;
  }
  for (const char* key : {"factor", "amount", "body", "brightness", "nasal"}) {
    if (!require_key(formant, key, "dsp.formant", error)) return false;
  }
  if (!require_number(formant, "factor", 0.55, 1.65, "dsp.formant", error)) return false;
  if (!require_number(formant, "amount", 0.0, 1.0, "dsp.formant", error)) return false;
  if (!require_number(formant, "body", -1.0, 1.0, "dsp.formant", error)) return false;
  if (!require_number(formant, "brightness", -1.0, 1.0, "dsp.formant", error)) return false;
  if (!require_number(formant, "nasal", -1.0, 1.0, "dsp.formant", error)) return false;

  const auto& eq = *dsp.find("eq");
  if (!has_allowed_keys(eq, {"highpassHz", "bodyDb", "presenceDb", "airDb"}, "dsp.eq", error)) {
    return false;
  }
  for (const char* key : {"highpassHz", "bodyDb", "presenceDb", "airDb"}) {
    if (!require_key(eq, key, "dsp.eq", error)) return false;
  }
  if (!require_number(eq, "highpassHz", 20.0, 300.0, "dsp.eq", error)) return false;
  if (!require_number(eq, "bodyDb", -12.0, 12.0, "dsp.eq", error)) return false;
  if (!require_number(eq, "presenceDb", -12.0, 12.0, "dsp.eq", error)) return false;
  if (!require_number(eq, "airDb", -12.0, 12.0, "dsp.eq", error)) return false;

  const auto& gate = *dsp.find("gate");
  if (!has_allowed_keys(gate, {"thresholdDb", "attackMs", "releaseMs", "rangeDb"}, "dsp.gate",
                        error)) {
    return false;
  }
  for (const char* key : {"thresholdDb", "attackMs", "releaseMs", "rangeDb"}) {
    if (!require_key(gate, key, "dsp.gate", error)) return false;
  }
  if (!require_number(gate, "thresholdDb", -90.0, -12.0, "dsp.gate", error)) return false;
  if (!require_number(gate, "attackMs", 0.05, 100.0, "dsp.gate", error)) return false;
  if (!require_number(gate, "releaseMs", 5.0, 1000.0, "dsp.gate", error)) return false;
  if (!require_number(gate, "rangeDb", 0.0, 80.0, "dsp.gate", error)) return false;

  const auto& compressor = *dsp.find("compressor");
  if (!has_allowed_keys(compressor,
                        {"thresholdDb", "ratio", "attackMs", "releaseMs", "makeupGainDb"},
                        "dsp.compressor", error)) {
    return false;
  }
  for (const char* key : {"thresholdDb", "ratio", "attackMs", "releaseMs", "makeupGainDb"}) {
    if (!require_key(compressor, key, "dsp.compressor", error)) return false;
  }
  if (!require_number(compressor, "thresholdDb", -60.0, 0.0, "dsp.compressor", error)) {
    return false;
  }
  if (!require_number(compressor, "ratio", 1.0, 20.0, "dsp.compressor", error)) return false;
  if (!require_number(compressor, "attackMs", 0.05, 200.0, "dsp.compressor", error)) return false;
  if (!require_number(compressor, "releaseMs", 5.0, 2000.0, "dsp.compressor", error)) {
    return false;
  }
  if (!require_number(compressor, "makeupGainDb", -12.0, 12.0, "dsp.compressor", error)) {
    return false;
  }

  const auto& deesser = *dsp.find("deesser");
  if (!has_allowed_keys(deesser, {"frequencyHz", "thresholdDb", "ratio", "rangeDb"}, "dsp.deesser",
                        error)) {
    return false;
  }
  for (const char* key : {"frequencyHz", "thresholdDb", "ratio", "rangeDb"}) {
    if (!require_key(deesser, key, "dsp.deesser", error)) return false;
  }
  if (!require_number(deesser, "frequencyHz", 3000.0, 12000.0, "dsp.deesser", error)) {
    return false;
  }
  if (!require_number(deesser, "thresholdDb", -60.0, -6.0, "dsp.deesser", error)) return false;
  if (!require_number(deesser, "ratio", 1.0, 20.0, "dsp.deesser", error)) return false;
  if (!require_number(deesser, "rangeDb", 0.0, 24.0, "dsp.deesser", error)) return false;

  const auto& reverb = *dsp.find("reverb");
  if (!has_allowed_keys(reverb, {"mix", "timeMs", "damping", "seed"}, "dsp.reverb", error)) {
    return false;
  }
  for (const char* key : {"mix", "timeMs", "damping", "seed"}) {
    if (!require_key(reverb, key, "dsp.reverb", error)) return false;
  }
  if (!require_number(reverb, "mix", 0.0, 0.45, "dsp.reverb", error)) return false;
  if (!require_number(reverb, "timeMs", 40.0,
                      static_cast<double>(RealtimeVoiceChanger::kMaxReverbTimeMs), "dsp.reverb",
                      error)) {
    return false;
  }
  if (!require_number(reverb, "damping", 0.0, 1.0, "dsp.reverb", error)) return false;
  if (!require_number(reverb, "seed", -2147483648.0, 2147483647.0, "dsp.reverb", error, true)) {
    return false;
  }

  const auto& limiter = *dsp.find("limiter");
  // enableIspLimiter / ispCeilingDbtp are optional: presets authored before the
  // ISP stage was added omit them and fall back to the POD defaults (enabled,
  // -1.0 dBTP). When present they are validated below.
  if (!has_allowed_keys(limiter, {"ceilingDb", "releaseMs", "enableIspLimiter", "ispCeilingDbtp"},
                        "dsp.limiter", error)) {
    return false;
  }
  for (const char* key : {"ceilingDb", "releaseMs"}) {
    if (!require_key(limiter, key, "dsp.limiter", error)) return false;
  }
  // True-peak headroom: sample-domain limiter cannot bound inter-sample peaks,
  // so the upper bound is -1.0 dBFS (one full dB of inter-sample headroom)
  // rather than -0.3 dBFS. Builds that opt in to a 4x-oversampled TP detector
  // can later relax this.
  if (!require_number(limiter, "ceilingDb", -12.0, -1.0, "dsp.limiter", error)) return false;
  if (!require_number(limiter, "releaseMs", 1.0, 500.0, "dsp.limiter", error)) return false;
  if (const auto* isp = limiter.find("enableIspLimiter")) {
    if (!isp->is_bool()) {
      if (error) *error = "field must be a boolean: dsp.limiter.enableIspLimiter";
      return false;
    }
  }
  // The ISP stage detects inter-sample peaks via 4x oversampling, so its ceiling
  // may legitimately reach 0.0 dBTP (no extra headroom needed) unlike the
  // sample-domain ceilingDb above.
  if (limiter.find("ispCeilingDbtp") != nullptr) {
    if (!require_number(limiter, "ispCeilingDbtp", -12.0, 0.0, "dsp.limiter", error)) return false;
  }
  return true;
}

bool config_is_finite(const RealtimeVoiceChangerConfig& c, std::string* error) {
  auto check = [&](const char* name, float v) -> bool {
    if (std::isfinite(v)) return true;
    if (error) *error = std::string("config field is not finite: ") + name;
    return false;
  };
  return check("inputGainDb", c.input_gain_db) && check("outputGainDb", c.output_gain_db) &&
         check("wetMix", c.wet_mix) && check("retune.semitones", c.retune.semitones) &&
         check("retune.mix", c.retune.mix) && check("formant.factor", c.formant.factor) &&
         check("formant.amount", c.formant.amount) && check("formant.body", c.formant.body) &&
         check("formant.brightness", c.formant.brightness) &&
         check("formant.nasal", c.formant.nasal) && check("eq.highpassHz", c.eq.highpass_hz) &&
         check("eq.bodyDb", c.eq.body_db) && check("eq.presenceDb", c.eq.presence_db) &&
         check("eq.airDb", c.eq.air_db) && check("gate.thresholdDb", c.gate.threshold_db) &&
         check("gate.attackMs", c.gate.attack_ms) && check("gate.releaseMs", c.gate.release_ms) &&
         check("gate.rangeDb", c.gate.range_db) &&
         check("compressor.thresholdDb", c.compressor.threshold_db) &&
         check("compressor.ratio", c.compressor.ratio) &&
         check("compressor.attackMs", c.compressor.attack_ms) &&
         check("compressor.releaseMs", c.compressor.release_ms) &&
         check("compressor.makeupGainDb", c.compressor.makeup_gain_db) &&
         check("deesser.frequencyHz", c.deesser.frequency_hz) &&
         check("deesser.thresholdDb", c.deesser.threshold_db) &&
         check("deesser.ratio", c.deesser.ratio) && check("deesser.rangeDb", c.deesser.range_db) &&
         check("reverb.mix", c.reverb.mix) && check("reverb.timeMs", c.reverb.time_ms) &&
         check("reverb.damping", c.reverb.damping) &&
         check("limiter.ceilingDb", c.limiter.ceiling_db) &&
         check("limiter.releaseMs", c.limiter.release_ms) &&
         check("limiter.ispCeilingDbtp", c.limiter.isp_ceiling_dbtp);
}

}  // namespace

RealtimeVoiceChanger::RealtimeVoiceChanger(RealtimeVoiceChangerConfig config)
    : config_(normalize_realtime_voice_changer_config(config)) {
  // Derive sample-rate-independent gains/mixes so that config() observers see
  // consistent state even before prepare() is called. The sample-rate-dependent
  // branch inside update_derived() is guarded and will be re-run by prepare().
  update_derived(config_);
  // Publish the initial snapshot so the audio thread can adopt it on the first
  // process_block() call even if set_config() is never invoked.
  config_publisher_.publish(std::make_shared<const RealtimeVoiceChangerConfig>(config_));
}

void RealtimeVoiceChanger::prepare(double sample_rate, int max_block_size, int num_channels) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  if (num_channels < 1 || num_channels > 2)
    throw SonareException(ErrorCode::InvalidParameter, "num_channels must be 1 or 2");

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  num_channels_ = num_channels;
  channels_.resize(static_cast<std::size_t>(num_channels_));
  scratch_.assign(static_cast<std::size_t>(std::max(1, max_block_size_)), 0.0f);
  // Allocation phase: this is the only place buffers may be (re)sized.
  for (auto& channel : channels_) allocate_channel(channel);
  update_derived(config_);
  // Configuration phase: realtime-safe coefficient/state updates. Safe to do
  // here from the control thread because prepare() is called before any audio
  // thread runs against this instance.
  for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
    apply_channel_config(channels_[ch], static_cast<int>(ch), config_);
  }
  reset();
  // Re-publish so the audio thread sees the same (post-prepare) snapshot and
  // does NOT re-apply coefficients on its first block (they are already
  // up-to-date from the loop above). adopt_snapshot_for_block() detects this
  // via the applied_snapshot_ pointer guard.
  auto fresh = std::make_shared<const RealtimeVoiceChangerConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_.publish(std::move(fresh));
  // Force the audio-thread current pointer to match applied_snapshot_ now,
  // so the first process_block() does not redundantly re-apply coefficients.
  config_publisher_.acquire();
}

void RealtimeVoiceChanger::reset() {
  for (auto& channel : channels_) reset_channel(channel);
}

void RealtimeVoiceChanger::set_config(const RealtimeVoiceChangerConfig& config) {
  // Control-thread side: normalize, update the visible mirror used by config(),
  // and hand the snapshot off to the audio thread via the lock-free publisher.
  // We deliberately DO NOT touch derived scalars (input_gain_, gate_attack_,
  // ...) or per-channel BiquadState here — those are written by the audio
  // thread inside adopt_snapshot_for_block() so set_config() can race safely
  // with process_block().
  config_ = normalize_realtime_voice_changer_config(config);
  config_publisher_.publish(std::make_shared<const RealtimeVoiceChangerConfig>(config_));
}

void RealtimeVoiceChanger::update_derived(const RealtimeVoiceChangerConfig& config) {
  input_gain_ = db_to_gain(config.input_gain_db);
  output_gain_ = db_to_gain(config.output_gain_db);
  wet_mix_ = std::clamp(config.wet_mix, 0.0f, 1.0f);
  if (sample_rate_ > 0.0) {
    fast_det_alpha_ = rt::one_pole_lowpass_alpha_matched(kFastDetectorHz, sample_rate_);
    gate_attack_ = rt::one_pole_alpha_from_time_ms(config.gate.attack_ms, sample_rate_);
    gate_release_ = rt::one_pole_alpha_from_time_ms(config.gate.release_ms, sample_rate_);
    comp_attack_ = rt::one_pole_alpha_from_time_ms(config.compressor.attack_ms, sample_rate_);
    comp_release_ = rt::one_pole_alpha_from_time_ms(config.compressor.release_ms, sample_rate_);
    limiter_attack_ = rt::one_pole_alpha_from_time_ms(kLimiterAttackMs, sample_rate_);
    limiter_release_ = rt::one_pole_alpha_from_time_ms(config.limiter.release_ms, sample_rate_);
    deess_alpha_ = rt::one_pole_lowpass_alpha_matched(kDeessEnvelopeHz, sample_rate_);
    deess_gain_alpha_ = rt::one_pole_lowpass_alpha_matched(kDeessGainSmoothingHz, sample_rate_);
  }
}

void RealtimeVoiceChanger::allocate_channel(ChannelState& state) {
  // Sub-component allocations: streaming retune/formant/reverb own their
  // internal buffers and resize them inside their own prepare() entry points.
  state.retune.prepare(sample_rate_, max_block_size_);
  state.formant.prepare(sample_rate_, max_block_size_);
  state.reverb.prepare(sample_rate_, max_block_size_);
  // ISP limiter: prepared unconditionally so toggling
  // LimiterConfig::enable_isp_limiter at runtime never triggers an allocation
  // from the audio thread. Cost is small (one TruePeakFilter history vector +
  // a sliding-max ring, both proportional to max_block_size_).
  state.isp_limiter.prepare(sample_rate_, max_block_size_);
}

void RealtimeVoiceChanger::apply_channel_config(ChannelState& state, int channel_index,
                                                const RealtimeVoiceChangerConfig& config) {
  // Sub-component coefficient updates (no buffer resizing).
  state.retune.set_config(config.retune);
  state.formant.set_config(config.formant);
  state.reverb.set_config(config.reverb, channel_index);

  // Biquad coefficient updates: pure scalar math, RT-safe.
  state.hpf.set(rt::rbj_highpass(rt::frequency_to_w0(config.eq.highpass_hz, sample_rate_),
                                 sonare::constants::kButterworthQ));
  state.body.set(rt::rbj_peak(rt::frequency_to_w0(180.0f, sample_rate_), 0.85f, config.eq.body_db));
  state.presence.set(
      rt::rbj_peak(rt::frequency_to_w0(3600.0f, sample_rate_), 0.9f, config.eq.presence_db));
  state.air.set(
      rt::rbj_high_shelf(rt::frequency_to_w0(9500.0f, sample_rate_), 0.75f, config.eq.air_db));
  state.deess_band.set(
      rt::rbj_bandpass(rt::frequency_to_w0(config.deesser.frequency_hz, sample_rate_), 2.2f));

  // ISP limiter config updates are RT-safe (no allocation / no re-prepare).
  // The enable flag is read at block dispatch time in process_block; this only
  // mirrors the time-constant changes.
  state.isp_limiter.set_config({config.limiter.isp_ceiling_dbtp, config.limiter.release_ms});
}

const RealtimeVoiceChangerConfig& RealtimeVoiceChanger::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry point. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients and re-apply per-channel DSP coefficients
  // — both write to members that the per-sample loop reads, but the loop has
  // not started yet for this block, so no race.
  config_publisher_.acquire();
  const RealtimeVoiceChangerConfig* current = config_publisher_.current();
  if (current && current != applied_snapshot_) {
    update_derived(*current);
    for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
      apply_channel_config(channels_[ch], static_cast<int>(ch), *current);
    }
    applied_snapshot_ = current;
  }
  // Fallback path: only reachable if the constructor's initial publish was
  // dropped (ring full, which cannot happen for a fresh publisher) AND prepare
  // was never called. In that case use the control-thread mirror; the per-
  // sample loop is itself guarded by sample_rate_ > 0.0 so this path stays
  // defined even with default-initialised members.
  return current ? *current : config_;
}

void RealtimeVoiceChanger::reset_channel(ChannelState& state) {
  state.retune.reset();
  state.formant.reset();
  state.hpf.reset();
  state.body.reset();
  state.presence.reset();
  state.air.reset();
  state.deess_band.reset();
  state.gate_env = 0.0f;
  state.gate_gain = 1.0f;
  state.comp_env = 0.0f;
  state.comp_gain = 1.0f;
  state.deess_env = 0.0f;
  state.deess_gain = 1.0f;
  state.limiter_gain = 1.0f;
  state.reverb.reset();
  state.isp_limiter.reset();
}

float RealtimeVoiceChanger::process_input_stage(ChannelState& state,
                                                const RealtimeVoiceChangerConfig& config,
                                                float input) noexcept {
  // Apply input gain then a 2nd-order HPF. The HPF (highpass_hz >= 20) already
  // removes DC, so no separate DC blocker is needed.
  float x = state.hpf.process(input * input_gain_);

  // Noise gate.
  //   1. A fixed fast detector (~0.8 ms) follows |x| so the level estimate
  //      tracks transients without being delayed by the user's A/R settings.
  //   2. The gate gain itself is exponentially smoothed using the
  //      user-configured attack (when opening) / release (when closing).
  //      Smoothing the *gain* — not just the detector — is what eliminates
  //      the zipper noise that a hard threshold-cross produces.
  const float env_in = std::abs(x);
  state.gate_env += fast_det_alpha_ * (env_in - state.gate_env);
  const float gate_target = amp_to_db(state.gate_env) < config.gate.threshold_db
                                ? db_to_gain(-config.gate.range_db)
                                : 1.0f;
  smooth_attack_release(state.gate_gain, gate_target, gate_attack_, gate_release_,
                        /*attack_when_decreasing=*/false);
  x *= state.gate_gain;
  return x;
}

float RealtimeVoiceChanger::process_output_stage(ChannelState& state,
                                                 const RealtimeVoiceChangerConfig& config,
                                                 float input) noexcept {
  float x = input;
  x = state.body.process(x);
  x = state.presence.process(x);
  x = state.air.process(x);

  // Compressor: feed-forward with ratio-based reduction.
  //   Detection uses the same fast follower as the gate so the user's
  //   attack/release apply to the *gain* only — a single-stage LP. The
  //   earlier double-smoothing (detector A/R + gain A/R with the same
  //   coefficients) stretched the effective time constant ~2x and made
  //   the user-set attack feel sluggish.
  const float comp_env_in = std::abs(x);
  state.comp_env += fast_det_alpha_ * (comp_env_in - state.comp_env);
  const float over = amp_to_db(state.comp_env) - config.compressor.threshold_db;
  float comp_target = 1.0f;
  if (over > 0.0f) {
    const float reduction_db = over - over / config.compressor.ratio;
    comp_target = db_to_gain(-reduction_db + config.compressor.makeup_gain_db);
  } else {
    comp_target = db_to_gain(config.compressor.makeup_gain_db);
  }
  smooth_attack_release(state.comp_gain, comp_target, comp_attack_, comp_release_,
                        /*attack_when_decreasing=*/true);
  x *= state.comp_gain;

  // De-esser: ratio-based broadband reduction triggered by the sibilance
  // band-pass. The kDeessEnvelopeHz LP gives a fast (~1.6 ms) detector, and
  // kDeessGainSmoothingHz smooths the gain itself (~0.8 ms) so the two stages
  // serve distinct purposes — detector tracking vs gain dezippering.
  const float ess = std::abs(state.deess_band.process(x));
  state.deess_env += deess_alpha_ * (ess - state.deess_env);
  const float ess_over = amp_to_db(state.deess_env) - config.deesser.threshold_db;
  float deess_target = 1.0f;
  if (ess_over > 0.0f) {
    const float reduction_db =
        std::min(config.deesser.range_db, ess_over - ess_over / config.deesser.ratio);
    deess_target = db_to_gain(-reduction_db);
  }
  state.deess_gain += deess_gain_alpha_ * (deess_target - state.deess_gain);
  x *= state.deess_gain;

  // Reverb: variable-length Schroeder reverb (2 combs + 1 series allpass).
  // Implementation lives in streaming_reverb.{h,cpp}; the helper handles
  // wet/dry mix internally and returns the mixed signal.
  x = state.reverb.process_sample(x);

  // Output gain + simple peak limiter. Not a true-peak (inter-sample) limiter
  // — the schema cap on ceilingDb keeps typical material safe without 4x
  // oversampling. Attack is sub-millisecond (kLimiterAttackMs) so transient
  // bursts taper across ~5 samples instead of a single-sample step (audibly
  // clicks); the final clamp absorbs the residual peak over that taper.
  x *= output_gain_;
  const float ceiling = db_to_gain(config.limiter.ceiling_db);
  const float abs_x = std::abs(x);
  const float limit_target =
      abs_x > ceiling ? ceiling / std::max(abs_x, sonare::constants::kAmpEpsilon) : 1.0f;
  smooth_attack_release(state.limiter_gain, limit_target, limiter_attack_, limiter_release_,
                        /*attack_when_decreasing=*/true);
  return std::clamp(x * state.limiter_gain, -ceiling, ceiling);
}

void RealtimeVoiceChanger::ensure_scratch(int num_samples) noexcept {
  // RT-safe: prepare() always allocates max_block_size_ samples up front, so
  // the scratch buffer is guaranteed to be large enough whenever process_block
  // accepts the request. The caller MUST have already validated
  // num_samples <= max_block_size_ — we never resize here (an audio-thread
  // resize would risk priority inversion).
  assert(num_samples <= max_block_size_);
  (void)num_samples;
}

void RealtimeVoiceChanger::process_block(const float* input, float* output,
                                         int num_samples) noexcept {
  rt::ScopedNoDenormals no_denormals;
  // Pre-condition violations are silent no-ops to keep this RT-safe (no throw,
  // no allocation). When sample_rate_ is unset we still zero-fill the output
  // so callers observe a defined buffer state rather than uninitialised memory.
  if (num_samples <= 0) return;
  if (sample_rate_ <= 0.0) {
    if (output != nullptr) std::fill_n(output, num_samples, 0.0f);
    return;
  }
  if (num_samples > max_block_size_) return;
  if (input == nullptr || output == nullptr) return;
  // Reuse the multi-channel path with channels=1 by staging the dry input in
  // the output buffer and passing that as the channel pointer. The
  // multi-channel path uses its own internal scratch_, so reading the
  // "dry" sample back from channels[0][i] still observes the original input
  // (the wet/dry mix would otherwise read the input-stage-processed signal).
  if (input != output) {
    std::copy_n(input, num_samples, output);
  }
  float* channel_ptr = output;
  process_block(&channel_ptr, 1, num_samples);
}

void RealtimeVoiceChanger::process_block(float* const* channels, int num_channels,
                                         int num_samples) noexcept {
  rt::ScopedNoDenormals no_denormals;
  // Pre-condition violations are silent no-ops; caller-owned planar buffers
  // are left untouched (we do not know their channel layout to safely zero).
  if (num_samples <= 0) return;
  if (sample_rate_ <= 0.0) return;
  if (channels == nullptr) return;
  if (num_channels < 1 || num_channels > num_channels_) return;
  if (num_samples > max_block_size_) return;
  ensure_scratch(num_samples);
  // Adopt the latest published configuration snapshot exactly once at block
  // start. After this point the per-sample loop reads from a stable const
  // reference; the control thread cannot mutate any field the loop touches
  // because set_config() only writes to config_ + publishes a NEW snapshot
  // (the audio thread keeps owning the previously-adopted one).
  const RealtimeVoiceChangerConfig& config = adopt_snapshot_for_block();
  for (int ch = 0; ch < num_channels; ++ch) {
    // Skip null channel pointers (caller's responsibility) rather than aborting
    // the whole block: a null right pointer must not leave the left output
    // buffer untouched / undefined.
    if (channels[ch] == nullptr) continue;
    auto& channel = channels_[static_cast<std::size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      scratch_[i] = process_input_stage(channel, config, channels[ch][i]);
    }
    channel.retune.process_block(scratch_.data(), scratch_.data(), num_samples);
    channel.formant.process_block(scratch_.data(), scratch_.data(), num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const float input = channels[ch][i];
      const float wet = process_output_stage(channel, config, scratch_[i]);
      channels[ch][i] = input * (1.0f - wet_mix_) + wet * wet_mix_;
    }
    // Final inter-sample-peak limiter — applied after the dry/wet mix so the
    // sample-domain limiter inside process_output_stage cannot create new ISP
    // overshoots by clamping a transient. Skipped when wet_mix_ == 0 because
    // the output equals the dry input (no DSP modification by the voice
    // changer, so no new ISP overshoots are possible) and applying the limiter
    // would introduce its lookahead latency to a signal the caller expects to
    // pass through unchanged. Any ISP overshoots in the caller's own dry
    // signal are the caller's responsibility.
    if (config.limiter.enable_isp_limiter && wet_mix_ > 0.0f) {
      channel.isp_limiter.process_block(channels[ch], num_samples);
    }
  }
}

int RealtimeVoiceChanger::latency_samples() const noexcept {
  if (channels_.empty()) return 0;
  // Retune grain dominates; biquad / formant group delays (<= 8 samples
  // combined) are intentionally not added so this stays a stable,
  // host-compensable integer. See header for details.
  return channels_[0].retune.grain_size();
}

RealtimeVoiceChangerConfig normalize_realtime_voice_changer_config(
    const RealtimeVoiceChangerConfig& input) {
  RealtimeVoiceChangerConfig c = input;
  // Replace any non-finite (NaN/Inf) value with a safe default before clamping
  // so std::clamp does not propagate NaN.
  auto sanitize = [](float& v, float fallback) {
    if (!std::isfinite(v)) v = fallback;
  };
  sanitize(c.input_gain_db, 0.0f);
  sanitize(c.output_gain_db, 0.0f);
  sanitize(c.wet_mix, 1.0f);
  sanitize(c.retune.semitones, 0.0f);
  sanitize(c.retune.mix, 1.0f);
  sanitize(c.formant.factor, 1.0f);
  sanitize(c.formant.amount, 1.0f);
  sanitize(c.formant.body, 0.0f);
  sanitize(c.formant.brightness, 0.0f);
  sanitize(c.formant.nasal, 0.0f);
  sanitize(c.eq.highpass_hz, 80.0f);
  sanitize(c.eq.body_db, 0.0f);
  sanitize(c.eq.presence_db, 0.0f);
  sanitize(c.eq.air_db, 0.0f);
  sanitize(c.gate.threshold_db, -55.0f);
  sanitize(c.gate.attack_ms, 2.0f);
  sanitize(c.gate.release_ms, 100.0f);
  sanitize(c.gate.range_db, 18.0f);
  sanitize(c.compressor.threshold_db, -22.0f);
  sanitize(c.compressor.ratio, 2.5f);
  sanitize(c.compressor.attack_ms, 6.0f);
  sanitize(c.compressor.release_ms, 90.0f);
  sanitize(c.compressor.makeup_gain_db, 1.0f);
  sanitize(c.deesser.frequency_hz, 7200.0f);
  sanitize(c.deesser.threshold_db, -28.0f);
  sanitize(c.deesser.ratio, 4.0f);
  sanitize(c.deesser.range_db, 8.0f);
  sanitize(c.reverb.mix, 0.04f);
  sanitize(c.reverb.time_ms, 320.0f);
  sanitize(c.reverb.damping, 0.55f);
  sanitize(c.limiter.ceiling_db, -1.0f);
  sanitize(c.limiter.release_ms, 50.0f);

  c.input_gain_db = std::clamp(c.input_gain_db, -24.0f, 24.0f);
  c.output_gain_db = std::clamp(c.output_gain_db, -36.0f, 12.0f);
  c.wet_mix = std::clamp(c.wet_mix, 0.0f, 1.0f);
  c.retune.semitones = std::clamp(c.retune.semitones, -24.0f, 24.0f);
  c.retune.mix = std::clamp(c.retune.mix, 0.0f, 1.0f);
  // Upper bound must agree with validate_dsp_section() so the POD set_config
  // path (which bypasses JSON validation) cannot allocate a runaway grain
  // buffer in StreamingRetune::prepare().
  c.retune.grain_size = std::clamp(c.retune.grain_size, 0, 8192);
  c.formant.factor = std::clamp(c.formant.factor, 0.55f, 1.65f);
  c.formant.amount = std::clamp(c.formant.amount, 0.0f, 1.0f);
  c.formant.body = std::clamp(c.formant.body, -1.0f, 1.0f);
  c.formant.brightness = std::clamp(c.formant.brightness, -1.0f, 1.0f);
  c.formant.nasal = std::clamp(c.formant.nasal, -1.0f, 1.0f);
  c.eq.highpass_hz = std::clamp(c.eq.highpass_hz, 20.0f, 300.0f);
  c.eq.body_db = std::clamp(c.eq.body_db, -12.0f, 12.0f);
  c.eq.presence_db = std::clamp(c.eq.presence_db, -12.0f, 12.0f);
  c.eq.air_db = std::clamp(c.eq.air_db, -12.0f, 12.0f);
  c.gate.threshold_db = std::clamp(c.gate.threshold_db, -90.0f, -12.0f);
  c.gate.attack_ms = std::clamp(c.gate.attack_ms, 0.05f, 100.0f);
  c.gate.release_ms = std::clamp(c.gate.release_ms, 5.0f, 1000.0f);
  c.gate.range_db = std::clamp(c.gate.range_db, 0.0f, 80.0f);
  c.compressor.threshold_db = std::clamp(c.compressor.threshold_db, -60.0f, 0.0f);
  c.compressor.ratio = std::clamp(c.compressor.ratio, 1.0f, 20.0f);
  c.compressor.attack_ms = std::clamp(c.compressor.attack_ms, 0.05f, 200.0f);
  c.compressor.release_ms = std::clamp(c.compressor.release_ms, 5.0f, 2000.0f);
  c.compressor.makeup_gain_db = std::clamp(c.compressor.makeup_gain_db, -12.0f, 12.0f);
  c.deesser.frequency_hz = std::clamp(c.deesser.frequency_hz, 3000.0f, 12000.0f);
  c.deesser.threshold_db = std::clamp(c.deesser.threshold_db, -60.0f, -6.0f);
  c.deesser.ratio = std::clamp(c.deesser.ratio, 1.0f, 20.0f);
  c.deesser.range_db = std::clamp(c.deesser.range_db, 0.0f, 24.0f);
  c.reverb.mix = std::clamp(c.reverb.mix, 0.0f, 0.45f);
  c.reverb.time_ms = std::clamp(c.reverb.time_ms, 40.0f, RealtimeVoiceChanger::kMaxReverbTimeMs);
  c.reverb.damping = std::clamp(c.reverb.damping, 0.0f, 1.0f);
  // See validate_dsp_section for the -1.0 dBFS ceiling rationale (inter-sample
  // headroom for a sample-domain limiter without 4x oversampling).
  c.limiter.ceiling_db = std::clamp(c.limiter.ceiling_db, -12.0f, -1.0f);
  c.limiter.release_ms = std::clamp(c.limiter.release_ms, 1.0f, 500.0f);
  return c;
}

bool validate_realtime_voice_changer_config(const RealtimeVoiceChangerConfig& config,
                                            RealtimeVoiceChangerConfig* normalized,
                                            std::string* error) {
  if (error) error->clear();
  std::string local_error;
  if (!config_is_finite(config, &local_error)) {
    if (error) *error = local_error;
    if (normalized) *normalized = {};
    return false;
  }
  if (normalized) *normalized = normalize_realtime_voice_changer_config(config);
  return true;
}

RealtimeVoiceChangerConfig realtime_voice_changer_preset(VoiceCharacterPreset preset) {
  RealtimeVoiceChangerConfig c;
  c.limiter.ceiling_db = -1.0f;
  switch (preset) {
    case VoiceCharacterPreset::NeutralMonitor:
      c.retune = {0.0f, 0.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.1f, 0.0f};
      c.eq = {75.0f, 0.0f, 1.2f, 0.5f};
      c.reverb.mix = 0.0f;
      c.compressor = {-24.0f, 1.8f, 8.0f, 120.0f, 0.5f};
      break;
    case VoiceCharacterPreset::BrightIdol:
      c.output_gain_db = 2.2f;
      c.retune = {4.0f, 1.0f, 0};
      // formant.body kept at 0 because the body-region EQ already drops 2.5 dB
      // around 180 Hz; doubling the cut here muddied the lower-mids audibly.
      c.formant = {1.18f, 1.0f, 0.0f, 0.7f, 0.05f};
      c.eq = {160.0f, -2.5f, 4.5f, 3.5f};
      c.gate = {-50.0f, 2.0f, 80.0f, 22.0f};
      c.compressor = {-23.0f, 3.2f, 4.5f, 75.0f, 2.0f};
      c.deesser = {7600.0f, -30.0f, 4.0f, 9.0f};
      c.reverb = {0.11f, 380.0f, 0.35f, 7};
      break;
    case VoiceCharacterPreset::SoftWhisper:
      c.output_gain_db = 3.0f;
      c.retune = {2.0f, 1.0f, 0};
      c.formant = {1.10f, 0.85f, 0.2f, 0.25f, -0.2f};
      c.eq = {110.0f, 1.0f, -2.0f, 2.5f};
      // Release shortened from 180→90 ms: whisper passages have short
      // inter-word gaps; the previous 180 ms kept the gate open so background
      // breath leaked between words.
      c.gate = {-62.0f, 6.0f, 90.0f, 10.0f};
      c.compressor = {-28.0f, 2.6f, 12.0f, 180.0f, 3.0f};
      c.deesser = {7000.0f, -32.0f, 4.5f, 10.0f};
      c.reverb = {0.09f, 520.0f, 0.65f, 11};
      break;
    case VoiceCharacterPreset::DeepNarrator:
      c.output_gain_db = 3.5f;
      c.retune = {-5.0f, 1.0f, 0};
      c.formant = {0.84f, 1.0f, 0.65f, -0.25f, -0.15f};
      c.eq = {70.0f, 4.0f, 2.0f, -1.5f};
      c.gate = {-52.0f, 2.5f, 120.0f, 18.0f};
      c.compressor = {-24.0f, 3.0f, 14.0f, 140.0f, 1.2f};
      c.deesser = {6500.0f, -26.0f, 3.5f, 5.0f};
      c.reverb = {0.08f, 650.0f, 0.8f, 13};
      break;
    case VoiceCharacterPreset::RobotMascot:
      c.output_gain_db = 4.0f;
      // Retune capped at +7 (from +9): grain-based pitch shift has no
      // anti-aliasing, so combining +9 with formant 1.35 and presence +6 dB
      // produced audible aliasing above ~5 kHz, especially at 22050 Hz.
      // factor lowered 1.35→1.30 for the same reason.
      c.retune = {7.0f, 1.0f, 0};
      c.formant = {1.30f, 1.0f, -0.6f, 0.75f, 0.35f};
      c.eq = {240.0f, -6.0f, 4.5f, 0.0f};
      c.gate = {-45.0f, 1.0f, 55.0f, 34.0f};
      c.compressor = {-26.0f, 5.5f, 2.0f, 55.0f, 2.5f};
      c.deesser = {8200.0f, -31.0f, 5.0f, 8.0f};
      c.reverb = {0.06f, 260.0f, 0.3f, 17};
      break;
    case VoiceCharacterPreset::DarkVillain:
      c.output_gain_db = 4.5f;
      c.retune = {-9.0f, 1.0f, 0};
      c.formant = {0.72f, 1.0f, 0.8f, -0.7f, -0.2f};
      // HPF raised 55→80 Hz to keep the long, dense reverb tail (820 ms,
      // 0.13 mix) from making sustained low-mid frequencies muddy. The dark
      // character is carried by formant+brightness shaping, not sub-bass.
      c.eq = {80.0f, 5.5f, -2.0f, -5.0f};
      c.gate = {-50.0f, 2.0f, 110.0f, 26.0f};
      c.compressor = {-25.0f, 4.2f, 8.0f, 120.0f, 1.0f};
      c.deesser = {6100.0f, -24.0f, 3.0f, 4.0f};
      c.reverb = {0.13f, 820.0f, 0.9f, 19};
      break;
  }
  return normalize_realtime_voice_changer_config(c);
}

const char* realtime_voice_changer_preset_id(VoiceCharacterPreset preset) noexcept {
  return preset_metadata(preset).id.data();
}

VoiceCharacterPreset realtime_voice_changer_preset_from_id(std::string_view id) {
  for (const auto& row : kPresetMetadata) {
    if (row.id == id) return row.preset;
  }
  throw SonareException(ErrorCode::InvalidParameter, "unknown realtime voice changer preset");
}

std::vector<std::string> realtime_voice_changer_preset_names() {
  std::vector<std::string> out;
  out.reserve(kPresetMetadata.size());
  for (const auto& row : kPresetMetadata) {
    out.emplace_back(row.id);
  }
  return out;
}

RealtimeVoiceChangerConfig realtime_voice_changer_config_from_json(std::string_view text) {
  const std::string input = trim_copy(text);
  if (input.empty()) return realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
  if (input.front() != '{')
    return realtime_voice_changer_preset(realtime_voice_changer_preset_from_id(input));

  const auto root = sonare::util::json::parse(input);
  RealtimeVoiceChangerConfig c;
  const auto* dsp = root.find("dsp");
  const auto& object = (dsp && dsp->is_object()) ? *dsp : root;
  c.input_gain_db = object_number(object, "inputGainDb", c.input_gain_db);
  c.output_gain_db = object_number(object, "outputGainDb", c.output_gain_db);
  c.wet_mix = object_number(object, "wetMix", c.wet_mix);

  if (const auto* v = object.find("retune")) {
    c.retune.semitones = object_number(*v, "semitones", c.retune.semitones);
    c.retune.mix = object_number(*v, "mix", c.retune.mix);
    c.retune.grain_size = object_int(*v, "grainSize", c.retune.grain_size);
  }
  if (const auto* v = object.find("formant")) {
    c.formant.factor = object_number(*v, "factor", c.formant.factor);
    c.formant.amount = object_number(*v, "amount", c.formant.amount);
    c.formant.body = object_number(*v, "body", c.formant.body);
    c.formant.brightness = object_number(*v, "brightness", c.formant.brightness);
    c.formant.nasal = object_number(*v, "nasal", c.formant.nasal);
  }
  if (const auto* v = object.find("eq")) {
    c.eq.highpass_hz = object_number(*v, "highpassHz", c.eq.highpass_hz);
    c.eq.body_db = object_number(*v, "bodyDb", c.eq.body_db);
    c.eq.presence_db = object_number(*v, "presenceDb", c.eq.presence_db);
    c.eq.air_db = object_number(*v, "airDb", c.eq.air_db);
  }
  if (const auto* v = object.find("gate")) {
    c.gate.threshold_db = object_number(*v, "thresholdDb", c.gate.threshold_db);
    c.gate.attack_ms = object_number(*v, "attackMs", c.gate.attack_ms);
    c.gate.release_ms = object_number(*v, "releaseMs", c.gate.release_ms);
    c.gate.range_db = object_number(*v, "rangeDb", c.gate.range_db);
  }
  if (const auto* v = object.find("compressor")) {
    c.compressor.threshold_db = object_number(*v, "thresholdDb", c.compressor.threshold_db);
    c.compressor.ratio = object_number(*v, "ratio", c.compressor.ratio);
    c.compressor.attack_ms = object_number(*v, "attackMs", c.compressor.attack_ms);
    c.compressor.release_ms = object_number(*v, "releaseMs", c.compressor.release_ms);
    c.compressor.makeup_gain_db = object_number(*v, "makeupGainDb", c.compressor.makeup_gain_db);
  }
  if (const auto* v = object.find("deesser")) {
    c.deesser.frequency_hz = object_number(*v, "frequencyHz", c.deesser.frequency_hz);
    c.deesser.threshold_db = object_number(*v, "thresholdDb", c.deesser.threshold_db);
    c.deesser.ratio = object_number(*v, "ratio", c.deesser.ratio);
    c.deesser.range_db = object_number(*v, "rangeDb", c.deesser.range_db);
  }
  if (const auto* v = object.find("reverb")) {
    c.reverb.mix = object_number(*v, "mix", c.reverb.mix);
    c.reverb.time_ms = object_number(*v, "timeMs", c.reverb.time_ms);
    c.reverb.damping = object_number(*v, "damping", c.reverb.damping);
    c.reverb.seed = object_int(*v, "seed", c.reverb.seed);
  }
  if (const auto* v = object.find("limiter")) {
    c.limiter.ceiling_db = object_number(*v, "ceilingDb", c.limiter.ceiling_db);
    c.limiter.release_ms = object_number(*v, "releaseMs", c.limiter.release_ms);
    c.limiter.enable_isp_limiter =
        object_bool(*v, "enableIspLimiter", c.limiter.enable_isp_limiter);
    c.limiter.isp_ceiling_dbtp = object_number(*v, "ispCeilingDbtp", c.limiter.isp_ceiling_dbtp);
  }
  return normalize_realtime_voice_changer_config(c);
}

std::string realtime_voice_changer_config_to_json(const RealtimeVoiceChangerConfig& config) {
  const auto c = normalize_realtime_voice_changer_config(config);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"schemaVersion\":" << kVoiceChangerPresetSchemaVersion << ",";
  dump_dsp_section(out, c);
  out << "}";
  return out.str();
}

std::string realtime_voice_changer_preset_json(VoiceCharacterPreset preset) {
  const auto c = realtime_voice_changer_preset(preset);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"schemaVersion\":" << kVoiceChangerPresetSchemaVersion << ",\"id\":\""
      << sonare::util::json::escape_string(realtime_voice_changer_preset_id(preset))
      << "\",\"name\":\""
      << sonare::util::json::escape_string(realtime_voice_changer_preset_display_name(preset))
      << "\",";
  dump_dsp_section(out, c);
  out << "}";
  return out.str();
}

bool validate_realtime_voice_changer_preset_json(std::string_view json,
                                                 std::string* normalized_json, std::string* error) {
  if (normalized_json) normalized_json->clear();
  if (error) error->clear();
  try {
    // Strict parse: a preset document with duplicate keys (e.g. two `"id"`
    // entries) is a user-config bug — fail fast rather than silently keeping
    // the last value. The lenient `realtime_voice_changer_config_from_json`
    // path below keeps using `parse` because it doubles as the realtime
    // C-API entry point and must stay maximally tolerant.
    const auto root = sonare::util::json::parse_strict(trim_copy(json));
    if (!has_allowed_keys(
            root, {"schemaVersion", "id", "name", "description", "category", "macros", "dsp"}, "$",
            error)) {
      return false;
    }
    for (const char* key : {"schemaVersion", "id", "name", "dsp"}) {
      if (!require_key(root, key, "$", error)) return false;
    }
    // schemaVersion: JSON-Schema "type: integer, const: 1" demands an
    // *exact* integer. as_int() silently truncates 1.5 -> 1, so check the
    // raw double for both integral-ness and equality before comparing.
    const auto* schema = root.find("schemaVersion");
    if (!schema || !schema->is_number()) {
      if (error) *error = "field is not a valid schemaVersion: $.schemaVersion (must be integer)";
      return false;
    }
    {
      const double n = schema->as_number();
      if (!std::isfinite(n) || std::floor(n) != n ||
          n != static_cast<double>(kVoiceChangerPresetSchemaVersion)) {
        if (error) {
          *error = "field is not a valid schemaVersion: $.schemaVersion (must be integer == " +
                   std::to_string(kVoiceChangerPresetSchemaVersion) + ")";
        }
        return false;
      }
    }
    // id: schema regex ^[a-z0-9][a-z0-9._-]*$ + length 1..96. The C++ validator
    // historically accepted any non-empty string up to 96 bytes, which let
    // upper-case / whitespace / punctuation slip through and break downstream
    // consumers that treat the id as a TS enum literal or comparison key.
    if (!require_string(root, "id", 1, 96, "$", error)) return false;
    if (const auto* id_value = root.find("id"); !is_valid_preset_id(id_value->as_string())) {
      if (error) *error = "field is not a valid preset id: $.id";
      return false;
    }
    if (!require_string(root, "name", 1, 96, "$", error)) return false;
    if (root.find("description") && !require_string(root, "description", 0, 512, "$", error)) {
      return false;
    }
    if (const auto* category = root.find("category")) {
      static constexpr std::array<std::string_view, 7> kCategories = {
          "monitor", "bright", "soft", "deep", "robot", "dark", "custom"};
      if (!category->is_string() || std::find(kCategories.begin(), kCategories.end(),
                                              category->as_string()) == kCategories.end()) {
        if (error) *error = "preset category is invalid";
        return false;
      }
    }
    if (const auto* macros = root.find("macros")) {
      if (!has_allowed_keys(
              *macros,
              {"pitch", "formant", "brightness", "space", "intensity", "noiseControl", "sibilance"},
              "$.macros", error)) {
        return false;
      }
      if (macros->find("pitch") &&
          !require_number(*macros, "pitch", -24.0, 24.0, "$.macros", error)) {
        return false;
      }
      if (macros->find("formant") &&
          !require_number(*macros, "formant", 0.55, 1.65, "$.macros", error)) {
        return false;
      }
      for (const char* key : {"brightness", "space", "intensity", "noiseControl", "sibilance"}) {
        if (macros->find(key) && !require_number(*macros, key, 0.0, 1.0, "$.macros", error)) {
          return false;
        }
      }
    }
    const auto* dsp = root.find("dsp");
    if (!dsp || !validate_dsp_section(*dsp, error)) return false;
    const auto config = realtime_voice_changer_config_from_json(trim_copy(json));
    if (normalized_json) *normalized_json = realtime_voice_changer_config_to_json(config);
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace sonare::editing::voice_changer
