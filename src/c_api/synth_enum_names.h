#pragma once

/// @file synth_enum_names.h
/// @brief The spelling of every synth enum value the C ABI names: the one
///        table @ref sonare_synth_enum_names serves and the Node addon and the
///        WASM bindings read names through, so no surface keeps its own copy.
///        Index i of a table is the name of C ordinal i, except that
///        kBuiltinWaveforms lists "sawtooth" beside "saw" as the catalogue
///        does. Input-only spellings live in kInputAliases, apart from the
///        canonical names, so they are accepted but never listed or returned.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project_part_rig.h>

#include <cstddef>
#include <iterator>
#include <string_view>

#include "midi/part_rig.h"

namespace sonare::c_api::synth_names {

inline constexpr const char* kEngineModes[] = {
    "default",    "subtractive",    "fm",         "karplus-strong", "modal",       "additive",
    "percussion", "piano",          "pipe-organ", "bowed-string",   "reed",        "brass",
    "flute",      "plucked-string", "vocal",      "free-reed",      "harpsichord", "sample"};
// SonareSampleLoopMode, the patch's per-voice override. Distinct from the
// SoundFont sampleModes value a SonareSampleDesc carries.
inline constexpr const char* kSampleLoopModes[] = {"default", "none", "continuous", "key-down"};
inline constexpr const char* kSampleKeyTracks[] = {"default", "on", "off"};
inline constexpr const char* kWaveforms[] = {"default", "sine",     "saw",
                                             "square",  "triangle", "noise"};
inline constexpr const char* kBuiltinWaveforms[] = {"sine", "saw", "sawtooth", "square",
                                                    "triangle"};
inline constexpr const char* kFilterModels[] = {"default", "svf", "transistor-ladder",
                                                "diode-ladder", "sallen-key"};
inline constexpr const char* kFilterOutputs[] = {"default", "lowpass", "bandpass", "highpass"};
inline constexpr const char* kBodyTypes[] = {"default",   "none",       "guitar", "violin",
                                             "wood-tube", "brass-bell", "vocal"};
inline constexpr const char* kRetriggers[] = {"default", "free", "note"};
inline constexpr const char* kModSources[] = {
    "none",      "amp-env", "filter-env", "lfo1",       "lfo2",          "velocity",  "key-track",
    "mod-wheel", "random",  "breath",     "aftertouch", "expression-cc", "pitch-bend"};
inline constexpr const char* kModDestinations[] = {"none",
                                                   "pitch-cents",
                                                   "cutoff-cents",
                                                   "amp-gain",
                                                   "pan-units",
                                                   "resonance-q",
                                                   "vibrato-depth-cents",
                                                   "filter-env-depth",
                                                   "lfo1-rate-scale",
                                                   "excitation-force",
                                                   "excitation-position",
                                                   "excitation-brightness",
                                                   "spectrum-morph"};
inline constexpr const char* kControllerInputs[] = {"control-change", "channel-pressure",
                                                    "poly-pressure", "pitch-bend", "velocity"};
inline constexpr const char* kControllerAxes[] = {"none",        "excitation",   "position",
                                                  "brightness",  "morph",        "loudness",
                                                  "pitch-cents", "vibrato-depth"};
inline constexpr const char* kArticulations[] = {"poly", "mono-retrigger", "mono-legato"};
inline constexpr const char* kMpeDimensions[] = {"bend", "pressure", "timbre"};
inline constexpr const char* kNoteTrackings[] = {"last", "lowest", "highest", "all"};
inline constexpr const auto& kPartRigModes = sonare::midi::kPartRigModeNames;
inline constexpr int kPartRigModeCount = SONARE_PART_RIG_CHAIN + 1;
// SonareSampleDesc.loop_mode, in SoundFont sampleModes values: 0 = no loop,
// 1 = continuous, 3 = loop while the key is held. Not the scale of
// kSampleLoopModes, and the names carry no ordinal 2, so the value of name i is
// kSampleDescLoopValues[i].
inline constexpr const char* kSampleDescLoopModes[] = {"none", "continuous", "key-down"};
inline constexpr int kSampleDescLoopValues[] = {0, 1, 3};

static_assert(std::size(kPartRigModes) == kPartRigModeCount,
              "part rig mode names out of step with the enum");
static_assert(std::size(kSampleDescLoopModes) == std::size(kSampleDescLoopValues),
              "sample descriptor loop mode names out of step with their values");
static_assert(std::size(kEngineModes) == SONARE_SYNTH_ENGINE_MODE_COUNT,
              "engine mode names out of step with the enum");
static_assert(std::size(kSampleLoopModes) == SONARE_SAMPLE_LOOP_MODE_COUNT,
              "sample loop mode names out of step with the enum");
static_assert(std::size(kSampleKeyTracks) == SONARE_SAMPLE_KEY_TRACK_COUNT,
              "sample key track names out of step with the enum");
static_assert(std::size(kWaveforms) == SONARE_SYNTH_OSC_WAVEFORM_COUNT,
              "oscillator waveform names out of step with the enum");
static_assert(std::size(kFilterModels) == SONARE_SYNTH_FILTER_MODEL_COUNT,
              "filter model names out of step with the enum");
static_assert(std::size(kFilterOutputs) == SONARE_SYNTH_FILTER_OUTPUT_COUNT,
              "filter output names out of step with the enum");
static_assert(std::size(kBodyTypes) == SONARE_SYNTH_BODY_TYPE_COUNT,
              "body type names out of step with the enum");
static_assert(std::size(kRetriggers) == SONARE_SYNTH_RETRIGGER_COUNT,
              "retrigger names out of step with the enum");
static_assert(std::size(kModSources) == SONARE_SYNTH_MOD_SOURCE_COUNT,
              "mod source names out of step with the enum");
static_assert(std::size(kModDestinations) == SONARE_SYNTH_MOD_DESTINATION_COUNT,
              "mod destination names out of step with the enum");
static_assert(std::size(kControllerInputs) == SONARE_CONTROLLER_INPUT_COUNT,
              "controller input names out of step with the enum");
static_assert(std::size(kControllerAxes) == SONARE_CONTROLLER_AXIS_COUNT,
              "controller axis names out of step with the enum");
static_assert(std::size(kArticulations) == SONARE_ARTICULATION_COUNT,
              "articulation names out of step with the enum");
static_assert(std::size(kMpeDimensions) == SONARE_MPE_DIMENSION_COUNT,
              "MPE dimension names out of step with the enum");
static_assert(std::size(kNoteTrackings) == SONARE_NOTE_TRACKING_COUNT,
              "note tracking names out of step with the enum");

/// An input-only spelling of a value: accepted by name, never listed or returned.
struct InputAlias {
  const char* const* table;
  const char* name;
  int value;
};

inline constexpr InputAlias kInputAliases[] = {
    // The pre-rename spelling of "transistor-ladder".
    {kFilterModels, "moog-ladder", SONARE_SYNTH_FILTER_TRANSISTOR_LADDER},
};

/// Ordinal an input-only spelling of @p table stands for, or -1.
inline int input_alias(const char* const* table, std::string_view name) {
  for (const InputAlias& alias : kInputAliases) {
    if (alias.table == table && name == alias.name) return alias.value;
  }
  return -1;
}

/// SonareSampleDesc.loop_mode value of a sample descriptor loop mode name, or -1.
inline int sample_desc_loop_value(std::string_view name) {
  for (std::size_t i = 0; i < std::size(kSampleDescLoopModes); ++i) {
    if (name == kSampleDescLoopModes[i]) return kSampleDescLoopValues[i];
  }
  return -1;
}

/// One table with its entry count, for code that takes a table at runtime.
struct NameTable {
  const char* const* names;
  std::size_t count;
};

template <std::size_t N>
constexpr NameTable name_table(const char* const (&names)[N]) {
  return {names, N};
}

/// The table @ref sonare_synth_enum_names serves for each SonareSynthEnumKind, in kind order.
inline constexpr NameTable kTablesByKind[] = {
    name_table(kEngineModes),     name_table(kWaveforms),        name_table(kFilterModels),
    name_table(kFilterOutputs),   name_table(kBodyTypes),        name_table(kModSources),
    name_table(kModDestinations), name_table(kBuiltinWaveforms), name_table(kControllerInputs),
    name_table(kControllerAxes),  name_table(kArticulations),    name_table(kMpeDimensions),
    name_table(kNoteTrackings),   name_table(kSampleLoopModes),  name_table(kSampleKeyTracks),
    name_table(kRetriggers),      name_table(kPartRigModes),     name_table(kSampleDescLoopModes),
};

static_assert(std::size(kTablesByKind) == SONARE_SYNTH_ENUM_SAMPLE_DESC_LOOP_MODE + 1,
              "every SonareSynthEnumKind needs a table, in kind order");

}  // namespace sonare::c_api::synth_names
