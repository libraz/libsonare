#pragma once

/// @file synth_patch_common.h
/// @brief Shared C-ABI instrument config conversions used by project bounce and
///        realtime engine surfaces.

#include <sonare/sonare_c_project_instruments.h>
#include <sonare/sonare_c_types.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "c_api/c_ordinal.h"
#include "midi/builtin_synth.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#include "util/number_format.h"
#include "util/zero_is_default.h"

namespace sonare_c_detail {

/// Converts either C ABI built-in-synth config spelling to the core config.
/// The engine and project surfaces intentionally carry identical fields under
/// different public names; keeping this conversion templated leaves both ABI
/// structs independent while making their validation order and diagnostics one
/// implementation.
template <typename C>
inline sonare::midi::BuiltinSynthConfig builtin_synth_config_from_c(const C& c) {
  sonare::midi::BuiltinSynthConfig cfg;
  if (!ordinal_to_enum(c.waveform, 0, SONARE_SYNTH_WAVEFORM_COUNT - 1, &cfg.waveform)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "waveform is not a SONARE_SYNTH_WAVEFORM_* value");
  }
  // clamp_synth_config reads a non-positive or non-finite field as "use the
  // built-in default" and reports nothing, so a request that reached it came
  // back as a successful call at a level the caller never chose. Refused here,
  // where it is still visible; 0 stays the documented way to ask for the default.
  cfg.gain = sonare::ZeroIsDefault(c.gain).checked_non_negative(0.0f, "gain");
  cfg.attack_ms = sonare::ZeroIsDefault(c.attack_ms).checked_non_negative(0.0f, "attack_ms");
  cfg.decay_ms = sonare::ZeroIsDefault(c.decay_ms).checked_non_negative(0.0f, "decay_ms");
  cfg.sustain = sonare::ZeroIsDefault(c.sustain).checked_non_negative(0.0f, "sustain");
  cfg.release_ms = sonare::ZeroIsDefault(c.release_ms).checked_non_negative(0.0f, "release_ms");
  if (!(c.polyphony >= 0)) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "polyphony must be 0 (the library default) or a positive voice count");
  }
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

/// Highest struct_version either C ABI SF2 config spelling defines.
inline constexpr int kSf2InstrumentConfigVersion = 4;

/// Whether @p c carries a struct_version its spelling defines (0 and 1 are both
/// version 1). Shared by every entry that reads either spelling, so a negative or
/// future version is refused the same way everywhere.
template <typename C>
constexpr bool sf2_config_version_supported(const C& c) noexcept {
  return c.struct_version >= 0 && c.struct_version <= kSf2InstrumentConfigVersion;
}

/// Converts either C ABI SF2 config spelling to the core player config.
/// Version gates are kept here so engine and bounce bindings cannot drift;
/// callers check sf2_config_version_supported first and keep any
/// surface-specific offline options.
template <typename C>
inline sonare::midi::synth::Sf2PlayerConfig sf2_player_config_from_c(const C& c) {
  sonare::midi::synth::Sf2PlayerConfig cfg;
  // Passed through, the player's constructor would substitute for it in silence.
  cfg.gain = sonare::ZeroIsDefault(c.gain).checked_non_negative(cfg.gain, "gain");
  if (!(c.polyphony >= 0)) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "polyphony must be 0 (the library default) or a positive voice count");
  }
  if (c.polyphony != 0) cfg.polyphony = c.polyphony;
  if (c.struct_version >= 2) {
    cfg.prefer_model_for_modeled_families = c.prefer_model_for_modeled_families != 0;
  }
  if (c.struct_version >= 3 && c.clear_bank_rig != 0) cfg.bank_rig_binding = false;
  if (c.struct_version >= 4) {
    if (!(c.gs_efx_realization == 0 || c.gs_efx_realization == 1)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "gs_efx_realization must be 0 (modern) or 1 (classic)");
    }
    if (c.gs_efx_realization == 1) {
      cfg.gs_efx_realization = sonare::midi::synth::GsEfxRealization::kClassic;
    }
  }
  return cfg;
}

static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kSubtractive) + 1 ==
              SONARE_SYNTH_ENGINE_SUBTRACTIVE);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kFm) + 1 ==
              SONARE_SYNTH_ENGINE_FM);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kKarplusStrong) + 1 ==
              SONARE_SYNTH_ENGINE_KARPLUS_STRONG);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kModal) + 1 ==
              SONARE_SYNTH_ENGINE_MODAL);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kAdditive) + 1 ==
              SONARE_SYNTH_ENGINE_ADDITIVE);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kPercussion) + 1 ==
              SONARE_SYNTH_ENGINE_PERCUSSION);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kPiano) + 1 ==
              SONARE_SYNTH_ENGINE_PIANO);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kPipeOrgan) + 1 ==
              SONARE_SYNTH_ENGINE_PIPE_ORGAN);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kBowedString) + 1 ==
              SONARE_SYNTH_ENGINE_BOWED_STRING);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kReed) + 1 ==
              SONARE_SYNTH_ENGINE_REED);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kBrass) + 1 ==
              SONARE_SYNTH_ENGINE_BRASS);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kFlute) + 1 ==
              SONARE_SYNTH_ENGINE_FLUTE);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kPluckedString) + 1 ==
              SONARE_SYNTH_ENGINE_PLUCKED_STRING);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kVocal) + 1 ==
              SONARE_SYNTH_ENGINE_VOCAL);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kFreeReed) + 1 ==
              SONARE_SYNTH_ENGINE_FREE_REED);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kHarpsichord) + 1 ==
              SONARE_SYNTH_ENGINE_HARPSICHORD);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kSample) + 1 ==
              SONARE_SYNTH_ENGINE_SAMPLE);

static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kSine) + 1 ==
              SONARE_SYNTH_OSC_SINE);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kSaw) + 1 == SONARE_SYNTH_OSC_SAW);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kSquare) + 1 ==
              SONARE_SYNTH_OSC_SQUARE);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kTriangle) + 1 ==
              SONARE_SYNTH_OSC_TRIANGLE);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kNoise) + 1 ==
              SONARE_SYNTH_OSC_NOISE);

static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kSvf) + 1 ==
              SONARE_SYNTH_FILTER_SVF);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kTransistorLadder) + 1 ==
              SONARE_SYNTH_FILTER_TRANSISTOR_LADDER);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kDiodeLadder) + 1 ==
              SONARE_SYNTH_FILTER_DIODE_LADDER);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kSallenKey) + 1 ==
              SONARE_SYNTH_FILTER_SALLEN_KEY);

static_assert(static_cast<int>(sonare::midi::synth::SynthFilterOutput::kLowpass) + 1 ==
              SONARE_SYNTH_FILTER_OUT_LOWPASS);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterOutput::kBandpass) + 1 ==
              SONARE_SYNTH_FILTER_OUT_BANDPASS);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterOutput::kHighpass) + 1 ==
              SONARE_SYNTH_FILTER_OUT_HIGHPASS);

static_assert(static_cast<int>(sonare::midi::synth::BodyType::kNone) + 1 == SONARE_SYNTH_BODY_NONE);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kGuitar) + 1 ==
              SONARE_SYNTH_BODY_GUITAR);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kViolin) + 1 ==
              SONARE_SYNTH_BODY_VIOLIN);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kWoodTube) + 1 ==
              SONARE_SYNTH_BODY_WOOD_TUBE);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kBrassBell) + 1 ==
              SONARE_SYNTH_BODY_BRASS_BELL);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kVocal) + 1 ==
              SONARE_SYNTH_BODY_VOCAL);

static_assert(static_cast<int>(sonare::midi::synth::SynthRetrigger::kFree) + 1 ==
              SONARE_SYNTH_RETRIGGER_FREE);
static_assert(static_cast<int>(sonare::midi::synth::SynthRetrigger::kNote) + 1 ==
              SONARE_SYNTH_RETRIGGER_NOTE);

static_assert(static_cast<int>(sonare::midi::synth::LfoShape::kTriangle) + 1 ==
              SONARE_SYNTH_LFO_SHAPE_TRIANGLE);
static_assert(static_cast<int>(sonare::midi::synth::LfoShape::kSine) + 1 ==
              SONARE_SYNTH_LFO_SHAPE_SINE);

static_assert(static_cast<int>(sonare::midi::synth::ModSource::kPitchBend) + 1 ==
              SONARE_SYNTH_MOD_SOURCE_COUNT);
static_assert(static_cast<int>(sonare::midi::synth::ModDestination::kSpectrumMorph) + 1 ==
              SONARE_SYNTH_MOD_DESTINATION_COUNT);

/// The built-in synth's oscillator waveform ordinal, shared by the project
/// bounce and realtime-engine entry points so the two cannot answer differently
/// for the same value. Unlike the surrounding numeric fields there is no
/// nearest value to clamp an unknown ordinal to, so it is refused.
inline bool valid_builtin_waveform(int value) noexcept {
  return valid_c_enum(value, SONARE_SYNTH_WAVEFORM_COUNT);
}

/// Storage for a refusal message composed from the caller's values; valid
/// until the next refusal on the same thread.
inline std::string& synth_patch_error_message() {
  static thread_local std::string message;
  return message;
}

/// Refuses @p value for the patch field @p key unless it is finite and in [lo, hi].
inline bool synth_patch_field_in_range(const char* key, float value, float lo, float hi,
                                       const char** out_error) {
  if (std::isfinite(value) && value >= lo && value <= hi) return true;
  std::string& message = synth_patch_error_message();
  message = std::string("synth patch field '") + key + "': ";
  if (std::isfinite(value)) {
    message +=
        "out of range [" + sonare::util::to_text(lo) + ", " + sonare::util::to_text(hi) + "]";
  } else {
    message += "not a finite number";
  }
  if (out_error) *out_error = message.c_str();
  return false;
}

/// The automation table's range check for the patch field behind @p id.
inline bool synth_patch_param_in_range(sonare::midi::synth::NativeSynthParamId id, float value,
                                       const char** out_error) {
  const char* key = "";
  float lo = 0.0f;
  float hi = 0.0f;
  sonare::midi::synth::native_synth_param_range(id, &key, &lo, &hi);
  return synth_patch_field_in_range(key, value, lo, hi, out_error);
}

/// The sample-engine level and start-offset ranges, read back from the patch clamp's extremes.
struct SynthPatchSampleBounds {
  float level_lo;
  float level_hi;
  float start_offset_lo;
  float start_offset_hi;
};

inline const SynthPatchSampleBounds& synth_patch_sample_bounds() {
  static const SynthPatchSampleBounds bounds = [] {
    const auto extreme = [](float value) {
      sonare::midi::synth::NativeSynthPatch p = sonare::midi::synth::NativeSynthConfig{}.patch;
      p.sample.level = value;
      p.sample.start_offset01 = value;
      return sonare::midi::synth::clamp_synth_patch(p);
    };
    const auto low = extreme(-std::numeric_limits<float>::max());
    const auto high = extreme(std::numeric_limits<float>::max());
    return SynthPatchSampleBounds{low.sample.level, high.sample.level, low.sample.start_offset01,
                                  high.sample.start_offset01};
  }();
  return bounds;
}

/// The mod-route depth bound, read back from the patch clamp's extreme.
inline float synth_patch_mod_depth_limit() {
  static const float limit = [] {
    sonare::midi::synth::NativeSynthPatch p = sonare::midi::synth::NativeSynthConfig{}.patch;
    p.mod_matrix.routes[0].depth = std::numeric_limits<float>::max();
    return sonare::midi::synth::clamp_synth_patch(p).mod_matrix.routes[0].depth;
  }();
  return limit;
}

/// Copies the engine section @p mode reads from @p from into @p to, leaving
/// every other section of @p to as it was. Modes without a section copy nothing.
inline void copy_engine_section(const sonare::midi::synth::NativeSynthPatch& from,
                                sonare::midi::synth::SynthEngineMode mode,
                                sonare::midi::synth::NativeSynthPatch* to) {
  using sonare::midi::synth::SynthEngineMode;
  switch (mode) {
    case SynthEngineMode::kFm:
      to->fm = from.fm;
      break;
    case SynthEngineMode::kKarplusStrong:
      to->ks = from.ks;
      break;
    case SynthEngineMode::kModal:
      to->modal = from.modal;
      break;
    case SynthEngineMode::kAdditive:
      to->additive = from.additive;
      break;
    case SynthEngineMode::kPercussion:
      to->percussion = from.percussion;
      break;
    case SynthEngineMode::kPiano:
      to->piano = from.piano;
      break;
    case SynthEngineMode::kPipeOrgan:
      to->pipe_organ = from.pipe_organ;
      break;
    case SynthEngineMode::kBowedString:
      to->bowed_string = from.bowed_string;
      break;
    case SynthEngineMode::kReed:
      to->reed = from.reed;
      break;
    case SynthEngineMode::kBrass:
      to->brass = from.brass;
      break;
    case SynthEngineMode::kFlute:
      to->flute = from.flute;
      break;
    case SynthEngineMode::kPluckedString:
      to->plucked_string = from.plucked_string;
      break;
    case SynthEngineMode::kVocal:
      to->vocal = from.vocal;
      break;
    case SynthEngineMode::kFreeReed:
      to->free_reed = from.free_reed;
      break;
    case SynthEngineMode::kHarpsichord:
      to->harpsichord = from.harpsichord;
      break;
    case SynthEngineMode::kSubtractive:
    case SynthEngineMode::kSample:
      break;
  }
}

/// Resolves a versioned C synth patch onto a NativeSynthConfig. The base is
/// the named preset (or the default subtractive patch when @p c.preset is
/// empty). The engine section is the named preset's when the preset uses the
/// selected engine, else that engine's base preset's (@ref base_preset_name).
/// Every non-zero struct field then overrides the base ("0 => keep"), and the
/// engine params assign fields of the selected engine's section by key.
/// Struct version 2 adds @c present_fields, so a caller can also override with
/// an explicit zero; version 1 has no presence bits and cannot express one.
/// Version 3 adds the sample-engine block, which only a sample patch reads.
/// Version 4 adds the series highpass, 5 the converter, 6 the pitch offset, 7
/// the retrigger mode, 8 the engine params, 9 the second presence word and its
/// fields and 10 the LFO shape; an older caller's tail is never read. Returns false (and sets @p
/// out_error, valid until the next refusal on this thread) for an unsupported struct_version, an
/// unknown preset name, a refused engine param, a bounded numeric field that is non-finite or
/// outside the range sonare_synth_patch_param_info() publishes for it, an enum ordinal outside its
/// set, a mod routing with an unknown source or destination or a depth that is non-finite or beyond
/// the patch clamp, or more mod routings than the matrix holds; nothing is clamped silently at that
/// step.
inline bool synth_config_from_patch_c(const SonareSynthPatch& c,
                                      sonare::midi::synth::NativeSynthConfig* out,
                                      const char** out_error) {
  using sonare::midi::synth::BodyType;
  using sonare::midi::synth::find_synth_preset;
  using sonare::midi::synth::kMaxModRoutes;
  using sonare::midi::synth::LfoShape;
  using sonare::midi::synth::NativeSynthConfig;
  using ParamId = sonare::midi::synth::NativeSynthParamId;
  using sonare::midi::synth::SynthEngineMode;
  using sonare::midi::synth::SynthFilterModel;
  using sonare::midi::synth::SynthFilterOutput;
  using sonare::midi::synth::SynthRetrigger;
  using sonare::midi::synth::VaWaveform;

  if (out_error) *out_error = nullptr;
  // A field is applied when its presence bit is set or it is non-zero (NaN included);
  // an applied value outside the automation table's range is refused.
  const auto take = [out_error](bool given, ParamId id, float value, float* dst) {
    if (!given && value == 0.0f) return true;
    if (!synth_patch_param_in_range(id, value, out_error)) return false;
    *dst = value;
    return true;
  };
  if (c.struct_version < 0 || c.struct_version > SONARE_SYNTH_PATCH_STRUCT_VERSION) {
    if (out_error) *out_error = "unsupported SonareSynthPatch struct_version";
    return false;
  }
  // A version-1 caller's struct predates present_fields, so its bits are not
  // part of that contract even though the member exists in this build.
  const uint32_t present = c.struct_version >= 2 ? c.present_fields : 0u;
  const auto set = [present](uint32_t bit) { return (present & bit) != 0u; };
  // A pre-9 struct ends at engine_param_count, so the second word is not read.
  const uint32_t present2 = c.struct_version >= 9 ? c.present_fields2 : 0u;
  const auto set2 = [present2](uint32_t bit) { return (present2 & bit) != 0u; };
  if (!valid_c_enum(c.engine_mode, SONARE_SYNTH_ENGINE_MODE_COUNT)) {
    if (out_error) *out_error = "invalid synth engine_mode";
    return false;
  }
  if (!valid_c_enum(c.waveform, SONARE_SYNTH_OSC_WAVEFORM_COUNT)) {
    if (out_error) *out_error = "invalid synth waveform";
    return false;
  }
  if (!valid_c_enum(c.filter_model, SONARE_SYNTH_FILTER_MODEL_COUNT)) {
    if (out_error) *out_error = "invalid synth filter_model";
    return false;
  }
  if (!valid_c_enum(c.filter_output, SONARE_SYNTH_FILTER_OUTPUT_COUNT)) {
    if (out_error) *out_error = "invalid synth filter_output";
    return false;
  }
  if (!valid_c_enum(c.body, SONARE_SYNTH_BODY_TYPE_COUNT)) {
    if (out_error) *out_error = "invalid synth body";
    return false;
  }
  // A pre-7 struct has no retrigger member, so its tail bytes are not read.
  const int retrigger = c.struct_version >= 7 ? c.retrigger : SONARE_SYNTH_RETRIGGER_BASE;
  if (!valid_c_enum(retrigger, SONARE_SYNTH_RETRIGGER_COUNT)) {
    if (out_error) *out_error = "invalid synth retrigger";
    return false;
  }
  // A pre-10 struct has no lfo_shape member, so its tail bytes are not read.
  const int lfo_shape = c.struct_version >= 10 ? c.lfo_shape : SONARE_SYNTH_LFO_SHAPE_BASE;
  if (!valid_c_enum(lfo_shape, SONARE_SYNTH_LFO_SHAPE_COUNT)) {
    if (out_error) *out_error = "invalid synth lfo_shape";
    return false;
  }
  // A pre-3 struct ends before the sample block; any engine refuses an unknown ordinal there.
  if (c.struct_version >= 3 && !valid_c_enum(c.sample_loop, SONARE_SAMPLE_LOOP_MODE_COUNT)) {
    if (out_error) *out_error = "invalid synth sample_loop";
    return false;
  }
  if (c.struct_version >= 3 && !valid_c_enum(c.sample_key_track, SONARE_SAMPLE_KEY_TRACK_COUNT)) {
    if (out_error) *out_error = "invalid synth sample_key_track";
    return false;
  }
  // A pre-8 struct ends before the engine params, so its tail is not read.
  const size_t engine_param_count = c.struct_version >= 8 ? c.engine_param_count : 0u;
  if (engine_param_count > 0 && c.engine_params == nullptr) {
    if (out_error) *out_error = "engine_params is NULL with a non-zero engine_param_count";
    return false;
  }
  NativeSynthConfig cfg;
  const sonare::midi::synth::SynthPreset* named = nullptr;
  if (c.preset[0] != '\0') {
    // Defensive copy: the fixed field may legally lack a terminator.
    char name[SONARE_SYNTH_PRESET_NAME_MAX + 1] = {};
    std::memcpy(name, c.preset, SONARE_SYNTH_PRESET_NAME_MAX);
    named = find_synth_preset(name);
    if (named == nullptr) {
      if (out_error) *out_error = "unknown synth preset name";
      return false;
    }
    cfg = named->config;
  }
  sonare::midi::synth::NativeSynthPatch& p = cfg.patch;

  if (c.engine_mode > 0) p.mode = admitted_ordinal_to_enum<SynthEngineMode>(c.engine_mode, 1);
  // An engine whose section the named preset does not carry takes its base
  // preset's; the wrapper fields stay the named preset's (or the init patch's).
  if (named == nullptr || named->config.patch.mode != p.mode) {
    if (const char* base_name = sonare::midi::synth::base_preset_name(p.mode)) {
      const sonare::midi::synth::SynthPreset* base = find_synth_preset(base_name);
      if (base != nullptr) copy_engine_section(base->config.patch, p.mode, &p);
    }
  }
  // Oscillator section.
  if (c.waveform > 0) p.waveform = admitted_ordinal_to_enum<VaWaveform>(c.waveform, 1);
  if (set(SONARE_SYNTH_FIELD_UNISON) || c.unison != 0) {
    if (!synth_patch_field_in_range("unison", static_cast<float>(c.unison), 1.0f,
                                    static_cast<float>(sonare::midi::synth::kMaxUnisonOscs),
                                    out_error)) {
      return false;
    }
    p.unison = c.unison;
  }
  if (!take(set(SONARE_SYNTH_FIELD_DETUNE_CENTS), ParamId::kDetuneCents, c.detune_cents,
            &p.detune_cents) ||
      !take(set(SONARE_SYNTH_FIELD_DRIFT_CENTS), ParamId::kDriftCents, c.drift_cents,
            &p.drift_cents) ||
      !take(set(SONARE_SYNTH_FIELD_DRIVE), ParamId::kDrive, c.drive, &p.drive)) {
    return false;
  }
  // Filter section.
  if (c.filter_model > 0) {
    p.filter_model = admitted_ordinal_to_enum<SynthFilterModel>(c.filter_model, 1);
  }
  if (c.filter_output > 0) {
    p.filter_output = admitted_ordinal_to_enum<SynthFilterOutput>(c.filter_output, 1);
  }
  if (!take(set(SONARE_SYNTH_FIELD_CUTOFF_HZ), ParamId::kCutoffHz, c.cutoff_hz, &p.cutoff_hz) ||
      !take(set(SONARE_SYNTH_FIELD_RESONANCE_Q), ParamId::kResonanceQ, c.resonance_q,
            &p.resonance_q) ||
      !take(set(SONARE_SYNTH_FIELD_KEY_TRACK), ParamId::kKeyTrack, c.key_track, &p.key_track) ||
      !take(set(SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS), ParamId::kEnvToCutoffCents,
            c.env_to_cutoff_cents, &p.env_to_cutoff_cents) ||
      !take(set(SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS), ParamId::kVelToCutoffCents,
            c.vel_to_cutoff_cents, &p.vel_to_cutoff_cents)) {
    return false;
  }
  // Envelopes.
  if (!take(set(SONARE_SYNTH_FIELD_AMP_ATTACK_MS), ParamId::kAmpAttackMs, c.amp_attack_ms,
            &p.amp_env.attack_ms) ||
      !take(set(SONARE_SYNTH_FIELD_AMP_DECAY_MS), ParamId::kAmpDecayMs, c.amp_decay_ms,
            &p.amp_env.decay_ms) ||
      !take(set(SONARE_SYNTH_FIELD_AMP_SUSTAIN), ParamId::kAmpSustain, c.amp_sustain,
            &p.amp_env.sustain) ||
      !take(set(SONARE_SYNTH_FIELD_AMP_RELEASE_MS), ParamId::kAmpReleaseMs, c.amp_release_ms,
            &p.amp_env.release_ms) ||
      !take(set(SONARE_SYNTH_FIELD_FILTER_ATTACK_MS), ParamId::kFilterAttackMs, c.filter_attack_ms,
            &p.filter_env.attack_ms) ||
      !take(set(SONARE_SYNTH_FIELD_FILTER_DECAY_MS), ParamId::kFilterDecayMs, c.filter_decay_ms,
            &p.filter_env.decay_ms) ||
      !take(set(SONARE_SYNTH_FIELD_FILTER_SUSTAIN), ParamId::kFilterSustain, c.filter_sustain,
            &p.filter_env.sustain) ||
      !take(set(SONARE_SYNTH_FIELD_FILTER_RELEASE_MS), ParamId::kFilterReleaseMs,
            c.filter_release_ms, &p.filter_env.release_ms)) {
    return false;
  }
  // LFOs / glide.
  if (!take(set(SONARE_SYNTH_FIELD_LFO_RATE_HZ), ParamId::kLfoRateHz, c.lfo_rate_hz,
            &p.lfo_rate_hz) ||
      !take(set(SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS), ParamId::kLfoToPitchCents,
            c.lfo_to_pitch_cents, &p.lfo_to_pitch_cents) ||
      !take(set(SONARE_SYNTH_FIELD_LFO2_RATE_HZ), ParamId::kLfo2RateHz, c.lfo2_rate_hz,
            &p.lfo2_rate_hz) ||
      !take(set(SONARE_SYNTH_FIELD_GLIDE_MS), ParamId::kGlideMs, c.glide_ms, &p.glide_ms)) {
    return false;
  }
  // Realism polish.
  if (c.body > 0) p.body = admitted_ordinal_to_enum<BodyType>(c.body, 1);
  if (!take(set(SONARE_SYNTH_FIELD_BODY_MIX), ParamId::kBodyMix, c.body_mix, &p.body_mix) ||
      !take(set(SONARE_SYNTH_FIELD_STEREO_SPREAD), ParamId::kStereoSpread, c.stereo_spread,
            &p.stereo_spread)) {
    return false;
  }
  // Mod matrix: a non-empty C table replaces the base matrix, and the presence
  // bit lets an empty table clear it instead of keeping the base.
  if (!synth_patch_field_in_range("modRoutings", static_cast<float>(c.num_mod_routings), 0.0f,
                                  static_cast<float>(kMaxModRoutes), out_error)) {
    return false;
  }
  if (c.num_mod_routings > 0 || set(SONARE_SYNTH_FIELD_MOD_ROUTINGS)) {
    p.mod_matrix = {};
    const int count = c.num_mod_routings;
    for (int i = 0; i < count; ++i) {
      const SonareSynthModRouting& r = c.mod_routings[i];
      auto& route = p.mod_matrix.routes[static_cast<size_t>(i)];
      // Refused like ControllerBinding's axis none: a caller mistake, not an empty slot.
      if (r.source == 0 || r.destination == 0) {
        if (out_error) *out_error = "mod routing refused: source and destination must not be none";
        return false;
      }
      if (!ordinal_to_enum(r.source, 1, SONARE_SYNTH_MOD_SOURCE_COUNT - 1, &route.source) ||
          !ordinal_to_enum(r.destination, 1, SONARE_SYNTH_MOD_DESTINATION_COUNT - 1,
                           &route.destination)) {
        if (out_error) *out_error = "mod routing refused: unknown source or destination ordinal";
        return false;
      }
      const float depth_limit = synth_patch_mod_depth_limit();
      if (!synth_patch_field_in_range("modDepth", r.depth, -depth_limit, depth_limit, out_error)) {
        return false;
      }
      route.depth = r.depth;
    }
  }
  // Voice pool / bus.
  if (!take(set(SONARE_SYNTH_FIELD_GAIN), ParamId::kGain, c.gain, &cfg.gain)) return false;
  if (set(SONARE_SYNTH_FIELD_POLYPHONY) || c.polyphony != 0) {
    if (!synth_patch_field_in_range("polyphony", static_cast<float>(c.polyphony), 1.0f,
                                    static_cast<float>(sonare::midi::kMaxSynthVoices), out_error)) {
      return false;
    }
    cfg.polyphony = c.polyphony;
  }
  if (!take(set(SONARE_SYNTH_FIELD_BUS_DRIVE), ParamId::kBusDrive, c.bus_drive, &cfg.bus_drive)) {
    return false;
  }

  // Sample engine: only a version-3 caller's struct reaches this block, and
  // only a sample patch reads it, which is what keeps set 0 addressable
  // without a presence bit of its own.
  if (c.struct_version >= 4 && !take(set(SONARE_SYNTH_FIELD_HP_CUTOFF_HZ), ParamId::kHpCutoffHz,
                                     c.hp_cutoff_hz, &p.hp_cutoff_hz)) {
    return false;
  }
  if (c.struct_version >= 5 &&
      (!take(set(SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ), ParamId::kSampleHoldHz, c.sample_hold_hz,
             &p.sample_hold_hz) ||
       !take(set(SONARE_SYNTH_FIELD_BIT_DEPTH), ParamId::kBitDepth, c.bit_depth, &p.bit_depth))) {
    return false;
  }
  if (c.struct_version >= 6 &&
      !take(set(SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS), ParamId::kPitchOffsetCents,
            c.pitch_offset_cents, &p.pitch_offset_cents)) {
    return false;
  }
  if (retrigger > 0) p.retrigger = admitted_ordinal_to_enum<SynthRetrigger>(retrigger, 1);
  if (lfo_shape > 0) p.lfo_shape = admitted_ordinal_to_enum<LfoShape>(lfo_shape, 1);
  if (c.struct_version >= 9) {
    if (!take(set2(SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ), ParamId::kDriftRateHz, c.drift_rate_hz,
              &p.drift_rate_hz) ||
        !take(set2(SONARE_SYNTH_FIELD2_AMP_DELAY_MS), ParamId::kAmpDelayMs, c.amp_delay_ms,
              &p.amp_env.delay_ms) ||
        !take(set2(SONARE_SYNTH_FIELD2_AMP_HOLD_MS), ParamId::kAmpHoldMs, c.amp_hold_ms,
              &p.amp_env.hold_ms) ||
        !take(set2(SONARE_SYNTH_FIELD2_FILTER_DELAY_MS), ParamId::kFilterDelayMs, c.filter_delay_ms,
              &p.filter_env.delay_ms) ||
        !take(set2(SONARE_SYNTH_FIELD2_FILTER_HOLD_MS), ParamId::kFilterHoldMs, c.filter_hold_ms,
              &p.filter_env.hold_ms)) {
      return false;
    }
    if (set2(SONARE_SYNTH_FIELD2_ONE_SHOT) || c.one_shot != 0) p.one_shot = c.one_shot != 0;
  }
  if (engine_param_count > 0) {
    std::vector<sonare::midi::synth::EngineParam> params(engine_param_count);
    for (size_t i = 0; i < engine_param_count; ++i) {
      params[i] = {c.engine_params[i].key, c.engine_params[i].value};
    }
    std::string& message = synth_patch_error_message();
    message.clear();
    if (!sonare::midi::synth::apply_engine_params(p, params.data(), params.size(), &message)) {
      if (out_error) *out_error = message.c_str();
      return false;
    }
  }
  if (c.struct_version >= 3 && p.mode == SynthEngineMode::kSample) {
    p.sample.set_index = c.sample_set;
    const SynthPatchSampleBounds& sample_bounds = synth_patch_sample_bounds();
    if (c.sample_level != 0.0f) {
      if (!synth_patch_field_in_range("sampleLevel", c.sample_level, sample_bounds.level_lo,
                                      sample_bounds.level_hi, out_error)) {
        return false;
      }
      p.sample.level = c.sample_level;
    }
    if (c.sample_start_offset != 0.0f) {
      if (!synth_patch_field_in_range("sampleStartOffset", c.sample_start_offset,
                                      sample_bounds.start_offset_lo, sample_bounds.start_offset_hi,
                                      out_error)) {
        return false;
      }
      p.sample.start_offset01 = c.sample_start_offset;
    }
    // The override holds the SoundFont sampleModes value, so the C ordinal is mapped.
    switch (admitted_ordinal_to_enum<SonareSampleLoopMode>(c.sample_loop)) {
      case SONARE_SAMPLE_LOOP_DEFAULT:
        break;
      case SONARE_SAMPLE_LOOP_NONE:
        p.sample.loop_override = 0;
        break;
      case SONARE_SAMPLE_LOOP_CONTINUOUS:
        p.sample.loop_override = 1;
        break;
      case SONARE_SAMPLE_LOOP_KEY_DOWN:
        p.sample.loop_override = 3;
        break;
    }
    if (c.sample_key_track == SONARE_SAMPLE_KEY_TRACK_ON) p.sample.key_track = true;
    if (c.sample_key_track == SONARE_SAMPLE_KEY_TRACK_OFF) p.sample.key_track = false;
  }

  *out = cfg;
  return true;
}

/// Every presence bit SonareSynthPatch defines. The read direction sets them
/// all: a preset snapshot is fully specified, so a field that happens to be
/// zero must survive a round-trip instead of decaying into "keep base".
inline constexpr uint32_t kSynthPatchAllFields =
    SONARE_SYNTH_FIELD_UNISON | SONARE_SYNTH_FIELD_DETUNE_CENTS | SONARE_SYNTH_FIELD_DRIFT_CENTS |
    SONARE_SYNTH_FIELD_DRIVE | SONARE_SYNTH_FIELD_CUTOFF_HZ | SONARE_SYNTH_FIELD_RESONANCE_Q |
    SONARE_SYNTH_FIELD_KEY_TRACK | SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS |
    SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS | SONARE_SYNTH_FIELD_AMP_ATTACK_MS |
    SONARE_SYNTH_FIELD_AMP_DECAY_MS | SONARE_SYNTH_FIELD_AMP_SUSTAIN |
    SONARE_SYNTH_FIELD_AMP_RELEASE_MS | SONARE_SYNTH_FIELD_FILTER_ATTACK_MS |
    SONARE_SYNTH_FIELD_FILTER_DECAY_MS | SONARE_SYNTH_FIELD_FILTER_SUSTAIN |
    SONARE_SYNTH_FIELD_FILTER_RELEASE_MS | SONARE_SYNTH_FIELD_LFO_RATE_HZ |
    SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS | SONARE_SYNTH_FIELD_LFO2_RATE_HZ |
    SONARE_SYNTH_FIELD_GLIDE_MS | SONARE_SYNTH_FIELD_BODY_MIX | SONARE_SYNTH_FIELD_STEREO_SPREAD |
    SONARE_SYNTH_FIELD_GAIN | SONARE_SYNTH_FIELD_POLYPHONY | SONARE_SYNTH_FIELD_BUS_DRIVE |
    SONARE_SYNTH_FIELD_MOD_ROUTINGS | SONARE_SYNTH_FIELD_HP_CUTOFF_HZ |
    SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ | SONARE_SYNTH_FIELD_BIT_DEPTH |
    SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS;

/// Every presence bit of SonareSynthPatch.present_fields2, set by the read
/// direction for the same reason.
inline constexpr uint32_t kSynthPatchAllFields2 =
    SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ | SONARE_SYNTH_FIELD2_AMP_DELAY_MS |
    SONARE_SYNTH_FIELD2_AMP_HOLD_MS | SONARE_SYNTH_FIELD2_FILTER_DELAY_MS |
    SONARE_SYNTH_FIELD2_FILTER_HOLD_MS | SONARE_SYNTH_FIELD2_ONE_SHOT;

/// Fills a versioned C synth patch from a catalog preset (the read direction:
/// preset name + the wrapper-section values, so hosts can inspect and tweak).
inline void synth_patch_to_c(const sonare::midi::synth::SynthPreset& preset,
                             SonareSynthPatch* out) {
  const sonare::midi::synth::NativeSynthPatch& p = preset.config.patch;
  *out = SonareSynthPatch{};
  out->struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  out->present_fields = kSynthPatchAllFields;
  out->present_fields2 = kSynthPatchAllFields2;
  std::strncpy(out->preset, preset.name, SONARE_SYNTH_PRESET_NAME_MAX - 1);
  out->engine_mode = static_cast<int>(p.mode) + 1;
  out->waveform = static_cast<int>(p.waveform) + 1;
  out->unison = p.unison;
  out->detune_cents = p.detune_cents;
  out->drift_cents = p.drift_cents;
  out->drive = p.drive;
  out->filter_model = static_cast<int>(p.filter_model) + 1;
  out->filter_output = static_cast<int>(p.filter_output) + 1;
  out->cutoff_hz = p.cutoff_hz;
  out->hp_cutoff_hz = p.hp_cutoff_hz;
  out->sample_hold_hz = p.sample_hold_hz;
  out->bit_depth = p.bit_depth;
  out->pitch_offset_cents = p.pitch_offset_cents;
  out->resonance_q = p.resonance_q;
  out->key_track = p.key_track;
  out->env_to_cutoff_cents = p.env_to_cutoff_cents;
  out->vel_to_cutoff_cents = p.vel_to_cutoff_cents;
  out->amp_attack_ms = p.amp_env.attack_ms;
  out->amp_decay_ms = p.amp_env.decay_ms;
  out->amp_sustain = p.amp_env.sustain;
  out->amp_release_ms = p.amp_env.release_ms;
  out->filter_attack_ms = p.filter_env.attack_ms;
  out->filter_decay_ms = p.filter_env.decay_ms;
  out->filter_sustain = p.filter_env.sustain;
  out->filter_release_ms = p.filter_env.release_ms;
  out->lfo_rate_hz = p.lfo_rate_hz;
  out->lfo_to_pitch_cents = p.lfo_to_pitch_cents;
  out->lfo2_rate_hz = p.lfo2_rate_hz;
  out->glide_ms = p.glide_ms;
  out->body = static_cast<int>(p.body) + 1;
  out->body_mix = p.body_mix;
  out->stereo_spread = p.stereo_spread;
  out->retrigger = static_cast<int>(p.retrigger) + 1;
  out->drift_rate_hz = p.drift_rate_hz;
  out->amp_delay_ms = p.amp_env.delay_ms;
  out->amp_hold_ms = p.amp_env.hold_ms;
  out->filter_delay_ms = p.filter_env.delay_ms;
  out->filter_hold_ms = p.filter_env.hold_ms;
  out->one_shot = p.one_shot ? 1 : 0;
  out->lfo_shape = static_cast<int>(p.lfo_shape) + 1;
  int n = 0;
  for (const sonare::midi::synth::ModRoute& r : p.mod_matrix.routes) {
    if (r.source == sonare::midi::synth::ModSource::kNone ||
        r.destination == sonare::midi::synth::ModDestination::kNone || r.depth == 0.0f) {
      continue;
    }
    out->mod_routings[n] = {static_cast<int>(r.source), static_cast<int>(r.destination), r.depth};
    ++n;
  }
  out->num_mod_routings = n;
  out->gain = preset.config.gain;
  out->polyphony = preset.config.polyphony;
  out->bus_drive = preset.config.bus_drive;
}

}  // namespace sonare_c_detail
