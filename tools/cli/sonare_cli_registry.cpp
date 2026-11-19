#include "sonare_cli_registry.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/section_analyzer.h"
#include "core/audio.h"
#include "core/channel_layout.h"
#include "effects/normalize.h"
#include "effects/silence.h"
#include "util/constants.h"

#ifdef SONARE_WITH_ARRANGEMENT
#include "editing/note_model/note_transcriber.h"
#include "editing/pitch_editor/note_segmenter.h"
#endif
#ifdef SONARE_WITH_PLAYBACK
#include "playback/config.h"
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
CliOptionSpec repeatable_path(const char* name) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Path,
                     string_array_default(), {}, {}, false, true, false, true);
}

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

CliOptionDomain greater_than(double minimum, CliOptionDomainStage stage) {
  CliOptionDomain domain;
  domain.has_minimum = true;
  domain.minimum = minimum;
  domain.exclusive_minimum = true;
  domain.stage = stage;
  return domain;
}

CliOptionDomain at_least(double minimum, CliOptionDomainStage stage) {
  CliOptionDomain domain;
  domain.has_minimum = true;
  domain.minimum = minimum;
  domain.stage = stage;
  return domain;
}

CliOptionDomain above_zero_up_to(double maximum, CliOptionDomainStage stage) {
  CliOptionDomain domain = greater_than(0.0, stage);
  domain.has_maximum = true;
  domain.maximum = maximum;
  return domain;
}

// An inclusive numeric range. Expressing one as a choices list is exact for a
// dozen enumerators and absurd for a twelve-bit mask, so a range that is a
// range says so.
CliOptionDomain between(double minimum, double maximum, CliOptionDomainStage stage) {
  CliOptionDomain domain = at_least(minimum, stage);
  domain.has_maximum = true;
  domain.maximum = maximum;
  return domain;
}

CliOptionDomain choices_of(std::vector<std::string> values, CliOptionDomainStage stage) {
  CliOptionDomain domain;
  domain.choices = std::move(values);
  domain.stage = stage;
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
// see. `[lowest, highest]` is inclusive, and the refusal takes the
// invalid-parameter class because the caller spelled a number correctly and
// named a target that does not exist, which is what
// mastering::api::checked_enum already reports for the same mistake arriving
// through --params.
CliOptionDomain enum_index(int lowest, int highest, CliOptionDomainStage stage) {
  std::vector<std::string> values;
  values.reserve(static_cast<size_t>(highest - lowest + 1));
  for (int value = lowest; value <= highest; ++value) values.push_back(std::to_string(value));
  return choices_of(std::move(values), stage);
}

CliOptionSpec with_domain(CliOptionSpec spec, CliOptionDomain domain) {
  spec.domain = std::move(domain);
  return spec;
}

// One spelling of a position given in samples or in seconds, from a builder with
// no default: absence lets the handler apply the library's own.
CliOptionSpec position(CliOptionSpec spec) {
  return with_domain(std::move(spec), at_least(0.0, CliOptionDomainStage::Parameter));
}

// A tempo, on every command that takes one, is refused at the flag unless it is
// greater than 0, so no command reaches the library with a tempo nobody can play.
CliOptionSpec tempo(CliOptionSpec spec) {
  return with_domain(std::move(spec), greater_than(0.0, CliOptionDomainStage::Usage));
}

// A render length or channel count the library chooses when the flag is left out;
// a value the caller spells is refused at the flag unless it is greater than 0.
CliOptionSpec library_sized(CliOptionSpec spec) {
  return with_domain(std::move(spec), greater_than(0.0, CliOptionDomainStage::Usage));
}

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

/// Marks an option required and declares which exit class its absence reports.
CliOptionSpec required_with_stage(CliOptionSpec spec, CliOptionDomainStage stage) {
  spec.required = true;
  spec.default_value = null_default();
  spec.required_stage = stage;
  return spec;
}

CliOptionSpec output_value(bool required = false) {
  return path_value("output", required, true, true, {"o"});
}

// An output file the command cannot run without. The Python CLI leaves `-o`
// optional in its parser and rejects the absent case inside the handler with
// its invalid-parameter code, so the native contract declares the same class
// here rather than letting the registry's default usage class diverge from it.
CliOptionSpec required_output() {
  return required_with_stage(output_value(), CliOptionDomainStage::Parameter);
}

#ifdef SONARE_WITH_MASTERING
// Output bit depth for every command that writes a WAV. The Python CLI parses
// it as a plain int and rejects anything but 16/24 inside the handler, which is
// its invalid-parameter class; declaring the same set and stage here makes the
// check unconditional (it used to sit inside the `-o` branch) without changing
// which code either CLI reports.
CliOptionSpec bits_value() {
  return with_domain(int_value("bits", 16),
                     choices_of({"16", "24"}, CliOptionDomainStage::Parameter));
}

// The Python CLI declares this one as an argparse `choices=` tuple, which is a
// parse-time rejection, so the native contract has to report it as usage.
CliOptionSpec true_peak_oversample_value() {
  return with_domain(int_value("true-peak-oversample", 4),
                     choices_of({"1", "2", "4", "8", "16"}, CliOptionDomainStage::Usage));
}

// Delivery target for the mastering assistant. The accepted names come from
// mastering::assistant::platform_names() rather than a list restated here, which
// is the derivation platform_targets.h describes: appending a row to that table
// extends this option with no edit, and no spelling can drift from it. The
// default matches AssistantConfig::target_platform, so an invocation without the
// option keeps the streaming convention it had before the option existed.
CliOptionSpec target_platform_value() {
  return with_domain(
      string_value("target-platform", "streaming"),
      choices_of(sonare::mastering::assistant::platform_names(), CliOptionDomainStage::Usage));
}

// `repair` writes a repaired file, so `--output` is normally required; `--detect`
// trades processing for a report, writes no file, and so refuses one. A plain
// `required_output()` cannot express that the requirement is conditional, so
// the command carries this validator instead.
CliValidationError validate_repair_output(const CliArgs& args) {
  if (args.has("detect") && args.has("output")) {
    return {"--output cannot be combined with --detect", true};
  }
  if (args.has("detect") || args.has("output")) return {};
  return {"--output is required unless --detect is given", true};
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
  return {message.str(), false};
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
  return {message.str(), false};
}

// The detector combines the MFCC and chroma streams frame-for-frame, so with
// neither enabled there is nothing to combine and the novelty curve is
// undefined. A per-option domain cannot express it: either flag is legal alone.
CliValidationError validate_boundary_feature_streams(const CliArgs& args) {
  if (!args.has("no-mfcc") || !args.has("no-chroma")) return {};
  return {
      "--no-mfcc and --no-chroma cannot both be given: boundary detection "
      "needs at least one feature stream",
      true};
}

// A position is spelled in samples or in seconds; both at once is refused by
// name, as the library refuses the two keyword spellings.
CliValidationError refuse_both_bound_spellings(const CliArgs& args,
                                               std::initializer_list<const char*> bounds) {
  for (const char* bound : bounds) {
    const std::string sample = std::string(bound) + "-sample";
    const std::string sec = std::string(bound) + "-sec";
    if (args.has(sample) && args.has(sec)) {
      return {"give --" + sample + " or --" + sec + ", not both", true};
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
  return {message.str(), true};
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
  return {message.str(), true};
}

// Only when HPSS is on: the separation needs enough overlap to resynthesize, and
// the C entry point refuses a hop below 16 for exactly this pair. The STFT's own
// geometry check does not reach it -- at the 4096-point default a hop of 8
// satisfies `hop <= n_fft / 2` and still starves the separation. A per-option
// domain cannot express it because the same hop is fine without the flag.
CliValidationError validate_key_hpss_hop(const CliArgs& args) {
  if (!args.has("hpss")) return {};
  if (args.hop_length >= sonare::kMinKeyHpssHopLength) return {};
  std::ostringstream message;
  message << "--hop-length must be at least " << sonare::kMinKeyHpssHopLength
          << " with --hpss (got " << args.hop_length << ")";
  return {message.str(), true};
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
  return {"invalid value for --channels: " + std::to_string(channels) + " (expected one of " +
              std::to_string(mono) + ", " + std::to_string(stereo) +
              ", 6 or 8; leave it out for the scene master's width)",
          true};
}
#endif

CliOptionSpec global_int(const char* name, int value) {
  return int_value(name, value, false, true);
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

/// A list of numbers as the CLI reads one back: comma-separated, no spaces.
std::string joined_numbers(const std::vector<int>& values) {
  std::string text;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) text += ',';
    text += std::to_string(values[i]);
  }
  return text;
}

/// A render length the library takes from the project when the flag is left out.
CliOptionSpec project_length(CliOptionSpec spec) {
  return with_stated_default(library_sized(std::move(spec)), "through the last event and its tail");
}

/// A render channel count the library takes from the scene master when the flag is left out.
CliOptionSpec master_width(CliOptionSpec spec) {
  return with_stated_default(library_sized(std::move(spec)), "the scene master's width");
}

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
         library_default(with_domain(number_value("chroma-high-pass-hz"),
                                     at_least(0.0, CliOptionDomainStage::Usage)),
                         "80.0"),
         // An odd meter is only ever reported if its numerator was
         // asked for, so without these the CLI cannot reach one. The
         // list is a string here and validated by the core, which is
         // what already holds the count and range rules.
         library_default(nullable_string_value("meter-candidates"),
                         joined_numbers(sonare::MusicAnalyzerConfig{}.meter_candidate_numerators)),
         // The Python CLI parses this through its positive-integer
         // checker, so the domain and the (parse-time) class match it.
         library_default(with_domain(int_value("meter-denominator"),
                                     greater_than(0.0, CliOptionDomainStage::Usage)),
                         "4")});
    for (const char* path : {"bpm", "beats", "downbeats", "onsets"})
      add_command(commands, path, true, {});
    add_command(commands, "timbre", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128")});
    add_command(
        commands, "key", true,
        {library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"), int_value("candidates"), flag("hpss"),
         flag("loudness-weighted"), library_default(number_value("high-pass-hz"), "80.0"),
         library_default(nullable_string_value("modes"), "major-minor"),
         with_stated_default(nullable_string_value("profile"), "chosen by --genre-hint"),
         with_stated_default(with_domain(nullable_string_value("genre-hint"),
                                         choices_of({"auto", "edm", "electronic", "dance", "pop",
                                                     "classical", "jazz"},
                                                    CliOptionDomainStage::Parameter)),
                             sonare::KeyConfig{}.genre_hint)},
        {}, validate_key_hpss_hop);
    // `smoothing-window` and `hmm-beam-width` carry the C entry point's domains;
    // `min-duration` and `threshold` are checked by ChordAnalyzer itself and so
    // need none. A zero smoothing window is the one the analyzer accepts and the
    // C entry refuses, and a negative beam width is never truncated against, so
    // `--hmm-beam-width -1` silently ran an unbeamed search.
    add_command(
        commands, "chords", true,
        {library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(number_value("min-duration-sec"), "0.3"),
         library_default(with_domain(number_value("smoothing-window"),
                                     greater_than(0.0, CliOptionDomainStage::Parameter)),
                         "2.0"),
         library_default(number_value("threshold"), "0.5"), flag("with-seventh"),
         library_default(
             with_domain(nullable_string_value("chroma-method"),
                         choices_of({"cqt", "stft", "nnls"}, CliOptionDomainStage::Parameter)),
             "cqt"),
         flag("no-beat-sync"), flag("no-hmm"),
         library_default(with_domain(int_value("hmm-beam-width"),
                                     at_least(0.0, CliOptionDomainStage::Parameter)),
                         "24"),
         flag("key-context"), library_default(nullable_string_value("key-root"), "C"),
         library_default(nullable_string_value("key-mode"), "major"), flag("detect-inversions")});
    add_command(commands, "sections", true,
                {library_default(with_domain(number_value("min-duration-sec"),
                                             at_least(0.0, CliOptionDomainStage::Parameter)),
                                 "4.0"),
                 library_default(with_domain(number_value("threshold"),
                                             at_least(0.0, CliOptionDomainStage::Parameter)),
                                 sonare::kSectionBoundaryThreshold),
                 library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512")});
    // DynamicsAnalyzer floors a tiny or negative window to avoid a 0/0 rather
    // than refusing it, so the refusal the other surfaces get has to be here.
    add_command(commands, "dynamics", true,
                {library_default(with_domain(number_value("window-sec"),
                                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                                 "0.4"),
                 library_default(global_int("hop-length"), "512")});
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
    add_command(commands, "melody", true,
                {library_default(with_domain(number_value("threshold"),
                                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                                 "0.1"),
                 library_default(with_domain(int_value("hop-length", false, true),
                                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                                 "256"),
                 library_default(with_domain(number_value("f0-min-hz"),
                                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                                 "65.0"),
                 library_default(with_domain(number_value("f0-max-hz"),
                                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                                 "2093.0")},
                {}, validate_melody_frequency_band);
    // The whole of BoundaryConfig, so a command line can reach the same detector
    // the other surfaces do. `absolute-threshold` is the one that was missing and
    // mattered: `threshold` is relative to the curve's own maximum, so it cannot
    // ask whether anything changed at all, and without the floor stationary
    // material segments every few seconds. The two feature streams are on by
    // default and so are spelled as negations. Each size is refused at zero
    // because the detector has no behaviour there, and the C entry the other
    // surfaces reach refuses the same values with the same class.
    add_command(
        commands, "boundaries", true,
        {with_domain(number_value("threshold", 0.3),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("absolute-threshold", 0.005),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         with_domain(int_value("kernel-size", 64),
                     greater_than(0.0, CliOptionDomainStage::Parameter)),
         with_domain(int_value("n-mfcc", 13), greater_than(0.0, CliOptionDomainStage::Parameter)),
         with_domain(int_value("n-chroma", 12), greater_than(0.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("peak-distance", 2.0),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         // One-sided seconds the relative threshold is measured against; 0 disables it.
         with_domain(number_value("reference-window", 60.0),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         flag("no-mfcc"), flag("no-chroma"), library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512")},
        {}, validate_boundary_feature_streams);

    // Processing leaves.
    // The native spectral backend repairs a non-positive analysis geometry into
    // the librosa defaults instead of refusing it, five lines above a comment
    // saying a geometry that cannot be overlap-added is an error rather than
    // something to repair. These two make the refusal the one a caller gets.
    add_command(commands, "pitch-shift", true,
                {number_value("pitch-semitones"), required_output(),
                 with_domain(library_default(global_int("n-fft"), "2048"),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(library_default(global_int("hop-length"), "n_fft/4"),
                             greater_than(0.0, CliOptionDomainStage::Parameter))});
    add_command(commands, "time-stretch", true,
                {number_value("rate"), required_output(),
                 with_domain(library_default(global_int("n-fft"), "2048"),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(library_default(global_int("hop-length"), "n_fft/4"),
                             greater_than(0.0, CliOptionDomainStage::Parameter))});
    add_command(
        commands, "pitch-correct", true,
        {number_value("current-midi", 69.0), number_value("target-midi", 69.0), required_output()});
    // The MIDI value is the positional, and the three narrowed options carry
    // the ranges the scale quantizer refuses outside of -- the pitch class, the
    // twelve-bit mode mask, and the grid anchor. Each is a value the caller
    // spelled correctly and the library will not act on, so all three take the
    // invalid-parameter class the Python CLI already reports for them.
    add_command(
        commands, "scale-quantize", false,
        {with_domain(int_value("root", 0), between(0.0, 11.0, CliOptionDomainStage::Parameter)),
         with_domain(mode_mask_value("mode-mask", 0xAB5),
                     between(1.0, 4095.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("reference-midi", 69.0),
                     between(0.0, 127.0, CliOptionDomainStage::Parameter))},
        {}, nullptr, 1);
    // The scale arguments are checked whichever target mode is selected, and
    // --target-midi names a note in both, so all three carry their range here
    // rather than only on the branch that reads them. --reference-midi is
    // deliberately unnarrowed: this path validates the anchor for finiteness
    // only, and declaring a range would refuse a value the library accepts.
    // The four retune options carry the ranges the library's correction config
    // refuses outside of.
    add_command(commands, "pitch-correct-timevarying", true,
                {with_domain(string_value("mode", "midi"),
                             choices_of({"midi", "scale"}, CliOptionDomainStage::Usage)),
                 with_domain(number_value("target-midi", 69.0),
                             between(0.0, 127.0, CliOptionDomainStage::Parameter)),
                 library_default(with_domain(int_value("hop-length"),
                                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                                 "512"),
                 library_default(with_domain(int_value("scale-root"),
                                             between(0.0, 11.0, CliOptionDomainStage::Parameter)),
                                 "0"),
                 with_domain(library_default(mode_mask_value("scale-mode-mask"), "2741"),
                             between(1.0, 4095.0, CliOptionDomainStage::Parameter)),
                 library_default(number_value("reference-midi"), "69.0"),
                 library_default(with_domain(number_value("retune-amount"),
                                             between(0.0, 1.0, CliOptionDomainStage::Parameter)),
                                 "1.0"),
                 library_default(with_domain(number_value("max-correction-semitones"),
                                             at_least(0.0, CliOptionDomainStage::Parameter)),
                                 "12.0"),
                 library_default(with_domain(number_value("retune-speed-ms"),
                                             at_least(0.0, CliOptionDomainStage::Parameter)),
                                 "50.0"),
                 library_default(with_domain(number_value("vibrato-threshold-cents"),
                                             at_least(0.0, CliOptionDomainStage::Parameter)),
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
    add_command(
        commands, "vocal-notes", true,
        {with_domain(number_value("segmentation-threshold-cents"),
                     greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(number_value("min-note-ms"), greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(int_value("frame-length-samples"),
                     greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(int_value("hop-length-samples"),
                     greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(number_value("f0-min-hz"), greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(number_value("f0-max-hz"), greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(number_value("yin-threshold"),
                     above_zero_up_to(1.0, CliOptionDomainStage::Usage)),
         with_domain(number_value("voiced-threshold"),
                     between(0.0, 1.0, CliOptionDomainStage::Usage)),
         flag("no-centered"),
         with_domain(number_value("reference-hz"), greater_than(0.0, CliOptionDomainStage::Usage))},
        {}, &validate_vocal_frequency_order);
    // One assignment per --edit occurrence, as --set does: the value reaches the
    // field parser as written, so no separator a fold could pick has to be
    // reserved.
    add_command(commands, "polyphonic-render", true,
                {string_value("edit", "", false, true), required_output()});
    add_command(
        commands, "voice-change", true,
        {with_stated_default(nullable_string_value("preset"), "none"), path_value("preset-json"),
         path_value("preset-pack"), string_value("set", "", false, true),
         number_value("pitch-semitones"), number_value("formant-factor"),
         with_domain(string_value("formant-mode", "relative"),
                     choices_of({"relative", "absolute"}, CliOptionDomainStage::Parameter)),
         required_output()});
    add_command(commands, "voice-presets", false, {});
    add_command(commands, "voice-preset", false, {string_value("preset", "neutralMonitor")});
    // The preset document arrives as the positional rather than through an
    // option, which is the arity the trailing argument declares.
    add_command(
        commands, "voice-preset-validate", false,
        {path_value("preset-json"), string_value("preset"), string_value("set", "", false, true)},
        {}, nullptr, 1);
    add_command(
        commands, "hpss", true,
        {library_default(int_value("kernel-harmonic"), "31"),
         library_default(int_value("kernel-percussive"), "31"), required_output(),
         flag("harmonic-only"), flag("percussive-only"), flag("with-residual"), flag("hard-mask"),
         with_domain(library_default(global_int("n-fft"), "2048"),
                     greater_than(0.0, CliOptionDomainStage::Parameter)),
         with_domain(library_default(global_int("hop-length"), "n_fft/4"),
                     greater_than(0.0, CliOptionDomainStage::Parameter))});
    // `init` is refused here because nothing below refuses it: the core's
    // validate_config checks the numeric fields only and reads any other name
    // as its own default, so an unrecognised initialiser is caught at the
    // surface or not at all.
    add_command(
        commands, "decompose-stems", true,
        {library_default(int_value("n-components"), "4"),
         library_default(int_value("n-iter"), "100"), library_default(number_value("beta"), "2.0"),
         library_default(
             with_domain(nullable_string_value("init"),
                         choices_of({"random", "nndsvd"}, CliOptionDomainStage::Parameter)),
             "random"),
         library_default(number_value("mask-power"), "1.0"), required_output(),
         library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "n_fft/4")});
    add_command(commands, "preemphasis", true,
                {library_default(number_value("coef"), "0.97"), required_output()});
    add_command(commands, "deemphasis", true,
                {library_default(number_value("coef"), "0.97"), required_output()});
    add_command(commands, "trim-silence", true,
                {library_default(number_value("threshold-db"), sonare::kDefaultAbsoluteSilenceDb),
                 number_value("top-db"), output_value(),
                 library_default(global_int("n-fft"), sonare::kDefaultSilenceFrameLength),
                 library_default(global_int("hop-length"), sonare::kDefaultSilenceHopLength)});
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
                {with_domain(string_value("mode", "peak"),
                             choices_of({"peak", "rms"}, CliOptionDomainStage::Parameter)),
                 with_stated_default(number_value("target-db"),
                                     stated_number(sonare::kDefaultPeakTargetDb) + " (peak) or " +
                                         stated_number(sonare::kDefaultRmsTargetDb) + " (rms)"),
                 required_output()},
                {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "gain", true, {number_value("gain-db"), required_output()});
    add_command(commands, "fade", true,
                {number_value("fade-in-sec"), number_value("fade-out-sec"), required_output()});
    add_command(
        commands, "filter", true,
        {with_domain(required_with_stage(string_value("type"), CliOptionDomainStage::Parameter),
                     choices_of({"hp", "lp", "bp", "notch"}, CliOptionDomainStage::Parameter)),
         with_domain(int_value("order", 2),
                     choices_of({"2", "4"}, CliOptionDomainStage::Parameter)),
         number_value("cutoff-hz", 0.0), number_value("center", 0.0),
         number_value("bandwidth", 0.0), required_output(), flag("zero-phase")});
    add_command(commands, "resample", true,
                {required_int("target-sample-rate"), required_output()});
    // The generators accept any positive rate, so a rate the library's own decoder
    // refuses produces a file none of these commands can read back. `resample`
    // already bounds its target rate here for the same reason; these three are the
    // rest of that family.
    add_command(commands, "tone", false,
                {number_value("frequency-hz"),
                 library_default(
                     with_domain(int_value("sample-rate"),
                                 between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate,
                                         CliOptionDomainStage::Parameter)),
                     "22050"),
                 library_default(number_value("duration-sec"), "1.0"),
                 library_default(number_value("phase"), "0.0"),
                 library_default(number_value("amplitude"), "1.0"), required_output()});
    add_command(
        commands, "chirp", false,
        {library_default(
             with_domain(int_value("sample-rate"),
                         between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate,
                                 CliOptionDomainStage::Parameter)),
             "22050"),
         library_default(number_value("duration-sec"), "1.0"), required_output(),
         flag("exponential"),
         with_domain(global_number("fmin-hz", 0.0), at_least(0.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("fmax-hz", true, true),
                     greater_than(0.0, CliOptionDomainStage::Usage))});
    add_command(commands, "clicks", false,
                {string_value("times"),
                 library_default(
                     with_domain(int_value("sample-rate"),
                                 between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate,
                                         CliOptionDomainStage::Parameter)),
                     "22050"),
                 library_default(int_value("length"), "0"),
                 library_default(number_value("frequency-hz"), "1000.0"),
                 library_default(number_value("click-duration-sec"), "0.1"), required_output()});

#ifdef SONARE_WITH_MASTERING
    // "--chain-config" names a complete chain config in the core's own
    // `{params, version}` form.
    add_command(commands, "mastering", true,
                {string_value("preset"), path_value("chain-config"),
                 number_value("target-lufs", -14.0), number_value("ceiling-dbtp", -1.0),
                 string_value("params"), bits_value(), path_value("report"), output_value(),
                 flag("assistant"), flag("enable-repair"), flag("explain"),
                 // The remaining AssistantConfig fields. `prefer_streaming_safe`
                 // defaults to true, so the reachable control is the one that
                 // turns it off -- a `--prefer-streaming-safe` flag would only
                 // ever restate the default. `--speech-mono-amount` carries no
                 // domain because the suggester clamps it to [0, 1] rather than
                 // refusing an outside value.
                 target_platform_value(), flag("no-streaming-safe"),
                 number_value("speech-mono-amount", 1.0)},
                {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "mastering-normalize-loudness", true,
                {number_value("target-lufs", -14.0), number_value("ceiling-dbtp", -1.0),
                 bits_value(), true_peak_oversample_value(), path_value("report"), output_value()},
                {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(
        commands, "mastering-processor", true,
        {required_string("processor"), string_value("params"), bits_value(), output_value()}, {},
        nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(
        commands, "eq", true,
        {string_value("params"),
         // Each of these indexes a closed enumeration in mastering/eq/eq_band.h
         // (EqBandType, BiquadCoeffMode, StereoPlacement, PhaseMode) or in
         // LinearPhaseEqConfig::Resolution. The bound is the enumerator count,
         // so an index past the end is refused instead of mapping to the first
         // enumerator. `type` takes it from the enum's last enumerator.
         with_domain(int_value("type", 0),
                     enum_index(0, static_cast<int>(sonare::mastering::eq::EqBandType::AllPass),
                                CliOptionDomainStage::Parameter)),
         number_value("frequency-hz", 1000.0), number_value("gain-db", 0.0), number_value("q", 1.0),
         with_domain(int_value("coeff-mode", 0), enum_index(0, 1, CliOptionDomainStage::Parameter)),
         int_value("slope-db-oct", 12),
         with_domain(int_value("placement", 0), enum_index(0, 4, CliOptionDomainStage::Parameter)),
         number_value("threshold-db", -24.0), number_value("ratio", 2.0),
         number_value("range-db", -6.0), number_value("attack-ms", 5.0),
         number_value("release-ms", 50.0), number_value("detector-delay-ms", 0.0),
         number_value("sidechain-freq-hz", -1.0), number_value("sidechain-q", 1.0),
         with_domain(int_value("phase-mode", 1), enum_index(0, 3, CliOptionDomainStage::Parameter)),
         with_domain(int_value("resolution", 0), enum_index(0, 5, CliOptionDomainStage::Parameter)),
         number_value("gain-scale", 1.0), number_value("output-gain-db", 0.0),
         number_value("output-pan", 0.0), bits_value(), output_value(), flag("proportional-q"),
         flag("dynamic"), flag("auto-threshold"), flag("auto-gain")},
        {}, nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "mastering-pair-processor", true,
                {required_string("processor"), required_path("reference"), string_value("params"),
                 bits_value(), output_value()});
    add_command(commands, "mastering-pair-analyze", true,
                {required_string("analysis"), required_path("reference"), string_value("params")});
    add_command(commands, "mastering-stereo-analyze", true,
                {required_string("analysis"), required_path("reference"), string_value("params")});
    // `--config-out` writes the suggested chain in the core's own chain-config
    // form, which is what `mastering --chain-config` reads back -- the same
    // role `suggest-mix --scene-out` plays for a mix scene.
    // The assistant options `mastering --assistant` declares, with the same
    // names, defaults and domains; --params still carries them as flat keys.
    add_command(commands, "mastering-suggest", true,
                {string_value("params"), string_value("config-out", ""),
                 number_value("target-lufs", -14.0), number_value("ceiling-dbtp", -1.0),
                 flag("enable-repair"), string_value("preset"), target_platform_value(),
                 flag("no-streaming-safe"), number_value("speech-mono-amount", 1.0)});
    add_command(commands, "mastering-processors", false, {});
    add_command(commands, "mastering-pair-processors", false, {});
    add_command(commands, "mastering-pair-analyses", false, {});
    add_command(commands, "mastering-stereo-analyses", false, {});
    add_command(commands, "mastering-presets", false, {});
    add_command(commands, "mastering-profile", true, {string_value("params")});
    add_command(commands, "mastering-streaming", true,
                {nullable_string_value("platforms"), path_value("platforms-file")});
    add_command(commands, "repair", true,
                {string_value("preset"), string_value("params"), bits_value(), output_value(),
                 flag("detect"), flag("explain")},
                {}, validate_repair_output);
#endif
#ifdef SONARE_WITH_MIXING
    // `mix-strip` is the only spelling for the strip. `mix` names the Python
    // CLI's scene mixer, which this front-end has no equivalent of, so it is
    // refused here as an unknown command rather than carried as an alias.
    add_command(
        commands, "mix-strip", true,
        {number_value("input-trim-db", 0.0), number_value("fader-db", 0.0),
         with_domain(number_value("pan", 0.0), between(-1.0, 1.0, CliOptionDomainStage::Parameter)),
         with_domain(
             string_value("pan-mode", "balance"),
             choices_of({"balance", "stereo-pan", "dual-pan"}, CliOptionDomainStage::Parameter)),
         with_domain(number_value("width", 1.0),
                     between(0.0, 2.0, CliOptionDomainStage::Parameter)),
         output_value()},
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
    add_command(
        commands, "suggest-mix", false,
        {repeatable_path("input"), int_value("sample-rate", 48000), string_value("params", ""),
         tempo(make_option("tempo-bpm", CliOptionArity::RequiredValue, CliOptionScalarType::String,
                           null_default())),
         string_value("scene-out", "")});
#endif

    // Feature leaves.
    // A lower band edge of 0 Hz is a real edge, so `fmin-hz` refuses only a negative;
    // an omitted `fmax-hz` is the Nyquist frequency, so a spelled one must be above 0.
    // `n-mels` needs none -- the filterbank's own ceiling refuses an oversized band
    // count for every command and every surface at once.
    add_command(commands, "mel", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128"),
                 with_domain(library_default(global_number("fmin-hz"), "0.0"),
                             at_least(0.0, CliOptionDomainStage::Parameter)),
                 with_stated_default(with_domain(number_value("fmax-hz", false, true),
                                                 greater_than(0.0, CliOptionDomainStage::Usage)),
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
    // Every domain the Python CLI declares for this command, in the same
    // classes: the frequency/threshold `type=` callables are parse-time (usage)
    // and the algorithm name is a handler-level rejection (invalid parameter).
    add_command(commands, "pitch", true,
                {flag("frames"),
                 with_domain(string_value("algorithm", "pyin"),
                             choices_of({"yin", "pyin"}, CliOptionDomainStage::Parameter)),
                 library_default(with_domain(number_value("threshold"),
                                             above_zero_up_to(1.0, CliOptionDomainStage::Usage)),
                                 "0.1"),
                 with_domain(library_default(global_int("hop-length"), "512"),
                             greater_than(0.0, CliOptionDomainStage::Usage)),
                 library_default(with_domain(number_value("f0-min-hz"),
                                             greater_than(0.0, CliOptionDomainStage::Usage)),
                                 "65.0"),
                 library_default(with_domain(number_value("f0-max-hz"),
                                             greater_than(0.0, CliOptionDomainStage::Usage)),
                                 "2093.0")},
                {}, &validate_pitch_frequency_order);
    add_command(commands, "onset-strength", true,
                {library_default(global_int("n-fft"), "2048"),
                 library_default(global_int("hop-length"), "512"),
                 library_default(global_int("n-mels"), "128")});
    for (const char* path : {"fourier-tempogram", "tempogram-ratio"})
      add_command(commands, path, true,
                  {int_value("win-length", 384), library_default(global_int("hop-length"), "512")});
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
    add_command(commands, "nnls-chroma", true, {library_default(global_int("hop-length"), "512")});
    // An omitted lower bound is the library's (C1); a constant-Q bin cannot sit at
    // or below 0 Hz, so a spelled one must be above it.
    add_command(commands, "cqt", true,
                {library_default(int_value("n-bins"), "84"),
                 library_default(int_value("bins-per-octave"), "12"),
                 library_default(global_int("hop-length"), "512"),
                 with_domain(number_value("fmin-hz", false, true),
                             greater_than(0.0, CliOptionDomainStage::Usage))});
    add_command(commands, "vqt", true,
                {library_default(int_value("n-bins"), "84"),
                 library_default(int_value("bins-per-octave"), "12"),
                 library_default(number_value("gamma"), "-1.0"),
                 library_default(number_value("filter-scale"), "1.0"),
                 library_default(global_int("hop-length"), "512"),
                 with_domain(number_value("fmin-hz", false, true),
                             greater_than(0.0, CliOptionDomainStage::Usage))});
    add_command(
        commands, "mel-to-audio", true,
        {library_default(int_value("n-iter"), sonare::constants::kDefaultGriffinLimIterations),
         required_output(), library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(global_int("n-mels"), "128"),
         with_domain(library_default(global_number("fmin-hz"), "0.0"),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("fmax-hz", false, true),
                     greater_than(0.0, CliOptionDomainStage::Usage))});
    add_command(
        commands, "mfcc-to-audio", true,
        {int_value("n-mfcc", 13),
         library_default(int_value("n-iter"), sonare::constants::kDefaultGriffinLimIterations),
         required_output(), library_default(global_int("n-fft"), "2048"),
         library_default(global_int("hop-length"), "512"),
         library_default(global_int("n-mels"), "128"),
         with_domain(library_default(global_number("fmin-hz"), "0.0"),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("fmax-hz", false, true),
                     greater_than(0.0, CliOptionDomainStage::Usage))});
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
    add_command(
        commands, "synthesize-rir", false,
        {library_default(number_value("length"), "7.0"),
         library_default(number_value("width"), "5.0"),
         library_default(number_value("height"), "3.0"),
         library_default(with_domain(number_value("absorption"),
                                     between(0.0, 1.0, CliOptionDomainStage::Parameter)),
                         "0.2"),
         library_default(number_value("source-x"), "1.0"),
         library_default(number_value("source-y"), "1.0"),
         library_default(number_value("source-z"), "1.2"),
         library_default(number_value("listener-x"), "5.0"),
         library_default(number_value("listener-y"), "4.0"),
         library_default(number_value("listener-z"), "1.7"), int_value("sample-rate", 48000),
         library_default(int_value("ism-order"), "3"),
         library_default(with_domain(int_value("seed"),
                                     between(0.0, kMaxCliSeed, CliOptionDomainStage::Parameter)),
                         "1"),
         with_stated_default(
             with_domain(number_value("max-sec"), greater_than(0.0, CliOptionDomainStage::Usage)),
             "natural length"),
         required_output(), flag("sabine")});
    add_command(
        commands, "room-morph", true,
        {number_value("length", 7.0), number_value("width", 5.0), number_value("height", 3.0),
         library_default(with_domain(number_value("absorption"),
                                     between(0.0, 1.0, CliOptionDomainStage::Parameter)),
                         "0.2"),
         library_default(number_value("source-x"), "1.0"),
         library_default(number_value("source-y"), "1.0"),
         library_default(number_value("source-z"), "1.2"),
         library_default(number_value("listener-x"), "5.0"),
         library_default(number_value("listener-y"), "4.0"),
         library_default(number_value("listener-z"), "1.7"),
         library_default(number_value("suppression"), "0.5"),
         library_default(number_value("wet"), "0.5"), library_default(int_value("ism-order"), "3"),
         library_default(with_domain(int_value("seed"),
                                     between(0.0, kMaxCliSeed, CliOptionDomainStage::Parameter)),
                         "1"),
         with_stated_default(
             with_domain(number_value("max-sec"), greater_than(0.0, CliOptionDomainStage::Usage)),
             "natural length"),
         library_default(with_domain(number_value("receiver-spacing"),
                                     above_zero_up_to(4.0, CliOptionDomainStage::Parameter)),
                         "0.5"),
         required_output(), flag("sabine")},
        {}, nullptr, 0, /*preserves_stereo_input=*/true);
#endif

    // Metering and scalar utility leaves.
    // Measures every channel as BS.1770 sums them, the loudness `master` reports for its input.
    add_command(commands, "lufs", true, {flag("series")}, {}, nullptr, 0,
                /*preserves_stereo_input=*/false, /*preserves_multichannel_input=*/true);
    add_command(commands, "meter", true,
                {library_default(number_value("clip-threshold"), "0.999"),
                 library_default(int_value("oversample"), "4")});
    add_command(commands, "clipping", true,
                {library_default(number_value("threshold"), "0.999"),
                 library_default(int_value("min-region"), "1")});
    add_command(commands, "dynamic-range", true,
                {library_default(number_value("window-sec"), "3.0"),
                 library_default(number_value("hop-sec"), "1.0"),
                 library_default(number_value("low-percentile"), "0.1"),
                 library_default(number_value("high-percentile"), "0.95")});
    add_command(commands, "stereo", true, {path_value("reference")});
    add_command(commands, "phase", true, {path_value("reference")});
    // Without --n-fft the conversion applies no centring offset, as the library does.
    add_command(commands, "frames-to-samples", false,
                {int_value("frames"), library_default(int_value("n-fft", false, true), "0"),
                 global_int("hop-length", 512)});
    add_command(commands, "samples-to-frames", false,
                {int_value("samples"), library_default(int_value("n-fft", false, true), "0"),
                 global_int("hop-length", 512)});
    add_command(commands, "power-to-db", false,
                {string_value("values"), library_default(number_value("ref"), "1.0"),
                 library_default(number_value("amin"), "1e-10"),
                 library_default(number_value("top-db"), "80.0")});
    add_command(commands, "amplitude-to-db", false,
                {string_value("values"), library_default(number_value("ref"), "1.0"),
                 library_default(number_value("amin"), "1e-05"),
                 library_default(number_value("top-db"), "80.0")});
    add_command(commands, "db-to-power", false,
                {string_value("values"), library_default(number_value("ref"), "1.0")});
    add_command(commands, "db-to-amplitude", false,
                {string_value("values"), library_default(number_value("ref"), "1.0")});
    add_command(commands, "frame-signal", false,
                {string_value("values"), int_value("frame-length"), global_int("n-fft", 2048),
                 global_int("hop-length", 512)});
    add_command(commands, "pad-center", false,
                {string_value("values"), int_value("size"),
                 library_default(number_value("pad-value"), "0.0")});
    add_command(commands, "fix-length", false,
                {string_value("values"), int_value("size"),
                 library_default(number_value("pad-value"), "0.0")});
    add_command(commands, "fix-frames", false,
                {string_value("values"), library_default(int_value("x-min"), "0"),
                 library_default(int_value("x-max"), "-1"), flag("no-pad")});
    add_command(commands, "peak-pick", false,
                {string_value("values"), int_value("pre-max", 1), int_value("post-max", 1),
                 int_value("pre-avg", 1), int_value("post-avg", 1), number_value("delta", 0.0),
                 int_value("wait", 0)});
    add_command(commands, "vector-normalize", false,
                {string_value("values"),
                 // Indexes NormType (inf, L1, L2, power); an index past the end used to
                 // fall through to the inf norm and report success.
                 library_default(with_domain(int_value("norm-type"),
                                             enum_index(0, 3, CliOptionDomainStage::Parameter)),
                                 "0"),
                 library_default(number_value("threshold"), "0.0")});
    add_command(
        commands, "pcen", false,
        {string_value("values"), library_default(int_value("sample-rate"), "22050"),
         library_default(number_value("time-constant"), "0.4"),
         library_default(number_value("gain"), "0.98"),
         library_default(number_value("bias"), "2.0"),
         library_default(number_value("power"), "0.5"),
         library_default(number_value("eps"), "1e-06"), int_value("n-bins"), int_value("n-frames"),
         library_default(int_value("hop-length", false, true), "512")});
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
    add_project_command(
        "project.new",
        {with_stated_default(int_value("sample-rate"), "the project's own"), required_output()});
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
        {required_output(), int_value("sample-rate"), project_length(int_value("frames")),
         master_width(int_value("channels")),
         library_default(int_value("instrument-latency-samples"), "0"), optional_string("synth"),
         string_value("audio", false, true), flag("resolve-audio")},
        &validate_project_bounce_channels, kProjectJsonPositional);
    // `align-takes` reads the source FILES rather than binding PCM, so it shares
    // `--audio` / `--resolve-audio` with `bounce` and nothing else. Its three
    // numeric options carry the invalid-parameter class: each is a value the
    // caller spelled correctly and the measurement then refuses. The C entry spells
    // "library value" as 0 for the two resolution options, so an explicit 0 is refused
    // here and only an absent flag sends it; their grid stays with the core.
    add_project_command(
        "project.align-takes",
        {required_output(),
         with_domain(int_value("reference-source", true),
                     between(1.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max()),
                             CliOptionDomainStage::Parameter)),
         string_value("audio", false, true), flag("resolve-audio"),
         with_domain(library_default(global_int("hop-length"), "512"),
                     greater_than(0.0, CliOptionDomainStage::Parameter)),
         library_default(with_domain(int_value("bins-per-octave"),
                                     greater_than(0.0, CliOptionDomainStage::Parameter)),
                         "12")},
        nullptr, kProjectJsonPositional);
    // `project bounce` with the synth pinned on, as a top-level leaf: it takes
    // no subcommand positional, and `--synth` is a plain value rather than an
    // optional flag because the rendering always goes through NativeSynth and
    // only the preset is in question.
    add_command(commands, "midi-render", false,
                {required_output(), int_value("sample-rate"), project_length(int_value("frames")),
                 master_width(int_value("channels")),
                 library_default(int_value("instrument-latency-samples"), "0"),
                 with_stated_default(nullable_string_value("synth"), "GM program/channel routing")},
                {}, &validate_project_bounce_channels, 1, false, false, kProjectJsonPositional);
    // Every numeric here means "keep the library default" when absent. The ranges a
    // single value can be refused by are declared; the reattack ratio's "0 or above 1"
    // stays in the handler, and the rules joining two options stay with the core.
    const sonare::editing::note_model::TranscribeConfig transcribe_defaults;
    add_command(
        commands, "transcribe", true,
        {required_output(),
         with_stated_default(tempo(number_value("tempo-bpm")), "detected from the audio"),
         flag("polyphonic"),
         library_default(number_value("reference-hz"), transcribe_defaults.reference_hz),
         number_value("f0-min-hz"), number_value("f0-max-hz"),
         // The segmenter's own shortest note; the singing and polyphonic values are the
         // transcriber's, which no header carries, so the derived-default row pins them.
         with_stated_default(
             number_value("min-note-ms"),
             stated_number(sonare::editing::pitch_editor::NoteSegmenterConfig{}.min_note_ms) +
                 " (segments), " + stated_number(45.0) + " (singing) or " + stated_number(60.0) +
                 " (polyphonic)"),
         library_default(number_value("segmentation-threshold-cents"),
                         transcribe_defaults.segmentation_threshold_cents),
         // The polyphonic values below are the transcriber's own and appear in no
         // header, so each one's stated-default row is what pins it.
         library_default(with_domain(int_value("max-polyphony"),
                                     between(1.0, 64.0, CliOptionDomainStage::Parameter)),
                         10),
         with_domain(int_value("min-note-division"),
                     between(1.0, 128.0, CliOptionDomainStage::Parameter)),
         library_default(with_domain(number_value("min-frame-peak-ratio"),
                                     between(0.0, 1.0, CliOptionDomainStage::Parameter)),
                         "0.2"),
         library_default(with_domain(number_value("min-ridge-peak-ratio"),
                                     between(0.0, 1.0, CliOptionDomainStage::Parameter)),
                         "0.1"),
         library_default(number_value("reattack-ratio"), "2.0"),
         library_default(number_value("velocity-floor-db"), transcribe_defaults.velocity_floor_db),
         int_value("fixed-velocity"), library_default(int_value("channel"), "0"),
         library_default(
             with_domain(nullable_string_value("monophonic-model"),
                         choices_of({"segments", "singing"}, CliOptionDomainStage::Parameter)),
             "segments")});
    // The reference melody is read by the arrangement library and assigned by the
    // pitch editor, so the leaf exists only where both do. `--reference-smf` is
    // required in the parser on both front-ends and so keeps the usage class,
    // while every value domain below is one the Python handler enforces after
    // parsing and therefore carries the invalid-parameter class.
    //
    // The two bounds are the null-default overload rather than a restated 0.5 and
    // 12: both values are legal inputs, so an absent flag cannot be spelled as a
    // sentinel and the handler leaves NoteTargetAssignConfig's own field alone.
#ifdef SONARE_WITH_PITCH_EDITOR
    add_command(commands, "tune-to-midi", true,
                {required_path("reference-smf"),
                 with_domain(int_value("track", 0), at_least(0.0, CliOptionDomainStage::Parameter)),
                 library_default(with_domain(nullable_string_value("unmatched-policy"),
                                             choices_of({"leave", "mute", "nearest"},
                                                        CliOptionDomainStage::Parameter)),
                                 "leave"),
                 with_domain(number_value("min-overlap-ratio"),
                             between(0.0, 1.0, CliOptionDomainStage::Parameter)),
                 with_domain(number_value("max-correction-semitones"),
                             at_least(0.0, CliOptionDomainStage::Parameter)),
                 required_output()});
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
    add_command(
        commands, "playback", true,
        {library_default(with_domain(nullable_string_value("target"),
                                     choices_of({"headphones", "stereo", "5.1", "7.1"},
                                                CliOptionDomainStage::Usage)),
                         "headphones"),
         library_default(with_domain(nullable_string_value("input-layout"),
                                     choices_of({"auto", "mono", "stereo", "5.1", "7.1"},
                                                CliOptionDomainStage::Usage)),
                         "auto"),
         path_value("config"), flag("no-upmix"),
         library_default(
             with_domain(number_value("night"), between(0.0, 1.0, CliOptionDomainStage::Usage)),
             playback_defaults.night_amount),
         library_default(with_domain(number_value("dialogue-db"),
                                     between(-12.0, 12.0, CliOptionDomainStage::Usage)),
                         playback_defaults.dialogue_level_db),
         with_stated_default(with_domain(number_value("program-lufs"),
                                         between(-70.0, 0.0, CliOptionDomainStage::Usage)),
                             "measured from the whole input"),
         library_default(with_domain(number_value("target-lufs"),
                                     between(-40.0, -5.0, CliOptionDomainStage::Usage)),
                         playback_defaults.target_lufs),
         with_domain(nullable_string_value("room"),
                     choices_of({"none", "living-room", "home-theater", "screening-room"},
                                CliOptionDomainStage::Usage)),
         path_value("hrtf"), required_output()},
        {}, nullptr, 0, /*preserves_stereo_input=*/false, /*preserves_multichannel_input=*/true);
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
