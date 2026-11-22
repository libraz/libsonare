#include "sonare_cli_registry.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "analysis/boundary_detector.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/section_analyzer.h"
#include "core/audio.h"
#include "core/channel_layout.h"
#include "effects/normalize.h"
#include "effects/silence.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/rhythm.h"
#include "metering/dynamic_range.h"
#include "sonare_cli.h"
#include "sonare_cli_project.h"
#include "util/constants.h"
#include "util/resource_limits.h"

#ifdef SONARE_WITH_ARRANGEMENT
#include "editing/note_model/note_transcriber.h"
#include "editing/pitch_editor/note_segmenter.h"
#endif
#ifdef SONARE_WITH_PLAYBACK
#include "playback/config.h"
#endif

#ifdef SONARE_WITH_VOICE_CHANGER
#include "editing/voice_changer/realtime.h"
#endif
#ifdef SONARE_WITH_MASTERING
#include "mastering/assistant/platform_targets.h"
#include "mastering/eq/eq_band.h"
#endif

// ---------------------------------------------------------------------------
// Immutable native CLI registry
// ---------------------------------------------------------------------------

CliOptionValue null_default() { return {}; }

namespace {

CliOptionValue bool_default(bool value = false) {
  CliOptionValue result;
  result.kind = CliOptionDefaultKind::Boolean;
  result.boolean_value = value;
  return result;
}

CliOptionValue int_default(int value) {
  CliOptionValue result;
  result.kind = CliOptionDefaultKind::Integer;
  result.integer_value = value;
  return result;
}

CliOptionValue number_default(double value) {
  CliOptionValue result;
  result.kind = CliOptionDefaultKind::Number;
  result.number_value = value;
  return result;
}

CliOptionValue string_default(const std::string& value) {
  CliOptionValue result;
  result.kind = CliOptionDefaultKind::String;
  result.string_value = value;
  return result;
}

CliOptionValue string_array_default() {
  CliOptionValue result;
  result.kind = CliOptionDefaultKind::StringArray;
  return result;
}

CliOptionSpec make_option(const char* name, CliOptionArity arity, CliOptionScalarType type,
                          CliOptionValue default_value, std::vector<std::string> aliases = {},
                          CliOptionValue implicit_optional_default = {}, bool required = false,
                          bool repeatable = false, bool global_lexical = false,
                          bool inventory = true) {
  CliOptionSpec result;
  result.name = name;
  result.aliases = std::move(aliases);
  result.arity = arity;
  result.scalar_type = type;
  result.default_value = required ? null_default() : std::move(default_value);
  result.implicit_optional_default = std::move(implicit_optional_default);
  result.required = required;
  result.repeatable = repeatable;
  result.global_lexical = global_lexical;
  result.inventory = inventory;
  return result;
}

CliOptionSpec flag(const char* name, bool global_lexical = false, bool inventory = true,
                   std::vector<std::string> aliases = {}) {
  return make_option(name, CliOptionArity::Flag, CliOptionScalarType::Boolean, bool_default(),
                     std::move(aliases), {}, false, false, global_lexical, inventory);
}

CliOptionSpec int_value(const char* name, int value, bool required = false,
                        bool global_lexical = false, bool inventory = true) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Integer,
                     int_default(value), {}, {}, required, false, global_lexical, inventory);
}

CliOptionSpec int_value(const char* name, bool required = false, bool global_lexical = false,
                        bool inventory = true) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Integer,
                     null_default(), {}, {}, required, false, global_lexical, inventory);
}

CliOptionSpec number_value(const char* name, double value, bool required = false,
                           bool global_lexical = false, bool inventory = true,
                           std::vector<std::string> aliases = {}) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Number,
                     number_default(value), std::move(aliases), {}, required, false, global_lexical,
                     inventory);
}

CliOptionSpec number_value(const char* name, bool required = false, bool global_lexical = false,
                           bool inventory = true) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Number,
                     null_default(), {}, {}, required, false, global_lexical, inventory);
}

CliOptionSpec string_value(const char* name, const char* value, bool required = false,
                           bool repeatable = false, bool inventory = true) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::String,
                     repeatable ? string_array_default() : string_default(value), {}, {}, required,
                     repeatable, false, inventory);
}

CliOptionSpec string_value(const char* name, bool required = false, bool repeatable = false,
                           bool inventory = true) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::String,
                     repeatable ? string_array_default() : string_default(""), {}, {}, required,
                     repeatable, false, inventory);
}

CliOptionSpec path_value(const char* name, bool required = false, bool global_lexical = false,
                         bool inventory = true, std::vector<std::string> aliases = {}) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Path, null_default(),
                     std::move(aliases), {}, required, false, global_lexical, inventory);
}

// Every caller sits inside one of these two command blocks, so the helper has
// to carry their union or it becomes an unused function when both are off.
#if defined(SONARE_WITH_MASTERING) || \
    (defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR))
CliOptionSpec required_path(const char* name) { return path_value(name, true); }
#endif

// A string option whose absence is null rather than the empty string. The CLI
// contract compares declared defaults across the two front-ends, so the
// representation has to agree with the one the Python CLI publishes; a value
// the caller spells, an empty one included, reaches the handler as given.
CliOptionSpec nullable_string_value(const char* name) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::String,
                     null_default(), {}, {}, false, false, false, true);
}

// A repeatable path. `path_value` takes no repeatable flag because most path
// options here are single-valued, and the default has to be the empty array
// rather than null so the two front-ends publish the same absent value.
#if defined(SONARE_WITH_MIXING) || defined(SONARE_WITH_MIXING_ASSISTANT)
CliOptionSpec repeatable_path(const char* name) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Path,
                     string_array_default(), {}, {}, false, true, false, true);
}
#endif

#ifdef SONARE_WITH_ARRANGEMENT
// The positional every project-reading leaf names in its missing-argument error.
constexpr const char* kProjectJsonPositional = "project.json";

CliOptionSpec optional_string(const char* name, const char* implicit = "true",
                              bool repeatable = false) {
  return make_option(name, CliOptionArity::OptionalValue, CliOptionScalarType::String,
                     null_default(), {}, string_default(implicit), false, repeatable);
}
#endif

// ---------------------------------------------------------------------------
// Option domains
//
// A domain is declared here, next to the option, and enforced in exactly one
// place (validate_cli_arguments). A handler that repeats one is the shape this
// layer exists to remove: the same (command, option, value) then has two
// answers depending on which code path reached it first.
// ---------------------------------------------------------------------------

CliOptionDomain greater_than(double minimum) {
  CliOptionDomain domain;
  domain.has_minimum = true;
  domain.minimum = minimum;
  domain.exclusive_minimum = true;
  return domain;
}

#ifdef SONARE_WITH_ARRANGEMENT
CliOptionDomain less_than(double maximum) {
  CliOptionDomain domain;
  domain.has_maximum = true;
  domain.maximum = maximum;
  domain.exclusive_maximum = true;
  return domain;
}
#endif

CliOptionDomain at_least(double minimum) {
  CliOptionDomain domain;
  domain.has_minimum = true;
  domain.minimum = minimum;
  return domain;
}

CliOptionDomain above_zero_up_to(double maximum) {
  CliOptionDomain domain = greater_than(0.0);
  domain.has_maximum = true;
  domain.maximum = maximum;
  return domain;
}

// An inclusive numeric range. Expressing one as a choices list is exact for a
// dozen enumerators and absurd for a twelve-bit mask, so a range that is a
// range says so.
CliOptionDomain between(double minimum, double maximum) {
  CliOptionDomain domain = at_least(minimum);
  domain.has_maximum = true;
  domain.maximum = maximum;
  return domain;
}

CliOptionDomain choices_of(std::vector<std::string> values) {
  CliOptionDomain domain;
  domain.choices = std::move(values);
  return domain;
}

// The accepted value set of an option that selects an enumerator by index.
//
// An index outside the enumeration is not a value the DSP can act on. The
// switches that map one (mastering::api::eq_band_type and its siblings) answer
// every unrecognized index with their first enumerator, so a typo'd `--type 999`
// applies a peak filter, prints a normal JSON result and exits 0. Declaring the
// range makes the refusal happen here, once, alongside every other domain --
// rather than each handler re-deriving an enumerator count the registry cannot
// see. `[lowest, highest]` is inclusive.
CliOptionDomain enum_index(int lowest, int highest) {
  std::vector<std::string> values;
  values.reserve(static_cast<size_t>(highest - lowest + 1));
  for (int value = lowest; value <= highest; ++value) values.push_back(std::to_string(value));
  return choices_of(std::move(values));
}

// An integer read into a C int ends at INT_MAX, so a lower bound alone gets that upper bound.
CliOptionSpec with_domain(CliOptionSpec spec, CliOptionDomain domain) {
  if (spec.scalar_type == CliOptionScalarType::Integer && !spec.int64_width && domain.has_minimum &&
      !domain.has_maximum && domain.choices.empty()) {
    domain.has_maximum = true;
    domain.maximum = static_cast<double>(std::numeric_limits<int>::max());
  }
  spec.domain = std::move(domain);
  return spec;
}

#ifdef SONARE_WITH_ARRANGEMENT
/// Marks an integer option backed by an `int64_t` field, read with `get_int64`.
CliOptionSpec int64_width(CliOptionSpec spec) {
  spec.int64_width = true;
  return spec;
}
#endif

// One spelling of a position given in samples or in seconds, from a builder with
// no default: absence lets the handler apply the library's own.
CliOptionSpec position(CliOptionSpec spec) { return with_domain(std::move(spec), at_least(0.0)); }

// A tempo, on every command that takes one, is refused at the flag unless it is
// greater than 0, so no command reaches the library with a tempo nobody can play.
CliOptionSpec tempo(CliOptionSpec spec) { return with_domain(std::move(spec), greater_than(0.0)); }

#if defined(SONARE_WITH_ARRANGEMENT) || defined(SONARE_WITH_ACOUSTIC_SIM)
// A render length or channel count the library chooses when the flag is left out;
// a value the caller spells is refused at the flag unless it is greater than 0.
CliOptionSpec library_sized(CliOptionSpec spec) {
  return with_domain(std::move(spec), greater_than(0.0));
}
#endif

// A twelve-bit scale mask the caller may also name by mode. It stays an Integer
// option in the inventory: the name is another spelling of the same mask.
CliOptionSpec mode_mask_value(const char* name, int value) {
  CliOptionSpec spec = int_value(name, value);
  spec.accepts_mode_name = true;
  return spec;
}

CliOptionSpec mode_mask_value(const char* name) {
  CliOptionSpec spec = int_value(name);
  spec.accepts_mode_name = true;
  return spec;
}

CliOptionSpec output_value(bool required = false) {
  return path_value("output", required, true, true, {"o"});
}

// An output file the command cannot run without.
CliOptionSpec required_output() { return output_value(true); }

#ifdef SONARE_WITH_MASTERING
// Output bit depth for every command that writes a WAV. Declared as a domain so
// the check is unconditional rather than sitting inside the `-o` branch.
CliOptionSpec bits_value() { return with_domain(int_value("bits", 16), choices_of({"16", "24"})); }

// The Python CLI declares this one as an argparse `choices=` tuple, which is a
// parse-time rejection, so the native contract has to report it as usage.
CliOptionSpec true_peak_oversample_value() {
  return with_domain(int_value("true-peak-oversample", 4), choices_of({"1", "2", "4", "8", "16"}));
}

// Delivery target for the mastering assistant. The accepted names come from
// mastering::assistant::platform_names() rather than a list restated here, which
// is the derivation platform_targets.h describes: appending a row to that table
// extends this option with no edit, and no spelling can drift from it. The
// default matches AssistantConfig::target_platform, so an invocation without the
// option keeps the streaming convention it had before the option existed.
CliOptionSpec target_platform_value() {
  return with_domain(string_value("target-platform", "streaming"),
                     choices_of(sonare::mastering::assistant::platform_names()));
}

// `mastering` runs one chain, chosen by --preset, --chain-config or --assistant; the
// choice and the options only the assistant reads are decided before the input is read.
CliValidationError validate_mastering_selection(const CliArgs& args) {
  const bool has_assistant = args.has("assistant");
  // Alongside --assistant, --preset names the base the suggestion starts from
  // rather than a second chain, so only --chain-config excludes the other two.
  if (args.has("chain-config") && (args.has("preset") || has_assistant)) {
    return {"--chain-config is mutually exclusive with --preset and --assistant"};
  }
  if (!args.has("chain-config") && !args.has("preset") && !has_assistant) {
    return {
        "mastering needs --preset or --chain-config, or --assistant (which may take "
        "--preset); loudness-only normalisation is mastering-normalize-loudness"};
  }
  if (has_assistant) return {};
  // An option that only reaches an AssistantConfig field is refused, not dropped.
  for (const char* name : {"target-lufs", "ceiling-dbtp", "enable-repair", "explain",
                           "target-platform", "no-streaming-safe", "speech-mono-amount"}) {
    if (args.has(name)) return {std::string("--") + name + " requires --assistant"};
  }
  return {};
}

// A --platforms document is command-line text, so its refusals precede the input read.
CliValidationError validate_streaming_platforms(const CliArgs& args) {
  return {streaming_platforms_refusal(args)};
}

// `repair` switches between measuring (`--detect`) and repairing by the assistant, a
// preset or `--stages`; every combination is decided before the input is read.
CliValidationError validate_repair_arguments(const CliArgs& args) {
  return {repair_argument_refusal(args)};
}

// A `--params` text is command-line text: its entries are decided before the input is read.
CliValidationError validate_params_grammar(const CliArgs& args) {
  return {params_grammar_refusal(args.get_string("params"))};
}

CliValidationError validate_mastering_arguments(const CliArgs& args) {
  if (CliValidationError error = validate_mastering_selection(args); !error.empty()) return error;
  if (CliValidationError error = validate_params_grammar(args); !error.empty()) return error;
  return {chain_params_refusal(args.get_string("params"))};
}

CliValidationError validate_processor_params(const CliArgs& args) {
  if (CliValidationError error = validate_params_grammar(args); !error.empty()) return error;
  return {
      unknown_processor_params_refusal(args.get_string("processor"), args.get_string("params"))};
}

// `--params` fully specifies the EQ, so a shortcut beside it is refused rather than dropped.
CliValidationError validate_eq_params(const CliArgs& args) {
  if (!args.has("params")) return {};
  if (CliValidationError error = validate_params_grammar(args); !error.empty()) return error;
  for (const char* shortcut :
       {"type",         "frequency-hz",   "gain-db",           "q",          "coeff-mode",
        "slope-db-oct", "placement",      "proportional-q",    "dynamic",    "threshold-db",
        "ratio",        "range-db",       "attack-ms",         "release-ms", "detector-delay-ms",
        "phase-mode",   "resolution",     "auto-gain",         "gain-scale", "output-gain-db",
        "output-pan",   "auto-threshold", "sidechain-freq-hz", "sidechain-q"}) {
    if (args.has(shortcut)) {
      return {std::string("--") + shortcut + " cannot be combined with --params"};
    }
  }
  return {unknown_processor_params_refusal("eq.equalizer", args.get_string("params"))};
}

CliValidationError validate_assistant_params(const CliArgs& args) {
  return {assistant_params_refusal(args)};
}
#endif

// `--f0-max-hz` must stay above `--f0-min-hz`; neither option's own domain can express
// that, and the two pitch engines disagreed about it (pyin checked, yin did
// not), so the CLI settles it before either is reached.
//
// The comparison is between the values the command will actually run with, not
// between two supplied options: `--f0-min-hz 3000` on its own inverts the range
// just as surely against the 2093 Hz default `--f0-max-hz`, and gating the check on both
// being present let that invocation through to a `SONARE_CHECK` that names
// neither option. The effective values are in the message because the offending
// half is commonly the one the caller never typed.
CliValidationError validate_pitch_frequency_order(const CliArgs& args) {
  if (!args.has("f0-min-hz") && !args.has("f0-max-hz")) return {};
  const float minimum = args.get_float("f0-min-hz", 65.0f);
  const float maximum = args.get_float("f0-max-hz", 2093.0f);
  if (maximum > minimum) return {};
  std::ostringstream message;
  message << "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz " << minimum
          << ", --f0-max-hz " << maximum << ")";
  return {message.str()};
}

// The same ordering rule as `pitch`, for a vocal session. Neither
// option has a registry default here (an unspelled one is left to the session),
// so the effective value of the missing half is the session's own default,
// which is the pitch tracker's range.
CliValidationError validate_vocal_frequency_order(const CliArgs& args) {
  if (!args.has("f0-min-hz") && !args.has("f0-max-hz")) return {};
  const double minimum = args.get_double("f0-min-hz", 65.0);
  const double maximum = args.get_double("f0-max-hz", 2093.0);
  if (maximum > minimum) return {};
  std::ostringstream message;
  message << "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz " << minimum
          << ", --f0-max-hz " << maximum << ")";
  return {message.str()};
}

// The detector combines the MFCC and chroma streams frame-for-frame, so with
// neither enabled there is nothing to combine and the novelty curve is
// undefined. A per-option domain cannot express it: either flag is legal alone.
CliValidationError validate_boundary_feature_streams(const CliArgs& args) {
  if (!args.has("no-mfcc") || !args.has("no-chroma")) return {};
  return {
      "--no-mfcc and --no-chroma cannot both be given: boundary detection "
      "needs at least one feature stream"};
}

// Both lengths default to 0, which is no fade, so a fade with neither side given does nothing.
CliValidationError validate_fade_sides(const CliArgs& args) {
  if (args.has("fade-in-sec") || args.has("fade-out-sec")) return {};
  return {"give --fade-in-sec or --fade-out-sec"};
}

#ifdef SONARE_WITH_VOICE_CHANGER
// A built-in preset is named by its library id in any letter case; the id table is the one
// realtime_voice_changer_preset_from_id reads (voice_changer/realtime/voice_presets.cpp).
std::string builtin_voice_preset_refusal(const std::string& value) {
  const auto names = editing::voice_changer::realtime_voice_changer_preset_names();
  if (find_cli_token(value, names)) return {};
  std::string refusal = "argument --preset: must be one of ";
  for (size_t index = 0; index < names.size(); ++index) {
    if (index != 0) refusal += ", ";
    refusal += names[index];
  }
  return refusal;
}
#endif

// One output mode at most: the three write different file sets.
CliValidationError validate_hpss_output_modes(const CliArgs& args) {
  const int modes = static_cast<int>(args.has("harmonic-only")) +
                    static_cast<int>(args.has("percussive-only")) +
                    static_cast<int>(args.has("with-residual"));
  if (modes <= 1) return {};
  return {
      "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
      "--percussive-only, or --with-residual"};
}

// --top-db is the legacy relative rule and --threshold-db the absolute one.
CliValidationError validate_trim_silence_rule(const CliArgs& args) {
  if (args.has("threshold-db") && args.has("top-db")) {
    return {"--threshold-db and --top-db are mutually exclusive"};
  }
  return {};
}

// The preset-selector relations of voice-change, then the --set grammar.
CliValidationError validate_voice_change(const CliArgs& args) {
  const bool has_preset = args.has("preset");
  const bool has_preset_json = args.has("preset-json");
  const bool has_preset_pack = args.has("preset-pack");
  const bool selected = has_preset || has_preset_json;
  if (has_preset && has_preset_json) {
    return {
        "voice-change preset selectors are mutually exclusive: choose one of --preset, "
        "--preset-json, or --preset-pack"};
  }
  if (has_preset_pack && has_preset_json) {
    return {"--preset-pack and --preset-json are mutually exclusive"};
  }
  // A pack names the file, --preset names the entry inside it, so the pair is one selector; this
  // check precedes the ones below so a pack without an entry reports the missing --preset.
  if (has_preset_pack && !has_preset) {
    return {"--preset-pack requires --preset to select an entry"};
  }
  // The default mode is a no-op, so only an explicit absolute request conflicts with a preset.
  if (selected && (args.has("pitch-semitones") || args.has("formant-factor") ||
                   args.get_string("formant-mode", "relative") != "relative")) {
    return {
        "--pitch-semitones/--formant-factor/--formant-mode cannot be combined with a realtime "
        "preset"};
  }
  if (args.has("set") && !selected) {
    return {"--set requires --preset, --preset-json, or --preset-pack"};
  }
#ifdef SONARE_WITH_VOICE_CHANGER
  if (has_preset && !has_preset_pack) {
    if (auto refusal = builtin_voice_preset_refusal(args.get_string("preset")); !refusal.empty()) {
      return {std::move(refusal)};
    }
  }
#endif
  return {voice_set_refusal(args.get_string_list("set"))};
}

#ifdef SONARE_WITH_VOICE_CHANGER
CliValidationError validate_voice_preset(const CliArgs& args) {
  if (args.has("preset")) return {builtin_voice_preset_refusal(args.get_string("preset"))};
  return {};
}
constexpr CliCommandValidator kVoicePresetValidator = validate_voice_preset;
#else
constexpr CliCommandValidator kVoicePresetValidator = nullptr;
#endif

CliValidationError validate_voice_preset_validate(const CliArgs& args) {
  return {voice_set_refusal(args.get_string_list("set"))};
}

CliValidationError validate_polyphonic_edits(const CliArgs& args) {
  return {polyphonic_edit_refusal(args.get_string_list("edit"))};
}

#ifdef SONARE_WITH_ARRANGEMENT
// Each rule is one the Python facade states (_effects_note_model.py, _transcribe_config and
// _transcribe_reattack) or the C ABI refuses (sonare_c_transcribe.cpp:128 and :157).
CliValidationError validate_transcribe(const CliArgs& args) {
  if (args.has("f0-min-hz") && args.has("f0-max-hz")) {
    const double minimum = args.get_double("f0-min-hz", 0.0);
    const double maximum = args.get_double("f0-max-hz", 0.0);
    if (!(maximum > minimum)) {
      std::ostringstream message;
      message << "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz " << minimum
              << ", --f0-max-hz " << maximum << ")";
      return {message.str()};
    }
  }
  // 0 is "no split"; a given ratio otherwise has to exceed 1 (the facade's reattack_ratio rule).
  if (args.has("reattack-ratio")) {
    const float ratio = args.get_float("reattack-ratio", 0.0f);
    if (ratio != 0.0f && !(ratio > 1.0f)) {
      return {"--reattack-ratio must be 0 (no split) or greater than 1"};
    }
  }
  if (args.has("min-note-division") && args.has("min-note-ms")) {
    return {"--min-note-division and --min-note-ms cannot both be given"};
  }
  if (args.has("polyphonic") && args.get_string("monophonic-model", "segments") != "segments") {
    return {"--monophonic-model singing cannot be combined with --polyphonic"};
  }
  return {};
}
#endif

#ifdef SONARE_WITH_MIXING
// A scene comes from exactly one source, and a render needs both its inputs and a file to write.
CliValidationError validate_mix_sources(const CliArgs& args) {
  const bool scene = args.has("scene");
  const bool preset = args.has("preset");
  if (!scene && !preset) return {"mix needs --scene or --preset"};
  if (scene && preset) return {"--scene is mutually exclusive with --preset"};
  if (std::string refusal = mix_input_refusal(args); !refusal.empty()) return {refusal};
  const bool inputs = !args.get_string_list("input").empty();
  const bool output = !args.output_file.empty();
  if (inputs && !output) return {"mix with --input requires --output"};
  if (output && !inputs) return {"mix with --output requires at least one --input"};
  return {};
}
#endif

#ifdef SONARE_WITH_MIXING_ASSISTANT
// The track entries and the assistant's `--params` are command-line text, so they are refused
// before any track is decoded.
CliValidationError validate_suggest_mix(const CliArgs& args) {
  return {suggest_mix_argument_refusal(args)};
}
#endif

// --type decides which frequencies the filter reads; one it reads is required, and one it does
// not read is refused rather than ignored, as the library refuses it.
CliValidationError validate_filter_frequencies(const CliArgs& args) {
  const std::string type = args.get_string("type", "");
  const bool corner = type == "high-pass" || type == "low-pass";
  const char* types = corner ? "high-pass or low-pass" : "band-pass or notch";
  const std::vector<const char*> read = corner
                                            ? std::vector<const char*>{"cutoff-hz"}
                                            : std::vector<const char*>{"center-hz", "bandwidth-hz"};
  const std::vector<const char*> unread =
      corner ? std::vector<const char*>{"center-hz", "bandwidth-hz"}
             : std::vector<const char*>{"cutoff-hz"};
  for (const char* name : read) {
    if (!args.has(name)) return {std::string("--") + name + " is required when --type is " + types};
  }
  for (const char* name : unread) {
    if (args.has(name)) {
      return {std::string("--") + name + " does not apply when --type is " + types};
    }
  }
  if (!corner && args.get_int("order", 2) != 2) {
    return {std::string("--order must be 2 when --type is ") + types};
  }
  return {};
}

// A position is spelled in samples or in seconds; both at once is refused by
// name, as the library refuses the two keyword spellings.
CliValidationError refuse_both_bound_spellings(const CliArgs& args,
                                               std::initializer_list<const char*> bounds) {
  for (const char* bound : bounds) {
    const std::string sample = std::string(bound) + "-sample";
    const std::string sec = std::string(bound) + "-sec";
    if (args.has(sample) && args.has(sec)) {
      return {"give --" + sample + " or --" + sec + ", not both"};
    }
  }
  return {};
}

CliValidationError validate_note_move_bounds(const CliArgs& args) {
  return refuse_both_bound_spellings(args, {"onset", "offset", "target-onset"});
}

CliValidationError validate_note_stretch_bounds(const CliArgs& args) {
  return refuse_both_bound_spellings(args, {"onset", "offset"});
}

// The tempo search runs between the two bounds, so an inverted pair searches
// nothing and reports whatever the fallback is rather than failing. Neither the
// rhythm analyzer nor the beat analyzer behind it looks at these.
CliValidationError validate_rhythm_tempo_range(const CliArgs& args) {
  const float minimum = args.get_float("bpm-min", 60.0f);
  const float maximum = args.get_float("bpm-max", 200.0f);
  if (maximum > minimum) return {};
  std::ostringstream message;
  message << "--bpm-max must be greater than --bpm-min (--bpm-min " << minimum << ", --bpm-max "
          << maximum << ")";
  return {message.str()};
}

// Compared against the band the analyzer will use, not the raw options: an absent
// bound is MelodyConfig's, so `--f0-max-hz 50` alone is refused against its floor.
CliValidationError validate_melody_frequency_band(const CliArgs& args) {
  const sonare::MelodyConfig library;
  const float minimum = args.get_float("f0-min-hz", library.fmin);
  const float maximum = args.get_float("f0-max-hz", library.fmax);
  if (maximum > minimum) return {};
  std::ostringstream message;
  message << "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz " << minimum
          << ", --f0-max-hz " << maximum << ")";
  return {message.str()};
}

// The profile and mode names, wherever given, are refused before the input is read. With HPSS on
// the hop is bound too: the separation needs enough overlap to resynthesize, and
// the C entry point refuses a hop below 16 for exactly this pair. The STFT's own
// geometry check does not reach it -- at the 4096-point default a hop of 8
// satisfies `hop <= n_fft / 2` and still starves the separation. A per-option
// domain cannot express it because the same hop is fine without the flag.
CliValidationError validate_key_arguments(const CliArgs& args) {
  try {
    if (args.has("profile")) parse_key_profile_option(args.get_string("profile"));
    if (args.has("modes")) parse_mode_list_option(args.get_string("modes"));
  } catch (const std::invalid_argument& error) {
    return {error.what()};
  }
  if (!args.has("hpss")) return {};
  if (args.hop_length >= sonare::kMinKeyHpssHopLength) return {};
  std::ostringstream message;
  message << "--hop-length must be at least " << sonare::kMinKeyHpssHopLength
          << " with --hpss (got " << args.hop_length << ")";
  return {message.str()};
}

// The key context names are read whenever given, as the Python CLI reads them, though only
// --key-context applies them.
CliValidationError validate_chords_key_context(const CliArgs& args) {
  try {
    if (args.has("key-root")) parse_pitch_class_option(args.get_string("key-root"));
    if (args.has("key-mode")) parse_mode_option(args.get_string("key-mode"));
  } catch (const std::invalid_argument& error) {
    return {error.what()};
  }
  return {};
}

CliValidationError validate_analyze_meter_candidates(const CliArgs& args) {
  if (!args.has("meter-candidates")) return {};
  try {
    parse_meter_numerators(args.get_string("meter-candidates"));
  } catch (const std::invalid_argument& error) {
    return {error.what()};
  }
  return {};
}

#ifdef SONARE_WITH_ARRANGEMENT
// The project bounce renders its master at a speaker-layout width, so the C
// ABI accepts 1, 2, 6 or 8 channels -- each at most the scene master's own
// layout width, which only the C ABI can see -- and refuses any other count
// rather than emitting silent planes. Without this the refusal arrives from
// the render as a bare invalid-parameter error, after the project has been
// loaded, with nothing naming the option that caused it. The master-width cap
// itself is not duplicated here: a count from this set that still exceeds the
// scene's master reaches the same generic error the render always had.
//
// The layout set is a command validator rather than a per-option domain: a
// domain is published in the shared option inventory both CLIs are pinned
// against, and both declare only the positive bound there.
CliValidationError validate_project_bounce_channels(const CliArgs& args) {
  if (!args.has("channels")) return {};
  // The registry domain has already refused a count at or below 0; leaving the
  // flag out is how the scene master's own width is asked for.
  const int channels = args.get_int("channels", 0);
  const int mono = sonare::channel_count(sonare::ChannelLayout::Mono);
  const int stereo = sonare::channel_count(sonare::ChannelLayout::Stereo);
  if (channels == mono || channels == stereo || sonare::is_surround_channel_count(channels)) {
    return {};
  }
  return {"argument --channels: must be one of " + std::to_string(mono) + ", " +
          std::to_string(stereo) + ", 6, 8; leave it out for the scene master's width"};
}

// `project bounce` spells GM routing as the bare flag; its `--audio` bindings and the preset
// `--synth` names are command-line text, refused before the project is read.
CliValidationError validate_project_bounce_arguments(const CliArgs& args) {
  if (CliValidationError refusal = validate_project_bounce_channels(args); !refusal.empty()) {
    return refusal;
  }
  if (std::string refusal = project_synth_refusal(args, true); !refusal.empty()) return {refusal};
  return {project_audio_binding_refusal(args)};
}

// `midi-render` spells GM routing as leaving `--synth` out, so any value names a preset.
CliValidationError validate_midi_render_arguments(const CliArgs& args) {
  if (CliValidationError refusal = validate_project_bounce_channels(args); !refusal.empty()) {
    return refusal;
  }
  return {project_synth_refusal(args, false)};
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
// A source or listener is given whole or left to the room's scaled default, never filled in.
CliValidationError validate_room_placement(const CliArgs& args) {
  for (const std::string role : {"source", "listener"}) {
    const std::string x = role + "-x";
    const std::string y = role + "-y";
    const std::string z = role + "-z";
    const int given = int{args.has(x)} + int{args.has(y)} + int{args.has(z)};
    if (given != 0 && given != 3) {
      return {"--" + x + ", --" + y + " and --" + z + " must be given together"};
    }
  }
  return {};
}
#endif

CliValidationError validate_project_align_takes(const CliArgs& args) {
  return {project_audio_binding_refusal(args)};
}
#endif

// A sweep through equal ratios has no starting point at or below 0 Hz.
CliValidationError validate_chirp_sweep(const CliArgs& args) {
  if (args.has("exponential") && args.fmin <= 0.0f) {
    return {"--fmin-hz must be greater than 0 for an exponential sweep"};
  }
  return {};
}

// The grammar of a list option is one check for every command that takes one.
CliValidationError number_list_error(const CliArgs& args, const char* option, bool integral,
                                     size_t* count = nullptr) {
  return {number_list_refusal(option, args.get_string(option), integral, count)};
}

CliValidationError validate_float_values(const CliArgs& args) {
  return number_list_error(args, "values", false);
}

CliValidationError validate_int_values(const CliArgs& args) {
  return number_list_error(args, "values", true);
}

CliValidationError validate_click_times(const CliArgs& args) {
  return number_list_error(args, "times", false);
}

// The values are one n_bins x n_frames matrix, so their count is fixed by the two sizes.
CliValidationError validate_pcen_values(const CliArgs& args) {
  size_t count = 0;
  if (CliValidationError error = number_list_error(args, "values", false, &count); !error.empty()) {
    return error;
  }
  const long long expected = static_cast<long long>(args.get_int("n-bins", 0)) *
                             static_cast<long long>(args.get_int("n-frames", 0));
  if (static_cast<long long>(count) == expected) return {};
  return {option_subject("values") + " must hold --n-bins times --n-frames values"};
}

// The percentile pair picks two window levels, so the low one cannot sit above the high one.
// Compared against the values the command will run with, as the pitch bounds are.
CliValidationError validate_dynamic_range_percentiles(const CliArgs& args) {
  const sonare::metering::DynamicRangeConfig library;
  const float low = args.get_float("low-percentile", library.low_percentile);
  const float high = args.get_float("high-percentile", library.high_percentile);
  if (low <= high) return {};
  std::ostringstream message;
  message << "--high-percentile must be at least --low-percentile (--low-percentile " << low
          << ", --high-percentile " << high << ")";
  return {message.str()};
}

// The cepstral coefficients are taken from the mel bands, so there cannot be more of them.
CliValidationError validate_mfcc_to_audio_counts(const CliArgs& args) {
  const int n_mfcc = args.get_int("n-mfcc", sonare::kMfccDefaultCount);
  if (n_mfcc <= args.n_mels) return {};
  std::ostringstream message;
  message << "--n-mfcc must be at most --n-mels (--n-mfcc " << n_mfcc << ", --n-mels "
          << args.n_mels << ")";
  return {message.str()};
}

CliOptionSpec global_int(const char* name) { return int_value(name, false, true); }

/// An option the handler leaves to the library when absent; @p library_value is what `--help`
/// states the library takes then.
CliOptionSpec library_default(CliOptionSpec option, std::string library_value) {
  option.stated_default = std::move(library_value);
  return option;
}

/// An option whose library value depends on another option, stated as prose rather than a value.
CliOptionSpec with_stated_default(CliOptionSpec option, std::string statement) {
  option.stated_default = std::move(statement);
  return option;
}

/// A library constant as `--help` states it: an integral value keeps one decimal place.
std::string stated_number(double value) {
  std::ostringstream text;
  if (value == static_cast<double>(static_cast<long long>(value))) {
    text.setf(std::ios::fixed);
    text.precision(1);
  }
  text << value;
  return text.str();
}

CliOptionSpec library_default(CliOptionSpec option, int library_value) {
  option.stated_default = std::to_string(library_value);
  return option;
}

CliOptionSpec library_default(CliOptionSpec option, float library_value) {
  option.stated_default = stated_number(library_value);
  return option;
}

// The constant-Q hop the library accepts: it bounds the hop so the longest filter stays finite.
CliOptionDomain cqt_hop_domain() { return between(1.0, sonare::kMaxCqtHopLength); }

/// A list of numbers as the CLI reads one back: comma-separated, no spaces.
std::string joined_numbers(const std::vector<int>& values) {
  std::string text;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) text += ',';
    text += std::to_string(values[i]);
  }
  return text;
}

#ifdef SONARE_WITH_ARRANGEMENT
/// A render length the library takes from the project when the flag is left out.
CliOptionSpec project_length(CliOptionSpec spec) {
  return with_stated_default(library_sized(std::move(spec)), "through the last event and its tail");
}

/// A render channel count the library takes from the scene master when the flag is left out.
CliOptionSpec master_width(CliOptionSpec spec) {
  return with_stated_default(library_sized(std::move(spec)), "the scene master's width");
}
#endif

/// A twelve-bit scale mask (or mode name) the handler leaves to the library when absent.
CliOptionSpec global_number(const char* name, double value) {
  return number_value(name, value, false, true);
}

CliOptionSpec global_number(const char* name) { return number_value(name, false, true); }

CliOptionSpec required_int(const char* name, std::vector<std::string> aliases = {}) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Integer,
                     int_default(0), std::move(aliases), {}, true);
}

#ifdef SONARE_WITH_MASTERING
CliOptionSpec required_string(const char* name) { return string_value(name, "", true); }
#endif

std::vector<CliOptionSpec> with_json(std::vector<CliOptionSpec> options) {
  options.insert(options.begin(), flag("json", true));
  // These parser controls are part of every leaf contract, but remain hidden
  // from the stable inventory and per-command option list.  Keeping them on
  // the same records lets lexical parsing and path validation share one
  // source without making implementation controls part of the public dump.
  options.push_back(flag("quiet", true, false, {"q"}));
  options.push_back(flag("help", true, false, {"h"}));
  return options;
}

// `positionals` names the arity of a leaf that takes a positional which is not
// an audio file, or more than the one an audio leaf takes by default
// (kUnboundedPositionals for a list of audio files). `positional_name` makes
// the full count required and names the last one when it is missing.
// `preserves_stereo_input` is the leaf's own answer to what a two-channel input
// produces.
void add_command(std::vector<CliCommandSpec>& registry, const char* path, bool requires_audio,
                 std::vector<CliOptionSpec> options, std::vector<std::string> aliases = {},
                 CliCommandValidator validate = nullptr, size_t positionals = 0,
                 bool preserves_stereo_input = false, bool preserves_multichannel_input = false,
                 const char* positional_name = "") {
  registry.push_back({path, std::move(aliases), with_json(std::move(options)), requires_audio,
                      requires_audio && positionals == 0 ? 1u : positionals, positional_name,
                      preserves_stereo_input, preserves_multichannel_input, true, validate});
}

const std::vector<CliCommandSpec>& build_cli_registry() {
  static const std::vector<CliCommandSpec> registry = [] {
    std::vector<CliCommandSpec> commands;
    commands.reserve(110);

    // Analysis leaves.
    add_command(
        commands, "analyze", true,
        {flag("with-seventh"), flag("hpss"),
         // The Python CLI parses this through a non-negative finite
         // checker, so the native contract declares the same domain and
         // the same (parse-time) class.
         library_default(with_domain(number_value("chroma-high-pass-hz"), at_least(0.0)), "80.0"),
         // An odd meter is only ever reported if its numerator was
         // asked for, so without these the CLI cannot reach one. The
         // list is a string here and validated by the core, which is
         // what already holds the count and range rules.
         library_default(nullable_string_value("meter-candidates"),
                         joined_numbers(sonare::MusicAnalyzerConfig{}.meter_candidate_numerators)),
         // The Python CLI parses this through its positive-integer
         // checker, so the domain and the (parse-time) class match it.
         library_default(with_domain(int_value("meter-denominator"), greater_than(0.0)), "4")},
        {}, validate_analyze_meter_candidates);
    for (const char* path : {"bpm", "beats", "downbeats", "onsets"})
      add_command(commands, path, true, {});
    add_command(commands, "timbre", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128")});
    add_command(
        commands, "key", true,
        {library_default(with_domain(global_int("n-fft"), at_least(1.0)), "2048"),
         library_default(global_int("hop-length"), "512"), int_value("candidates"), flag("hpss"),
         flag("loudness-weighted"), library_default(number_value("high-pass-hz"), "80.0"),
         library_default(nullable_string_value("modes"), "major-minor"),
         with_stated_default(nullable_string_value("profile"), "chosen by --genre-hint"),
         with_stated_default(with_domain(nullable_string_value("genre-hint"),
                                         choices_of({"auto", "edm", "electronic", "dance", "pop",
                                                     "classical", "jazz"})),
                             sonare::KeyConfig{}.genre_hint)},
        {}, validate_key_arguments);
    // `smoothing-window` and `hmm-beam-width` carry the C entry point's domains;
    // `min-duration` and `threshold` are checked by ChordAnalyzer itself and so
    // need none. A zero smoothing window is the one the analyzer accepts and the
    // C entry refuses, and a negative beam width is never truncated against, so
    // `--hmm-beam-width -1` silently ran an unbeamed search.
    add_command(
        commands, "chords", true,
        {library_default(with_domain(global_int("n-fft"), at_least(1.0)), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(number_value("min-duration-sec"), "0.3"),
         library_default(with_domain(number_value("smoothing-window"), greater_than(0.0)), "2.0"),
         library_default(number_value("threshold"), "0.5"), flag("with-seventh"),
         library_default(with_domain(nullable_string_value("chroma-method"),
                                     choices_of({"cqt", "stft", "nnls"})),
                         "cqt"),
         flag("no-beat-sync"), flag("no-hmm"),
         library_default(with_domain(int_value("hmm-beam-width"), at_least(0.0)), "24"),
         flag("key-context"), library_default(nullable_string_value("key-root"), "C"),
         library_default(nullable_string_value("key-mode"), "major"), flag("detect-inversions")},
        {}, validate_chords_key_context);
    add_command(
        commands, "sections", true,
        {library_default(with_domain(number_value("min-duration-sec"), at_least(0.0)), "4.0"),
         library_default(with_domain(number_value("threshold"), at_least(0.0)),
                         sonare::kSectionBoundaryThreshold),
         library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512")});
    // DynamicsAnalyzer floors a tiny or negative window to avoid a 0/0 rather
    // than refusing it, so the refusal the other surfaces get has to be here.
    add_command(commands, "dynamics", true,
                {library_default(with_domain(number_value("window-sec"), greater_than(0.0)), "0.4"),
                 library_default(with_domain(global_int("hop-length"), at_least(1.0)), "512")});
    // Neither RhythmAnalyzer nor the BeatAnalyzer it forwards to validates any of
    // the three tempo bounds; the derived periods are merely floored at 1.
    add_command(commands, "rhythm", true,
                {library_default(tempo(number_value("start-bpm")), "120.0"),
                 library_default(tempo(number_value("bpm-min")), "60.0"),
                 library_default(tempo(number_value("bpm-max")), "200.0"),
                 library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512")},
                {}, validate_rhythm_tempo_range);
    // This command's `hop-length` does NOT reach the STFT, so the guard that
    // covers every other command's copy does not cover this one: MelodyAnalyzer
    // frames the signal itself and advances by the hop, so zero never advances
    // and the loop does not terminate. The plain-YIN entry it calls
    // (yin_with_confidence) validates nothing either -- the checks live in
    // yin_track, which this path does not go through.
    add_command(
        commands, "melody", true,
        {library_default(with_domain(number_value("threshold"), greater_than(0.0)), "0.1"),
         library_default(with_domain(int_value("hop-length", false, true), greater_than(0.0)),
                         "256"),
         library_default(with_domain(number_value("f0-min-hz"), greater_than(0.0)), "65.0"),
         library_default(with_domain(number_value("f0-max-hz"), greater_than(0.0)), "2093.0")},
        {}, validate_melody_frequency_band);
    // The whole of BoundaryConfig, so a command line can reach the same detector
    // the other surfaces do. `absolute-threshold` is the one that was missing and
    // mattered: `threshold` is relative to the curve's own maximum, so it cannot
    // ask whether anything changed at all, and without the floor stationary
    // material segments every few seconds. The two feature streams are on by
    // default and so are spelled as negations. Each size is refused at zero
    // because the detector has no behaviour there, and the C entry the other
    // surfaces reach refuses the same values with the same class.
    const sonare::BoundaryConfig boundary_defaults;
    add_command(commands, "boundaries", true,
                {library_default(with_domain(number_value("threshold"), at_least(0.0)),
                                 boundary_defaults.threshold),
                 library_default(with_domain(number_value("absolute-threshold"), at_least(0.0)),
                                 boundary_defaults.absolute_threshold),
                 library_default(with_domain(int_value("kernel-size"), greater_than(0.0)),
                                 boundary_defaults.kernel_size),
                 library_default(with_domain(int_value("n-mfcc"), greater_than(0.0)),
                                 boundary_defaults.n_mfcc),
                 library_default(with_domain(int_value("n-chroma"), greater_than(0.0)),
                                 boundary_defaults.n_chroma),
                 library_default(with_domain(number_value("peak-distance"), at_least(0.0)),
                                 boundary_defaults.peak_distance),
                 // One-sided seconds the relative threshold is measured against; 0 disables it.
                 library_default(with_domain(number_value("reference-window"), at_least(0.0)),
                                 boundary_defaults.reference_window),
                 flag("no-mfcc"), flag("no-chroma"), library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512")},
                {}, validate_boundary_feature_streams);

    // Processing leaves.
    // The native spectral backend repairs a non-positive analysis geometry into
    // the librosa defaults instead of refusing it, five lines above a comment
    // saying a geometry that cannot be overlap-added is an error rather than
    // something to repair. These two make the refusal the one a caller gets.
    add_command(
        commands, "pitch-shift", true,
        {number_value("pitch-semitones", true), required_output(),
         with_domain(library_default(global_int("n-fft"), "2048"), greater_than(0.0)),
         with_domain(library_default(global_int("hop-length"), "n_fft/4"), greater_than(0.0))});
    add_command(
        commands, "time-stretch", true,
        {number_value("rate", true), required_output(),
         with_domain(library_default(global_int("n-fft"), "2048"), greater_than(0.0)),
         with_domain(library_default(global_int("hop-length"), "n_fft/4"), greater_than(0.0))});
    add_command(
        commands, "pitch-correct", true,
        {number_value("current-midi", true), number_value("target-midi", true), required_output()});
    // The MIDI value is the positional, and the three narrowed options carry
    // the ranges the scale quantizer refuses outside of -- the pitch class, the
    // twelve-bit mode mask, and the grid anchor. Each is a value the caller
    // spelled correctly and the library will not act on.
    add_command(commands, "scale-quantize", false,
                {with_domain(int_value("root", 0), between(0.0, 11.0)),
                 with_domain(mode_mask_value("mode-mask", 0xAB5), between(1.0, 4095.0)),
                 with_domain(number_value("reference-midi", 69.0), between(0.0, 127.0))},
                {}, nullptr, 1);
    // The scale arguments are checked whichever target mode is selected, and
    // --target-midi names a note in both, so all three carry their range here
    // rather than only on the branch that reads them. --reference-midi is
    // deliberately unnarrowed: this path validates the anchor for finiteness
    // only, and declaring a range would refuse a value the library accepts.
    // The four retune options carry the ranges the library's correction config
    // refuses outside of.
    add_command(
        commands, "pitch-correct-timevarying", true,
        {with_domain(string_value("mode", "midi"), choices_of({"midi", "scale"})),
         with_domain(number_value("target-midi", 69.0), between(0.0, 127.0)),
         library_default(with_domain(int_value("hop-length"), greater_than(0.0)), "512"),
         library_default(with_domain(int_value("scale-root"), between(0.0, 11.0)), "0"),
         with_domain(library_default(mode_mask_value("scale-mode-mask"), "2741"),
                     between(1.0, 4095.0)),
         library_default(number_value("reference-midi"), "69.0"),
         library_default(with_domain(number_value("retune-amount"), between(0.0, 1.0)), "1.0"),
         library_default(with_domain(number_value("max-correction-semitones"), at_least(0.0)),
                         "12.0"),
         library_default(with_domain(number_value("retune-speed-ms"), at_least(0.0)), "50.0"),
         library_default(with_domain(number_value("vibrato-threshold-cents"), at_least(0.0)),
                         "20.0"),
         required_output()});
    // Each bound is given in samples or in seconds, never both, and an omitted
    // one takes the library's default (0, or the end of the input for an offset),
    // which the handler resolves.
    add_command(commands, "note-move", true,
                {position(int_value("onset-sample")), position(number_value("onset-sec")),
                 position(int_value("offset-sample")), position(number_value("offset-sec")),
                 position(int_value("target-onset-sample")),
                 position(number_value("target-onset-sec")), required_output()},
                {}, &validate_note_move_bounds);
    add_command(commands, "note-stretch", true,
                {position(int_value("onset-sample")), position(number_value("onset-sec")),
                 position(int_value("offset-sample")), position(number_value("offset-sec")),
                 number_value("ratio", 1.0), required_output()},
                {}, &validate_note_stretch_bounds);
    add_command(commands, "polyphonic-notes", true, {});
    // The analysis settings a vocal-edit session takes at creation, spelled as
    // the session spells them. None carries a default here: an omitted one is
    // not passed, so the session's own default applies, and `--no-centered` is
    // the only way to turn the (default-on) centred framing off.
    add_command(commands, "vocal-notes", true,
                {with_domain(number_value("segmentation-threshold-cents"), greater_than(0.0)),
                 with_domain(number_value("min-note-ms"), greater_than(0.0)),
                 with_domain(int_value("frame-length-samples"), above_zero_up_to(4294967295.0)),
                 with_domain(int_value("hop-length-samples"), above_zero_up_to(4294967295.0)),
                 with_domain(number_value("f0-min-hz"), greater_than(0.0)),
                 with_domain(number_value("f0-max-hz"), greater_than(0.0)),
                 with_domain(number_value("yin-threshold"), above_zero_up_to(1.0)),
                 with_domain(number_value("voiced-threshold"), between(0.0, 1.0)),
                 flag("no-centered"), with_domain(number_value("reference-hz"), greater_than(0.0))},
                {}, &validate_vocal_frequency_order);
    // One assignment per --edit occurrence, as --set does: the value reaches the
    // field parser as written, so no separator a fold could pick has to be
    // reserved.
    add_command(commands, "polyphonic-render", true,
                {string_value("edit", "", false, true), required_output()}, {},
                validate_polyphonic_edits);
    add_command(
        commands, "voice-change", true,
        {with_stated_default(nullable_string_value("preset"), "none"), path_value("preset-json"),
         path_value("preset-pack"), string_value("set", "", false, true),
         number_value("pitch-semitones"), number_value("formant-factor"),
         with_domain(string_value("formant-mode", "relative"),
                     choices_of({"relative", "absolute"})),
         required_output()},
        {}, validate_voice_change);
    add_command(commands, "voice-presets", false, {});
    add_command(commands, "voice-preset", false, {string_value("preset", "neutralMonitor")}, {},
                kVoicePresetValidator);
    // The preset document arrives as the positional rather than through an
    // option, which is the arity the trailing argument declares.
    add_command(
        commands, "voice-preset-validate", false,
        {path_value("preset-json"), string_value("preset"), string_value("set", "", false, true)},
        {}, validate_voice_preset_validate, 1);
    add_command(
        commands, "hpss", true,
        {library_default(int_value("kernel-harmonic"), "31"),
         library_default(int_value("kernel-percussive"), "31"), required_output(),
         flag("harmonic-only"), flag("percussive-only"), flag("with-residual"), flag("hard-mask"),
         with_domain(library_default(global_int("n-fft"), "2048"), greater_than(0.0)),
         with_domain(library_default(global_int("hop-length"), "n_fft/4"), greater_than(0.0))},
        {}, validate_hpss_output_modes);
    // `init` is refused here because nothing below refuses it: the core's
    // validate_config checks the numeric fields only and reads any other name
    // as its own default, so an unrecognised initialiser is caught at the
    // surface or not at all.
    add_command(commands, "decompose-stems", true,
                {library_default(with_domain(int_value("n-components"), at_least(1.0)), "4"),
                 library_default(with_domain(int_value("n-iter"), at_least(1.0)), "100"),
                 library_default(number_value("beta"), "2.0"),
                 library_default(
                     with_domain(nullable_string_value("init"), choices_of({"random", "nndsvd"})),
                     "random"),
                 library_default(with_domain(number_value("mask-power"), at_least(1.0)), "1.0"),
                 required_output(),
                 library_default(with_domain(global_int("n-fft"), at_least(1.0)), "2048"),
                 library_default(with_domain(global_int("hop-length"), at_least(1.0)), "n_fft/4")});
    add_command(commands, "preemphasis", true,
                {library_default(number_value("coef"), "0.97"), required_output()});
    add_command(commands, "deemphasis", true,
                {library_default(number_value("coef"), "0.97"), required_output()});
    add_command(commands, "trim-silence", true,
                {library_default(number_value("threshold-db"), sonare::kDefaultAbsoluteSilenceDb),
                 number_value("top-db"), output_value(),
                 library_default(global_int("n-fft"), sonare::kDefaultSilenceFrameLength),
                 library_default(global_int("hop-length"), sonare::kDefaultSilenceHopLength)},
                {}, validate_trim_silence_rule);
    // `--input` names further takes of the same part; the positional is the
    // first. `--write-takes` is the destination rather than `-o`, because one
    // run writes a file per take per interval.
    // `--report` wraps the result instead of replacing it: without the flag the
    // payload is the bare interval array it has always been, so a caller reading
    // `[0].start_sample` is untouched. The flag is what makes the diagnosis
    // reachable from a command line at all -- one interval covering everything
    // answers three different situations and the intervals cannot say which.
    add_command(commands, "split-silence", true,
                {library_default(number_value("top-db"), sonare::kDefaultSilenceTopDb),
                 path_value("write-takes"), flag("report"),
                 library_default(global_int("n-fft"), sonare::kDefaultSilenceFrameLength),
                 library_default(global_int("hop-length"), sonare::kDefaultSilenceHopLength)},
                {}, nullptr, kUnboundedPositionals);
    add_command(commands, "normalize", true,
                {with_domain(string_value("mode", "peak"), choices_of({"peak", "rms"})),
                 with_stated_default(number_value("target-db"),
                                     stated_number(sonare::kDefaultPeakTargetDb) + " (peak) or " +
                                         stated_number(sonare::kDefaultRmsTargetDb) + " (rms)"),
                 required_output()},
                {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "fade", true,
                {with_domain(number_value("fade-in-sec"), at_least(0.0)),
                 with_domain(number_value("fade-out-sec"), at_least(0.0)), required_output()},
                {}, validate_fade_sides);
    add_command(commands, "filter", true,
                {with_domain(string_value("type", true), choices_of(filter_type_tokens())),
                 library_default(with_domain(int_value("order"), choices_of({"2", "4"})), "2"),
                 with_domain(number_value("cutoff-hz"), greater_than(0.0)),
                 with_domain(number_value("center-hz"), greater_than(0.0)),
                 with_domain(number_value("bandwidth-hz"), greater_than(0.0)), required_output(),
                 flag("zero-phase")},
                {}, validate_filter_frequencies);
    add_command(commands, "resample", true,
                {with_domain(required_int("target-sample-rate"),
                             between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate)),
                 required_output()});
    // The generators accept any positive rate, so a rate the library's own decoder
    // refuses produces a file none of these commands can read back. `resample`
    // already bounds its target rate here for the same reason; these three are the
    // rest of that family.
    add_command(commands, "tone", false,
                {number_value("frequency-hz", true),
                 library_default(
                     with_domain(int_value("sample-rate"),
                                 between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate)),
                     "22050"),
                 library_default(number_value("duration-sec"), "1.0"),
                 library_default(number_value("phase"), "0.0"),
                 library_default(number_value("amplitude"), "1.0"), required_output()});
    add_command(commands, "chirp", false,
                {library_default(
                     with_domain(int_value("sample-rate"),
                                 between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate)),
                     "22050"),
                 library_default(number_value("duration-sec"), "1.0"), required_output(),
                 flag("exponential"), with_domain(global_number("fmin-hz", 0.0), at_least(0.0)),
                 with_domain(number_value("fmax-hz", true, true), greater_than(0.0))},
                {}, validate_chirp_sweep);
    add_command(
        commands, "clicks", false,
        {string_value("times", true),
         library_default(
             with_domain(int_value("sample-rate"),
                         between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate)),
             "22050"),
         with_stated_default(
             with_domain(
                 int_value("length"),
                 between(1.0, static_cast<double>(sonare::resource::kMaxOfflineAudioSamples))),
             "through the last click and its decay"),
         library_default(number_value("frequency-hz"), "1000.0"),
         library_default(number_value("click-duration-sec"), "0.1"), required_output()},
        {}, validate_click_times);

#ifdef SONARE_WITH_MASTERING
    // The library's utility.gain processor, so the command takes the processor's published value.
    add_command(commands, "gain", true, {number_value("gain-db", true), required_output()});
    // "--chain-config" names a complete chain config in the core's own
    // `{params, version}` form.
    add_command(
        commands, "mastering", true,
        {with_domain(nullable_string_value("preset"), choices_of(mastering::api::preset_names())),
         path_value("chain-config"), number_value("target-lufs", -14.0),
         number_value("ceiling-dbtp", -1.0), string_value("params"), bits_value(),
         path_value("report"), output_value(), flag("assistant"), flag("enable-repair"),
         flag("explain"),
         // The remaining AssistantConfig fields. `prefer_streaming_safe`
         // defaults to true, so the reachable control is the one that
         // turns it off -- a `--prefer-streaming-safe` flag would only
         // ever restate the default. `--speech-mono-amount` carries no
         // domain because the suggester clamps it to [0, 1] rather than
         // refusing an outside value.
         target_platform_value(), flag("no-streaming-safe"),
         number_value("speech-mono-amount", 1.0)},
        {}, validate_mastering_arguments, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "mastering-normalize-loudness", true,
                {number_value("target-lufs", -14.0), number_value("ceiling-dbtp", -1.0),
                 bits_value(), true_peak_oversample_value(), path_value("report"), output_value()},
                {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(
        commands, "mastering-processor", true,
        {with_domain(required_string("processor"), choices_of(mastering::api::processor_names())),
         string_value("params"), bits_value(), output_value()},
        {}, validate_processor_params, 0, /*preserves_stereo_input=*/true);
    add_command(
        commands, "eq", true,
        {string_value("params"),
         // Each of these indexes a closed enumeration in mastering/eq/eq_band.h
         // (EqBandType, BiquadCoeffMode, StereoPlacement, PhaseMode) or in
         // LinearPhaseEqConfig::Resolution. The bound is the enumerator count,
         // so an index past the end is refused instead of mapping to the first
         // enumerator. `type` takes it from the enum's last enumerator.
         with_domain(int_value("type", 0),
                     enum_index(0, static_cast<int>(sonare::mastering::eq::EqBandType::AllPass))),
         number_value("frequency-hz", 1000.0), number_value("gain-db", 0.0), number_value("q", 1.0),
         with_domain(int_value("coeff-mode", 0), enum_index(0, 1)), int_value("slope-db-oct", 12),
         with_domain(int_value("placement", 0), enum_index(0, 4)),
         number_value("threshold-db", -24.0), number_value("ratio", 2.0),
         number_value("range-db", -6.0), number_value("attack-ms", 5.0),
         number_value("release-ms", 50.0), number_value("detector-delay-ms", 0.0),
         number_value("sidechain-freq-hz", -1.0), number_value("sidechain-q", 1.0),
         with_domain(int_value("phase-mode", 1), enum_index(0, 3)),
         with_domain(int_value("resolution", 0), enum_index(0, 5)), number_value("gain-scale", 1.0),
         number_value("output-gain-db", 0.0), number_value("output-pan", 0.0), bits_value(),
         output_value(), flag("proportional-q"), flag("dynamic"), flag("auto-threshold"),
         flag("auto-gain")},
        {}, validate_eq_params, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "mastering-pair-processor", true,
                {with_domain(required_string("processor"),
                             choices_of(mastering::api::pair_processor_names())),
                 required_path("reference"), string_value("params"), bits_value(), output_value()},
                {}, validate_params_grammar);
    add_command(commands, "mastering-pair-analyze", true,
                {with_domain(required_string("analysis"),
                             choices_of(mastering::api::pair_analysis_names())),
                 required_path("reference"), string_value("params")},
                {}, validate_params_grammar);
    add_command(commands, "mastering-stereo-analyze", true,
                {with_domain(required_string("analysis"),
                             choices_of(mastering::api::stereo_analysis_names())),
                 required_path("reference"), string_value("params")},
                {}, validate_params_grammar);
    // `--config-out` writes the suggested chain in the core's own chain-config
    // form, which is what `mastering --chain-config` reads back -- the same
    // role `suggest-mix --scene-out` plays for a mix scene.
    // The assistant options `mastering --assistant` declares, with the same
    // names, defaults and domains; --params still carries them as flat keys.
    add_command(
        commands, "mastering-suggest", true,
        {string_value("params"), string_value("config-out", ""), number_value("target-lufs", -14.0),
         number_value("ceiling-dbtp", -1.0), flag("enable-repair"),
         with_domain(nullable_string_value("preset"), choices_of(mastering::api::preset_names())),
         target_platform_value(), flag("no-streaming-safe"),
         number_value("speech-mono-amount", 1.0)},
        {}, validate_assistant_params);
    add_command(commands, "mastering-processors", false, {});
    add_command(commands, "mastering-pair-processors", false, {});
    add_command(commands, "mastering-pair-analyses", false, {});
    add_command(commands, "mastering-stereo-analyses", false, {});
    add_command(commands, "mastering-presets", false, {});
    add_command(commands, "mastering-profile", true, {string_value("params")}, {},
                validate_params_grammar);
    add_command(commands, "mastering-streaming", true,
                {nullable_string_value("platforms"), path_value("platforms-file")}, {},
                validate_streaming_platforms);
    add_command(
        commands, "repair", true,
        {with_domain(nullable_string_value("preset"), choices_of(mastering::api::preset_names())),
         string_value("stages"), string_value("params"), bits_value(), output_value(),
         flag("detect"), flag("explain")},
        {}, validate_repair_arguments, 0, /*preserves_stereo_input=*/true);
#endif
#ifdef SONARE_WITH_MIXING
    add_command(commands, "mix", false,
                {path_value("scene"), nullable_string_value("preset"), repeatable_path("input"),
                 with_domain(int_value("sample-rate", kCliMixerSampleRate),
                             between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate)),
                 output_value()},
                {}, validate_mix_sources);
    add_command(commands, "mix-strip", true,
                {number_value("input-trim-db", 0.0), number_value("fader-db", 0.0),
                 with_domain(number_value("pan", 0.0), between(-1.0, 1.0)),
                 with_domain(string_value("pan-mode", "balance"),
                             choices_of({"balance", "stereo-pan", "dual-pan"})),
                 with_domain(number_value("width", 1.0), between(0.0, 2.0)), output_value()},
                {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "mixing-presets", false, {});
    // The advertised default has to be one the command can actually run: an
    // empty string reaches the preset lookup and fails, and the handler's own
    // fallback never applied because the registry default wins over it.
    add_command(commands, "mixing-preset", false, {string_value("preset", "vocalReverbSend")});
#endif
#ifdef SONARE_WITH_MIXING_ASSISTANT
    // `--tempo-bpm` is a string rather than a number because `auto` is one of
    // its values: the tempo is either stated or measured from the first input,
    // and a numeric option cannot spell the second.
    add_command(commands, "suggest-mix", false,
                {repeatable_path("input"), int_value("sample-rate", kCliMixerSampleRate),
                 string_value("params", ""),
                 tempo(make_option("tempo-bpm", CliOptionArity::RequiredValue,
                                   CliOptionScalarType::String, null_default())),
                 string_value("scene-out", "")},
                {}, validate_suggest_mix);
#endif

    // Feature leaves.
    // A lower band edge of 0 Hz is a real edge, so `fmin-hz` refuses only a negative;
    // an omitted `fmax-hz` is the Nyquist frequency, so a spelled one must be above 0.
    // `n-mels` needs none -- the filterbank's own ceiling refuses an oversized band
    // count for every command and every surface at once.
    add_command(
        commands, "mel", true,
        {library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(global_int("n-mels"), "128"),
         with_domain(library_default(global_number("fmin-hz"), "0.0"), at_least(0.0)),
         with_stated_default(with_domain(number_value("fmax-hz", false, true), greater_than(0.0)),
                             "the Nyquist frequency"),
         flag("htk")});
    add_command(commands, "chroma", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512")});
    add_command(commands, "tonnetz", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512")});
    add_command(commands, "spectral", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512")});
    // Every domain the Python CLI declares for this command.
    add_command(
        commands, "pitch", true,
        {flag("frames"),
         with_domain(string_value("algorithm", "pyin"), choices_of({"yin", "pyin"})),
         library_default(with_domain(number_value("threshold"), above_zero_up_to(1.0)), "0.1"),
         with_domain(library_default(global_int("hop-length"), "512"), greater_than(0.0)),
         library_default(with_domain(number_value("f0-min-hz"), greater_than(0.0)), "65.0"),
         library_default(with_domain(number_value("f0-max-hz"), greater_than(0.0)), "2093.0")},
        {}, &validate_pitch_frequency_order);
    add_command(commands, "onset-strength", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128")});
    for (const char* path : {"fourier-tempogram", "tempogram-ratio"})
      add_command(commands, path, true,
                  {library_default(int_value("win-length"), sonare::TempogramConfig{}.win_length),
                   library_default(global_int("hop-length"), "512")});
    add_command(commands, "tempogram", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128"),
                 library_default(int_value("win-length"), "384")});
    add_command(commands, "plp", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128"), tempo(number_value("bpm-min")),
                 tempo(number_value("bpm-max")), library_default(int_value("win-length"), "384")});
    add_command(commands, "nnls-chroma", true,
                {with_domain(library_default(global_int("hop-length"), "512"), cqt_hop_domain())});
    // An omitted lower bound is the library's (C1); a constant-Q bin cannot sit at
    // or below 0 Hz, so a spelled one must be above it.
    add_command(commands, "cqt", true,
                {library_default(with_domain(int_value("n-bins"), at_least(1.0)), "84"),
                 library_default(with_domain(int_value("bins-per-octave"), at_least(1.0)), "12"),
                 with_domain(library_default(global_int("hop-length"), "512"), cqt_hop_domain()),
                 with_domain(number_value("fmin-hz", false, true), greater_than(0.0))});
    add_command(
        commands, "vqt", true,
        {library_default(with_domain(int_value("n-bins"), at_least(1.0)), "84"),
         library_default(with_domain(int_value("bins-per-octave"), at_least(1.0)), "12"),
         with_stated_default(with_domain(number_value("gamma"), at_least(0.0)),
                             "derived from the ERB bandwidths"),
         library_default(with_domain(number_value("filter-scale"), greater_than(0.0)), "1.0"),
         with_domain(library_default(global_int("hop-length"), "512"), cqt_hop_domain()),
         with_domain(number_value("fmin-hz", false, true), greater_than(0.0))});
    add_command(
        commands, "mel-to-audio", true,
        {library_default(with_domain(int_value("n-iter"), at_least(1.0)),
                         sonare::constants::kDefaultGriffinLimIterations),
         required_output(), library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(global_int("n-mels"), "128"),
         with_domain(library_default(global_number("fmin-hz"), "0.0"), at_least(0.0)),
         with_stated_default(with_domain(number_value("fmax-hz", false, true), greater_than(0.0)),
                             "the Nyquist frequency")});
    add_command(
        commands, "mfcc-to-audio", true,
        {library_default(with_domain(int_value("n-mfcc"), at_least(1.0)),
                         sonare::kMfccDefaultCount),
         library_default(with_domain(int_value("n-iter"), at_least(1.0)),
                         sonare::constants::kDefaultGriffinLimIterations),
         required_output(), library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(global_int("n-mels"), "128"),
         with_domain(library_default(global_number("fmin-hz"), "0.0"), at_least(0.0)),
         with_stated_default(with_domain(number_value("fmax-hz", false, true), greater_than(0.0)),
                             "the Nyquist frequency")},
        {}, validate_mfcc_to_audio_counts);
    add_command(commands, "acoustic", true,
                {flag("ir"), library_default(int_value("n-octave-bands"), "6"),
                 library_default(number_value("min-decay-db"), "30.0"),
                 library_default(number_value("noise-floor-margin-db"), "10.0")});

#ifdef SONARE_WITH_ACOUSTIC_SIM
    // The C `seed` field's own width rather than a policy bound: every other
    // surface accepts the whole uint32 range, and this front-end reached only
    // the lower half while `int` was the widest thing its parser produced.
    // Exact as a double. Declared inside the gate its only two readers sit in,
    // because the CLI defines that macro from BUILD_ACOUSTIC_SIM AND BUILD_FX
    // and an unconditional declaration is unused in every build missing either.
    constexpr double kMaxCliSeed = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    add_command(commands, "estimate-room", true,
                {library_default(number_value("aspect-lw"), "1.0"),
                 library_default(number_value("aspect-lh"), "1.0"),
                 library_default(number_value("reference-absorption"), "0.15"), flag("sabine"),
                 library_sized(int_value("n-octave-bands"))});
    // `absorption` reaches uniform_shoebox, which clamps to [0, 0.999] rather than
    // calling validate_material_coefficient beside it, so the C entry point's
    // refusal of anything outside [0, 1] has no counterpart on this path. `seed`
    // keeps 0 as the library-default sentinel the C ABI spells the same way, and
    // refuses the negatives the handler currently swallows into that default.
    add_command(commands, "synthesize-rir", false,
                {library_default(number_value("length"), "7.0"),
                 library_default(number_value("width"), "5.0"),
                 library_default(number_value("height"), "3.0"),
                 library_default(with_domain(number_value("absorption"), between(0.0, 1.0)), "0.2"),
                 with_stated_default(number_value("source-x"), "scaled to the room"),
                 with_stated_default(number_value("source-y"), "scaled to the room"),
                 with_stated_default(number_value("source-z"), "scaled to the room"),
                 with_stated_default(number_value("listener-x"), "scaled to the room"),
                 with_stated_default(number_value("listener-y"), "scaled to the room"),
                 with_stated_default(number_value("listener-z"), "scaled to the room"),
                 with_domain(int_value("sample-rate", 48000),
                             between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate)),
                 library_default(int_value("ism-order"), "3"),
                 library_default(with_domain(int_value("seed"), between(0.0, kMaxCliSeed)), "1"),
                 with_stated_default(with_domain(number_value("max-sec"), greater_than(0.0)),
                                     "natural length"),
                 required_output(), flag("sabine")},
                {}, &validate_room_placement);
    add_command(
        commands, "room-morph", true,
        {number_value("length", 7.0), number_value("width", 5.0), number_value("height", 3.0),
         library_default(with_domain(number_value("absorption"), between(0.0, 1.0)), "0.2"),
         with_stated_default(number_value("source-x"), "scaled to the room"),
         with_stated_default(number_value("source-y"), "scaled to the room"),
         with_stated_default(number_value("source-z"), "scaled to the room"),
         with_stated_default(number_value("listener-x"), "scaled to the room"),
         with_stated_default(number_value("listener-y"), "scaled to the room"),
         with_stated_default(number_value("listener-z"), "scaled to the room"),
         library_default(number_value("suppression"), "0.5"),
         library_default(number_value("wet"), "0.5"), library_default(int_value("ism-order"), "3"),
         library_default(with_domain(int_value("seed"), between(0.0, kMaxCliSeed)), "1"),
         with_stated_default(with_domain(number_value("max-sec"), greater_than(0.0)),
                             "natural length"),
         library_default(with_domain(number_value("receiver-spacing"), above_zero_up_to(4.0)),
                         "0.5"),
         required_output(), flag("sabine")},
        {}, &validate_room_placement, 0, /*preserves_stereo_input=*/true);
#endif

    // Metering and scalar utility leaves.
    // Measures every channel as BS.1770 sums them, the loudness `master` reports for its input.
    add_command(commands, "lufs", true, {flag("series")}, {}, nullptr, 0,
                /*preserves_stereo_input=*/false, /*preserves_multichannel_input=*/true);
    add_command(
        commands, "meter", true,
        {with_stated_default(with_domain(number_value("clip-threshold-db"), between(-120.0, 0.0)),
                             "the level of a linear 0.999"),
         library_default(
             with_domain(int_value("oversample"), choices_of({"1", "2", "4", "8", "16"})), "4")});
    add_command(
        commands, "clipping", true,
        {with_stated_default(with_domain(number_value("threshold-db"), between(-120.0, 0.0)),
                             "the level of a linear 0.999"),
         library_default(with_domain(int_value("min-region"), at_least(0.0)), "1")});
    add_command(
        commands, "dynamic-range", true,
        {library_default(with_domain(number_value("window-sec"), greater_than(0.0)), "3.0"),
         library_default(with_domain(number_value("hop-sec"), greater_than(0.0)), "1.0"),
         library_default(with_domain(number_value("low-percentile"), between(0.0, 1.0)), "0.1"),
         library_default(with_domain(number_value("high-percentile"), between(0.0, 1.0)), "0.95")},
        {}, validate_dynamic_range_percentiles);
    add_command(commands, "stereo", true, {path_value("reference")});
    add_command(commands, "phase", true, {path_value("reference")});
    // Without --n-fft the conversion applies no centring offset, as the library does.
    add_command(commands, "frames-to-samples", false,
                {int_value("frames", true),
                 with_stated_default(with_domain(int_value("n-fft", false, true), at_least(1.0)),
                                     "no centring offset"),
                 with_domain(library_default(global_int("hop-length"), "512"), at_least(1.0))});
    add_command(commands, "samples-to-frames", false,
                {int_value("samples", true),
                 with_stated_default(with_domain(int_value("n-fft", false, true), at_least(1.0)),
                                     "no centring offset"),
                 with_domain(library_default(global_int("hop-length"), "512"), at_least(1.0))});
    add_command(commands, "power-to-db", false,
                {string_value("values", true), library_default(number_value("ref"), "1.0"),
                 library_default(number_value("amin"), "1e-10"),
                 library_default(number_value("top-db"), "80.0")},
                {}, validate_float_values);
    add_command(commands, "amplitude-to-db", false,
                {string_value("values", true), library_default(number_value("ref"), "1.0"),
                 library_default(number_value("amin"), "1e-05"),
                 library_default(number_value("top-db"), "80.0")},
                {}, validate_float_values);
    add_command(commands, "db-to-power", false,
                {string_value("values", true), library_default(number_value("ref"), "1.0")}, {},
                validate_float_values);
    add_command(commands, "db-to-amplitude", false,
                {string_value("values", true), library_default(number_value("ref"), "1.0")}, {},
                validate_float_values);
    add_command(commands, "frame-signal", false,
                {string_value("values", true), int_value("frame-length", true),
                 int_value("hop-length", true, true)},
                {}, validate_float_values);
    add_command(commands, "pad-center", false,
                {string_value("values", true), with_domain(int_value("size", true), at_least(1.0)),
                 library_default(number_value("pad-value"), "0.0")},
                {}, validate_float_values);
    add_command(commands, "fix-length", false,
                {string_value("values", true), with_domain(int_value("size", true), at_least(1.0)),
                 library_default(number_value("pad-value"), "0.0")},
                {}, validate_float_values);
    add_command(
        commands, "fix-frames", false,
        {string_value("values", true), library_default(int_value("x-min"), "0"),
         with_stated_default(with_domain(int_value("x-max"), at_least(0.0)), "no upper bound"),
         flag("no-pad")},
        {}, validate_int_values);
    add_command(commands, "peak-pick", false,
                {string_value("values", true), int_value("pre-max", true),
                 int_value("post-max", true), int_value("pre-avg", true),
                 int_value("post-avg", true), number_value("delta", true), int_value("wait", true)},
                {}, validate_float_values);
    add_command(commands, "vector-normalize", false,
                {string_value("values", true),
                 // Indexes NormType (inf, L1, L2, power); an index past the end used to
                 // fall through to the inf norm and report success.
                 library_default(with_domain(int_value("norm-type"), enum_index(0, 3)), "0"),
                 library_default(number_value("threshold"), "0.0")},
                {}, validate_float_values);
    add_command(commands, "pcen", false,
                {string_value("values", true), library_default(int_value("sample-rate"), "22050"),
                 library_default(number_value("time-constant"), "0.4"),
                 library_default(number_value("gain"), "0.98"),
                 library_default(number_value("bias"), "2.0"),
                 library_default(number_value("power"), "0.5"),
                 library_default(number_value("eps"), "1e-06"),
                 with_domain(int_value("n-bins", true), at_least(1.0)),
                 with_domain(int_value("n-frames", true), at_least(1.0)),
                 library_default(int_value("hop-length", false, true), "512")},
                {}, validate_pcen_values);
    add_command(commands, "info", true, {});

    add_command(commands, "version", false, {});
    add_command(commands, "doctor", false, {});
    add_command(commands, "system-info", false, {});

#ifdef SONARE_WITH_ARRANGEMENT
    // Exactly eleven project leaves; there is deliberately no broad `project`
    // option row. Each takes its subcommand as the first positional, and the
    // ones that read a file take it as the second (`input` names it), so they
    // are registered through a helper rather than each restating that arity.
    const auto add_project_command =
        [&commands](const char* path, std::vector<CliOptionSpec> options,
                    CliCommandValidator validate = nullptr, const char* input = nullptr) {
          add_command(commands, path, false, std::move(options), {}, validate, input ? 2 : 1, false,
                      false, input ? input : "");
        };
    add_project_command("project.abi", {});
    add_project_command("project.synth-presets", {});
    add_project_command("project.new",
                        {with_stated_default(with_domain(int_value("sample-rate"),
                                                         between(sonare::kMinAudioSampleRate,
                                                                 sonare::kMaxAudioSampleRate)),
                                             "the project's own"),
                         required_output()});
    add_project_command("project.validate", {flag("strict"), output_value()}, nullptr,
                        kProjectJsonPositional);
    add_project_command("project.compile", {}, nullptr, kProjectJsonPositional);
    // `--audio` is one assignment per occurrence rather than a delimited list,
    // as `--set` and `--edit` are: the source id written with the path keeps the
    // pairing in one token, and a path may carry whatever separator a fold would
    // have to pick. Its type is String rather than Path because the value is an
    // assignment, not a bare path.
    add_project_command(
        "project.bounce",
        {required_output(), int_value("sample-rate"),
         project_length(int64_width(int_value("frames"))), master_width(int_value("channels")),
         library_default(int_value("instrument-latency-samples"), "0"), optional_string("synth"),
         string_value("audio", false, true), flag("resolve-audio")},
        &validate_project_bounce_arguments, kProjectJsonPositional);
    // `align-takes` reads the source FILES rather than binding PCM, so it shares
    // `--audio` / `--resolve-audio` with `bounce` and nothing else. The C entry spells
    // "library value" as 0 for the two resolution options, so an explicit 0 is refused
    // here and only an absent flag sends it; their grid stays with the core.
    add_project_command(
        "project.align-takes",
        {required_output(),
         with_domain(int_value("reference-source", true),
                     between(1.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max()))),
         string_value("audio", false, true), flag("resolve-audio"),
         with_domain(library_default(global_int("hop-length"), "512"), greater_than(0.0)),
         library_default(with_domain(int_value("bins-per-octave"), greater_than(0.0)), "12")},
        &validate_project_align_takes, kProjectJsonPositional);
    // `project bounce` with the synth pinned on, as a top-level leaf: it takes
    // no subcommand positional, and `--synth` is a plain value rather than an
    // optional flag because the rendering always goes through NativeSynth and
    // only the preset is in question.
    add_command(
        commands, "midi-render", false,
        {required_output(), int_value("sample-rate"),
         project_length(int64_width(int_value("frames"))), master_width(int_value("channels")),
         library_default(int_value("instrument-latency-samples"), "0"),
         with_stated_default(nullable_string_value("synth"), "GM program/channel routing")},
        {}, &validate_midi_render_arguments, 1, false, false, kProjectJsonPositional);
    // Every numeric here means "keep the library default" when absent. The ranges a
    // single value can be refused by are declared; the reattack ratio's "0 or above 1" and the
    // rules joining options are the command validator's.
    const sonare::editing::note_model::TranscribeConfig transcribe_defaults;
    add_command(
        commands, "transcribe", true,
        {required_output(),
         with_stated_default(tempo(number_value("tempo-bpm")), "detected from the audio"),
         flag("polyphonic"),
         library_default(with_domain(number_value("reference-hz"), greater_than(0.0)),
                         transcribe_defaults.reference_hz),
         with_domain(number_value("f0-min-hz"), greater_than(0.0)),
         with_domain(number_value("f0-max-hz"), greater_than(0.0)),
         // The segmenter's own shortest note; the singing and polyphonic values are the
         // transcriber's, which no header carries, so the derived-default row pins them.
         with_stated_default(
             with_domain(number_value("min-note-ms"), greater_than(0.0)),
             stated_number(sonare::editing::pitch_editor::NoteSegmenterConfig{}.min_note_ms) +
                 " (segments), " + stated_number(45.0) + " (singing) or " + stated_number(60.0) +
                 " (polyphonic)"),
         library_default(
             with_domain(number_value("segmentation-threshold-cents"), greater_than(0.0)),
             transcribe_defaults.segmentation_threshold_cents),
         // The polyphonic values below are the transcriber's own and appear in no
         // header, so each one's stated-default row is what pins it.
         library_default(with_domain(int_value("max-polyphony"), between(1.0, 64.0)), 10),
         with_domain(int_value("min-note-division"), between(1.0, 128.0)),
         library_default(with_domain(number_value("min-frame-peak-ratio"), between(0.0, 1.0)),
                         "0.2"),
         library_default(with_domain(number_value("min-ridge-peak-ratio"), between(0.0, 1.0)),
                         "0.1"),
         library_default(number_value("reattack-ratio"), "2.0"),
         library_default(with_domain(number_value("velocity-floor-db"), less_than(0.0)),
                         transcribe_defaults.velocity_floor_db),
         with_domain(int_value("fixed-velocity"), between(1.0, 127.0)),
         library_default(with_domain(int_value("channel"), between(0.0, 15.0)), "0"),
         library_default(with_domain(nullable_string_value("monophonic-model"),
                                     choices_of({"segments", "singing"})),
                         "segments")},
        {}, validate_transcribe);
    // The reference melody is read by the arrangement library and assigned by the
    // pitch editor, so the leaf exists only where both do.
    //
    // The two bounds are the null-default overload rather than a restated 0.5 and
    // 12: both values are legal inputs, so an absent flag cannot be spelled as a
    // sentinel and the handler leaves NoteTargetAssignConfig's own field alone.
#ifdef SONARE_WITH_PITCH_EDITOR
    add_command(
        commands, "tune-to-midi", true,
        {required_path("reference-smf"), with_domain(int_value("track", 0), at_least(0.0)),
         library_default(with_domain(nullable_string_value("unmatched-policy"),
                                     choices_of({"leave", "mute", "nearest"})),
                         "leave"),
         with_domain(number_value("min-overlap-ratio"), between(0.0, 1.0)),
         with_domain(number_value("max-correction-semitones"), at_least(0.0)), required_output()});
#endif
    add_project_command("project.export-smf", {required_output()}, nullptr, kProjectJsonPositional);
    add_project_command("project.import-smf", {required_output()}, nullptr, "song.mid");
    add_project_command("project.export-midi2", {required_output()}, nullptr,
                        kProjectJsonPositional);
    add_project_command("project.import-midi2", {required_output()}, nullptr, "clip.midi2");
#endif
#ifdef SONARE_WITH_PLAYBACK
    // Movie-audio playback renderer: mono/stereo/5.1/7.1 PCM in, either
    // speaker-layout audio (upmix, calibration, bass management) or binaural
    // headphones audio out. `--input-layout` reaches the renderer's own
    // `input.layout` verbatim; the input file's actual channel count is a
    // property of the file rather than an option value, so it is checked by
    // the handler once the file is open rather than by a domain here.
    const sonare::playback::RealtimeConfig playback_defaults;
    add_command(commands, "playback", true,
                {library_default(with_domain(nullable_string_value("target"),
                                             choices_of({"headphones", "stereo", "5.1", "7.1"})),
                                 "headphones"),
                 library_default(with_domain(nullable_string_value("input-layout"),
                                             choices_of({"auto", "mono", "stereo", "5.1", "7.1"})),
                                 "auto"),
                 path_value("config"), flag("no-upmix"),
                 library_default(with_domain(number_value("night"), between(0.0, 1.0)),
                                 playback_defaults.night_amount),
                 library_default(with_domain(number_value("dialogue-db"), between(-12.0, 12.0)),
                                 playback_defaults.dialogue_level_db),
                 with_stated_default(with_domain(number_value("program-lufs"), between(-70.0, 0.0)),
                                     "measured from the whole input"),
                 library_default(with_domain(number_value("target-lufs"), between(-40.0, -5.0)),
                                 playback_defaults.target_lufs),
                 with_domain(nullable_string_value("room"),
                             choices_of({"none", "living-room", "home-theater", "screening-room"})),
                 path_value("hrtf"), required_output()},
                {}, nullptr, 0, /*preserves_stereo_input=*/false,
                /*preserves_multichannel_input=*/true);
#endif
    return commands;
  }();
  return registry;
}

}  // namespace

const std::vector<CliCommandSpec>& cli_command_registry() { return build_cli_registry(); }

std::string cli_option_stated_default(const CliOptionSpec& option) {
  if (!option.stated_default.empty()) return option.stated_default;
  if (option.required || option.repeatable) return "";
  switch (option.default_value.kind) {
    case CliOptionDefaultKind::Integer:
      return std::to_string(option.default_value.integer_value);
    case CliOptionDefaultKind::Number:
      return stated_number(option.default_value.number_value);
    case CliOptionDefaultKind::String:
      return option.default_value.string_value;
    case CliOptionDefaultKind::Null:
    case CliOptionDefaultKind::Boolean:
    case CliOptionDefaultKind::StringArray:
      return "";
  }
  return "";
}
