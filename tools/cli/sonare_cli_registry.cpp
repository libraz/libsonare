#include "sonare_cli_registry.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/channel_layout.h"

#ifdef SONARE_WITH_MASTERING
#include "mastering/assistant/platform_targets.h"
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
#if defined(SONARE_WITH_MASTERING) || defined(SONARE_WITH_ARRANGEMENT)
CliOptionSpec required_path(const char* name) { return path_value(name, true); }
#endif

#ifdef SONARE_WITH_MASTERING
// A string option whose absence is null rather than the empty string. Both
// spellings mean "not supplied" to the reader, but the CLI contract compares
// declared defaults across the two front-ends, so the representation has to
// agree with the one the Python CLI publishes.
CliOptionSpec nullable_string_value(const char* name) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::String,
                     null_default(), {}, {}, false, false, false, true);
}
#endif

#ifdef SONARE_WITH_MIXING_ASSISTANT
// A repeatable path. `path_value` takes no repeatable flag because every other
// path option here is single-valued, and the default has to be the empty array
// rather than null so the two front-ends publish the same absent value.
CliOptionSpec repeatable_path(const char* name) {
  return make_option(name, CliOptionArity::RequiredValue, CliOptionScalarType::Path,
                     string_array_default(), {}, {}, false, true, false, true);
}
#endif

#ifdef SONARE_WITH_ARRANGEMENT
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
// trades processing for a report and explicitly does not need it. A plain
// `required_output()` cannot express that the requirement is conditional, so
// the command carries this validator instead.
CliValidationError validate_repair_output(const CliArgs& args) {
  if (args.has("detect") || args.has("output")) return {};
  return {"--output is required unless --detect is given", true};
}
#endif

// `--fmax` must stay above `--fmin`; neither option's own domain can express
// that, and the two pitch engines disagreed about it (pyin checked, yin did
// not), so the CLI settles it before either is reached.
//
// The comparison is between the values the command will actually run with, not
// between two supplied options: `--fmin 3000` on its own inverts the range just
// as surely against the 2093 Hz default `--fmax`, and gating the check on both
// being present let that invocation through to a `SONARE_CHECK` that names
// neither option. The effective values are in the message because the offending
// half is commonly the one the caller never typed.
CliValidationError validate_pitch_frequency_order(const CliArgs& args) {
  if (!args.has("fmin") && !args.has("fmax")) return {};
  const float minimum = args.get_float("fmin", 0.0f);
  const float maximum = args.get_float("fmax", 0.0f);
  if (maximum > minimum) return {};
  std::ostringstream message;
  message << "--fmax must be greater than --fmin (--fmin " << minimum << ", --fmax " << maximum
          << ")";
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

// Compared against the values the handler will actually use, not the raw
// options: melody substitutes its own band for an absent one, so checking the
// raw pair lets `--fmax 50` through against an unstated 80 Hz floor that the
// analyzer then inverts. The two numbers repeat the handler's because the
// substitution lives there; they are this command's published band, not
// MelodyConfig's wider default.
CliValidationError validate_melody_frequency_band(const CliArgs& args) {
  const float minimum = args.fmin > 0.0f ? args.fmin : 80.0f;
  const float maximum = args.fmax > 0.0f ? args.fmax : 1000.0f;
  if (maximum > minimum) return {};
  std::ostringstream message;
  message << "--fmax must be greater than --fmin (--fmin " << minimum << ", --fmax " << maximum
          << ")";
  return {message.str(), true};
}

// Only when HPSS is on: the separation needs enough overlap to resynthesize, and
// the C entry point refuses a hop below 16 for exactly this pair. The STFT's own
// geometry check does not reach it -- at the 4096-point default a hop of 8
// satisfies `hop <= n_fft / 2` and still starves the separation. A per-option
// domain cannot express it because the same hop is fine without the flag.
CliValidationError validate_key_hpss_hop(const CliArgs& args) {
  if (!args.has("use-hpss") && !args.has("hpss")) return {};
  if (args.hop_length >= 16) return {};
  std::ostringstream message;
  message << "--hop-length must be at least 16 with --use-hpss (got " << args.hop_length << ")";
  return {message.str(), true};
}

#ifdef SONARE_WITH_ARRANGEMENT
// The project bounce renders a stereo master and writes either that pair or its
// mono downmix, so the C ABI accepts a channel count of 1 or 2 and refuses any
// other width rather than emitting silent planes. Without this the refusal
// arrives from the render as a bare invalid-parameter error, after the project
// has been loaded, with nothing naming the option that caused it.
//
// This is a command validator rather than a per-option domain because a domain
// is published in the shared option inventory both CLIs are pinned against, and
// the Python CLI declares none for this option.
CliValidationError validate_project_bounce_channels(const CliArgs& args) {
  if (!args.has("channels")) return {};
  const int channels = args.get_int("channels", 2);
  const int mono = sonare::channel_count(sonare::ChannelLayout::Mono);
  const int stereo = sonare::channel_count(sonare::ChannelLayout::Stereo);
  // `<= 0` is the bounce options struct's own documented "let the engine
  // choose" sentinel (sonare_c_project_core.h: num_channels <= 0 => 2), so
  // refusing it made this validator stricter than the oracle it fronts and
  // stricter than the Python CLI, which accepts it. Only a positive count that
  // is neither mono nor stereo is a value nothing downstream can honour.
  if (channels <= 0 || channels == mono || channels == stereo) return {};
  return {"invalid value for --channels: " + std::to_string(channels) + " (expected one of " +
              std::to_string(mono) + ", " + std::to_string(stereo) + ", or <= 0 for the default)",
          true};
}
#endif

CliOptionSpec global_int(const char* name, int value) {
  return int_value(name, value, false, true);
}

CliOptionSpec global_number(const char* name, double value) {
  return number_value(name, value, false, true);
}

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
// an audio file; an audio leaf already takes exactly one and ignores it.
// `preserves_stereo_input` is the leaf's own answer to what a two-channel input
// produces.
void add_command(std::vector<CliCommandSpec>& registry, const char* path, bool requires_audio,
                 std::vector<CliOptionSpec> options, std::vector<std::string> aliases = {},
                 CliCommandValidator validate = nullptr, size_t positionals = 0,
                 bool preserves_stereo_input = false) {
  registry.push_back({path, std::move(aliases), with_json(std::move(options)), requires_audio,
                      requires_audio ? 1u : positionals, preserves_stereo_input, true, validate});
}

const std::vector<CliCommandSpec>& build_cli_registry() {
  static const std::vector<CliCommandSpec> registry = [] {
    std::vector<CliCommandSpec> commands;
    commands.reserve(110);

    // Analysis leaves.
    add_command(commands, "analyze", true,
                {flag("with-seventh"), flag("no-hpss"),
                 // The Python CLI parses this through a non-negative finite
                 // checker, so the native contract declares the same domain and
                 // the same (parse-time) class.
                 with_domain(number_value("chroma-highpass", 80.0),
                             at_least(0.0, CliOptionDomainStage::Usage)),
                 // An odd meter is only ever reported if its numerator was
                 // asked for, so without these the CLI cannot reach one. The
                 // list is a string here and validated by the core, which is
                 // what already holds the count and range rules.
                 string_value("meter-candidates"),
                 // The Python CLI parses this through its positive-integer
                 // checker, so the domain and the (parse-time) class match it.
                 with_domain(int_value("meter-denominator", 4),
                             greater_than(0.0, CliOptionDomainStage::Usage))});
    for (const char* path : {"bpm", "beats", "downbeats", "onsets"})
      add_command(commands, path, true, {});
    add_command(
        commands, "timbre", true,
        {global_int("n-fft", 2048), global_int("hop-length", 512), global_int("n-mels", 128)});
    add_command(commands, "key", true,
                {global_int("n-fft", 4096), global_int("hop-length", 512), int_value("candidates"),
                 flag("use-hpss", false, true, {"hpss"}), flag("loudness-weighted"),
                 number_value("high-pass-hz", 0.0), string_value("modes"), string_value("profile"),
                 string_value("genre-hint")},
                {}, validate_key_hpss_hop);
    // `smoothing-window` and `hmm-beam-width` carry the C entry point's domains;
    // `min-duration` and `threshold` are checked by ChordAnalyzer itself and so
    // need none. A zero smoothing window is the one the analyzer accepts and the
    // C entry refuses, and a negative beam width is never truncated against, so
    // `--use-hmm --hmm-beam-width -1` silently ran an unbeamed search.
    add_command(commands, "chords", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512),
                 number_value("min-duration", 0.3),
                 with_domain(number_value("smoothing-window", 2.0),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 number_value("threshold", 0.5), flag("triads-only"), flag("nnls"),
                 flag("no-beat-sync"), flag("use-hmm"),
                 with_domain(int_value("hmm-beam-width", 24),
                             at_least(0.0, CliOptionDomainStage::Parameter)),
                 flag("key-context"), string_value("key-root", "C"),
                 string_value("key-mode", "major"), flag("detect-inversions")});
    // SectionAnalyzer validates only that the audio is non-empty, so nothing
    // downstream refuses a negative minimum section length.
    add_command(
        commands, "sections", true,
        {with_domain(number_value("min-duration", 4.0),
                     at_least(0.0, CliOptionDomainStage::Parameter)),
         number_value("threshold", 0.3), global_int("n-fft", 2048), global_int("hop-length", 512)});
    // DynamicsAnalyzer floors a tiny or negative window to avoid a 0/0 rather
    // than refusing it, so the refusal the other surfaces get has to be here.
    add_command(commands, "dynamics", true,
                {with_domain(number_value("window-sec", 0.4),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 global_int("hop-length", 512)});
    // Neither RhythmAnalyzer nor the BeatAnalyzer it forwards to validates any of
    // the three tempo bounds; the derived periods are merely floored at 1.
    add_command(commands, "rhythm", true,
                {with_domain(number_value("start-bpm", 120.0),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(number_value("bpm-min", 60.0),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(number_value("bpm-max", 200.0),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 global_int("n-fft", 2048), global_int("hop-length", 512)},
                {}, validate_rhythm_tempo_range);
    // This command's `hop-length` does NOT reach the STFT, so the guard that
    // covers every other command's copy does not cover this one: MelodyAnalyzer
    // frames the signal itself and advances by the hop, so zero never advances
    // and the loop does not terminate. The plain-YIN entry it calls
    // (yin_with_confidence) validates nothing either -- the checks live in
    // yin_track, which this path does not go through.
    add_command(commands, "melody", true,
                {with_domain(number_value("threshold", 0.1),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(global_int("hop-length", 512),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(global_number("fmin", 80.0),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(global_number("fmax", 1000.0),
                             greater_than(0.0, CliOptionDomainStage::Parameter))},
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
         flag("no-mfcc"), flag("no-chroma"), global_int("n-fft", 2048),
         global_int("hop-length", 512)},
        {}, validate_boundary_feature_streams);

    // Processing leaves.
    // The native spectral backend repairs a non-positive analysis geometry into
    // the librosa defaults instead of refusing it, five lines above a comment
    // saying a geometry that cannot be overlap-added is an error rather than
    // something to repair. These two make the refusal the one a caller gets.
    add_command(
        commands, "pitch-shift", true,
        {number_value("semitones"), required_output(),
         with_domain(global_int("n-fft", 2048), greater_than(0.0, CliOptionDomainStage::Parameter)),
         with_domain(global_int("hop-length", 512),
                     greater_than(0.0, CliOptionDomainStage::Parameter))});
    add_command(
        commands, "time-stretch", true,
        {number_value("rate"), required_output(),
         with_domain(global_int("n-fft", 2048), greater_than(0.0, CliOptionDomainStage::Parameter)),
         with_domain(global_int("hop-length", 512),
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
         with_domain(int_value("mode-mask", 0xAB5),
                     between(1.0, 4095.0, CliOptionDomainStage::Parameter)),
         with_domain(number_value("reference-midi", 69.0),
                     between(0.0, 127.0, CliOptionDomainStage::Parameter))},
        {}, nullptr, 1);
    // The scale arguments are checked whichever target mode is selected, and
    // --target-midi names a note in both, so all three carry their range here
    // rather than only on the branch that reads them. --reference-midi is
    // deliberately unnarrowed: this path validates the anchor for finiteness
    // only, and declaring a range would refuse a value the library accepts.
    add_command(commands, "pitch-correct-timevarying", true,
                {with_domain(string_value("mode", "midi"),
                             choices_of({"midi", "scale"}, CliOptionDomainStage::Usage)),
                 with_domain(number_value("target-midi", 69.0),
                             between(0.0, 127.0, CliOptionDomainStage::Parameter)),
                 with_domain(int_value("hop-length", 512),
                             greater_than(0.0, CliOptionDomainStage::Parameter)),
                 with_domain(int_value("scale-root", 0),
                             between(0.0, 11.0, CliOptionDomainStage::Parameter)),
                 with_domain(int_value("scale-mode-mask", 0xAB5),
                             between(1.0, 4095.0, CliOptionDomainStage::Parameter)),
                 number_value("reference-midi", 69.0), required_output()});
    // --offset defaults to the end of the buffer rather than to a number, which
    // no registry default can spell, so it is the null-default overload and the
    // handler resolves the absent case.
    add_command(commands, "note-move", true,
                {int_value("onset", 0), int_value("offset"), int_value("target-onset", 0),
                 required_output()});
    add_command(commands, "note-stretch", true,
                {int_value("onset", 0), int_value("offset", 0), number_value("ratio", 1.0),
                 required_output()});
    add_command(commands, "polyphonic-notes", true, {});
    // One assignment per --edit occurrence, as --set does: the value reaches the
    // field parser as written, so no separator a fold could pick has to be
    // reserved.
    add_command(commands, "polyphonic-render", true,
                {string_value("edit", "", false, true), required_output()});
    add_command(commands, "voice-change", true,
                {string_value("preset", ""), path_value("preset-json"), path_value("preset-pack"),
                 string_value("set", "", false, true), number_value("pitch-semitones"),
                 number_value("formant-factor"), required_output()});
    add_command(commands, "voice-presets", false, {});
    add_command(commands, "voice-preset", false, {string_value("preset", "neutral-monitor")});
    // The preset document arrives as the positional rather than through an
    // option, which is the arity the trailing argument declares.
    add_command(
        commands, "voice-preset-validate", false,
        {path_value("preset-json"), string_value("preset"), string_value("set", "", false, true)},
        {}, nullptr, 1);
    add_command(
        commands, "hpss", true,
        {int_value("kernel-harmonic", 31), int_value("kernel-percussive", 31), required_output(),
         flag("harmonic-only"), flag("percussive-only"), flag("with-residual"), flag("hard-mask"),
         global_int("n-fft", 2048), global_int("hop-length", 512)});
    // `init` is refused here because nothing below refuses it: the core's
    // validate_config checks the numeric fields only and reads any other name
    // as its own default, so an unrecognised initialiser is caught at the
    // surface or not at all.
    add_command(commands, "decompose-stems", true,
                {int_value("n-components", 4), int_value("n-iter", 100), number_value("beta", 2.0),
                 with_domain(string_value("init", "random"),
                             choices_of({"random", "nndsvd"}, CliOptionDomainStage::Parameter)),
                 number_value("mask-power", 1.0), required_output(), global_int("n-fft", 2048),
                 global_int("hop-length", 512)});
    add_command(commands, "preemphasis", true, {number_value("coef", 0.97), required_output()});
    add_command(commands, "deemphasis", true, {number_value("coef", 0.97), required_output()});
    add_command(commands, "trim-silence", true,
                {number_value("threshold-db"), number_value("top-db"), output_value(),
                 global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(
        commands, "split-silence", true,
        {number_value("top-db", 60.0), global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "normalize", true,
                {string_value("mode", "peak"), number_value("target-db"), required_output()}, {},
                nullptr, 0, /*preserves_stereo_input=*/true);
    add_command(commands, "gain", true, {number_value("gain-db"), required_output()});
    add_command(commands, "fade", true,
                {number_value("fade-in"), number_value("fade-out"), required_output()});
    add_command(commands, "filter", true,
                {string_value("type"), int_value("order", 2), number_value("cutoff", 0.0),
                 number_value("center", 0.0), number_value("bandwidth", 0.0), required_output(),
                 flag("zero-phase")});
    add_command(commands, "resample", true,
                {required_int("target-rate", {"target-sr"}), required_output()});
    // The generators accept any positive rate, so a rate the library's own decoder
    // refuses produces a file none of these commands can read back. `resample`
    // already bounds its target rate here for the same reason; these three are the
    // rest of that family.
    add_command(commands, "tone", false,
                {number_value("frequency"),
                 with_domain(int_value("sr", 22050),
                             between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate,
                                     CliOptionDomainStage::Parameter)),
                 number_value("duration", 1.0), number_value("phase", 0.0),
                 number_value("amplitude", 1.0), required_output()});
    add_command(commands, "chirp", false,
                {with_domain(int_value("sr", 22050),
                             between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate,
                                     CliOptionDomainStage::Parameter)),
                 number_value("duration", 1.0), required_output(), flag("exponential"),
                 global_number("fmin", 0.0), global_number("fmax", 0.0)});
    add_command(commands, "clicks", false,
                {string_value("times"),
                 with_domain(int_value("sr", 22050),
                             between(sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate,
                                     CliOptionDomainStage::Parameter)),
                 int_value("length", 0), number_value("frequency", 1000.0),
                 number_value("click-duration", 0.1), required_output()});

#ifdef SONARE_WITH_MASTERING
    add_command(commands, "mastering", true,
                {string_value("preset"), path_value("config"), number_value("target-lufs", -14.0),
                 number_value("ceiling-db", -1.0), string_value("params"), bits_value(),
                 true_peak_oversample_value(), path_value("report"), output_value(),
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
         // enumerator.
         with_domain(int_value("type", 0), enum_index(0, 8, CliOptionDomainStage::Parameter)),
         number_value("frequency-hz", 1000.0), number_value("gain-db", 0.0), number_value("q", 1.0),
         with_domain(int_value("coeff-mode", 0), enum_index(0, 1, CliOptionDomainStage::Parameter)),
         int_value("slope-db-oct", 12),
         with_domain(int_value("placement", 0), enum_index(0, 4, CliOptionDomainStage::Parameter)),
         number_value("threshold-db", -24.0), number_value("ratio", 2.0),
         number_value("range-db", -6.0), number_value("attack-ms", 5.0),
         number_value("release-ms", 50.0),
         // "--lookahead-ms" is the flag's former (misleading) spelling,
         // registered as an alias so it still resolves to the same
         // "detector-delay-ms" storage key (see canonical_option_name()).
         number_value("detector-delay-ms", 0.0, false, false, true, {"lookahead-ms"}),
         number_value("sidechain-freq-hz", -1.0), number_value("sidechain-q", 1.0),
         with_domain(int_value("phase-mode", 1), enum_index(0, 3, CliOptionDomainStage::Parameter)),
         with_domain(int_value("resolution", 0), enum_index(0, 5, CliOptionDomainStage::Parameter)),
         number_value("gain-scale", 1.0), number_value("output-gain-db", 0.0),
         number_value("output-pan", 0.0), bits_value(), output_value(), flag("proportional-q"),
         flag("dynamic"), flag("auto-threshold"), flag("auto-gain")});
    add_command(commands, "mastering-pair-processor", true,
                {required_string("processor"), required_path("reference"), string_value("params"),
                 bits_value(), output_value()});
    add_command(commands, "mastering-pair-analyze", true,
                {required_string("analysis"), required_path("reference"), string_value("params")});
    add_command(commands, "mastering-stereo-analyze", true,
                {required_string("analysis"), required_path("reference"), string_value("params")});
    add_command(commands, "mastering-suggest", true, {string_value("params")});
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
    add_command(commands, "mix-strip", true,
                {number_value("input-trim-db", 0.0), number_value("fader-db", 0.0),
                 number_value("pan", 0.0), string_value("pan-mode", "balance"),
                 number_value("width", 1.0), output_value()},
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
         string_value("tempo-bpm", ""), string_value("scene-out", "")});
#endif

    // Feature leaves.
    // Zero is the librosa default both bounds are spelled with on every surface, so
    // the domains refuse negatives only: the handler substitutes sr/2 for a negative
    // `fmax`, which made a typo produce output bit-identical to omitting the flag,
    // and a negative `fmin` reaches the filterbank, where only fmax > fmin is
    // checked. `n-mels` needs none -- the filterbank's own ceiling refuses an
    // oversized band count for every command and every surface at once.
    add_command(
        commands, "mel", true,
        {global_int("n-fft", 2048), global_int("hop-length", 512), global_int("n-mels", 128),
         with_domain(global_number("fmin", 0.0), at_least(0.0, CliOptionDomainStage::Parameter)),
         with_domain(global_number("fmax", 0.0), at_least(0.0, CliOptionDomainStage::Parameter)),
         flag("htk")});
    add_command(commands, "chroma", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "tonnetz", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "spectral", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512)});
    // Every domain the Python CLI declares for this command, in the same
    // classes: the frequency/threshold `type=` callables are parse-time (usage)
    // and the algorithm name is a handler-level rejection (invalid parameter).
    add_command(
        commands, "pitch", true,
        {with_domain(string_value("algorithm", "pyin"),
                     choices_of({"yin", "pyin"}, CliOptionDomainStage::Parameter)),
         with_domain(number_value("threshold", 0.1),
                     above_zero_up_to(1.0, CliOptionDomainStage::Usage)),
         with_domain(global_int("hop-length", 512), greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(global_number("fmin", 65.0), greater_than(0.0, CliOptionDomainStage::Usage)),
         with_domain(global_number("fmax", 2093.0),
                     greater_than(0.0, CliOptionDomainStage::Usage))},
        {}, &validate_pitch_frequency_order);
    add_command(
        commands, "onset-env", true,
        {global_int("n-fft", 2048), global_int("hop-length", 512), global_int("n-mels", 128)});
    add_command(
        commands, "onset-envelope", true,
        {global_int("n-fft", 2048), global_int("hop-length", 512), global_int("n-mels", 128)});
    for (const char* path : {"fourier-tempogram", "tempogram-ratio"})
      add_command(commands, path, true,
                  {int_value("win-length", 384), global_int("hop-length", 512)});
    add_command(commands, "tempogram", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512),
                 global_int("n-mels", 128), int_value("win-length", 384)});
    add_command(commands, "plp", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512),
                 global_int("n-mels", 128), number_value("tempo-min", 30.0),
                 number_value("tempo-max", 300.0), int_value("win-length", 384)});
    add_command(commands, "nnls-chroma", true, {global_int("hop-length", 512)});
    // Zero keeps the 32.7 Hz default, so the domain refuses only a negative lower
    // bound -- which the handler otherwise substitutes that same default for,
    // leaving a typo indistinguishable from an omission.
    add_command(
        commands, "cqt", true,
        {int_value("n-bins", 84), int_value("bins-per-octave", 12), global_int("hop-length", 512),
         with_domain(global_number("fmin", 0.0), at_least(0.0, CliOptionDomainStage::Parameter))});
    add_command(
        commands, "vqt", true,
        {int_value("n-bins", 84), int_value("bins-per-octave", 12), number_value("gamma", 0.0),
         number_value("filter-scale", 1.0), global_int("hop-length", 512),
         with_domain(global_number("fmin", 0.0), at_least(0.0, CliOptionDomainStage::Parameter))});
    add_command(commands, "mel-to-audio", true,
                {int_value("n-iter", 32), required_output(), global_int("n-fft", 2048),
                 global_int("hop-length", 512), global_int("n-mels", 128),
                 global_number("fmin", 0.0), global_number("fmax", 0.0)});
    add_command(
        commands, "mfcc-to-audio", true,
        {int_value("n-mfcc", 13), int_value("n-iter", 32), required_output(),
         global_int("n-fft", 2048), global_int("hop-length", 512), global_int("n-mels", 128),
         global_number("fmin", 0.0), global_number("fmax", 0.0)});
    add_command(commands, "acoustic", true,
                {flag("ir"), int_value("n-bands", 6), number_value("min-decay-db", 30.0),
                 number_value("noise-floor-margin-db", 10.0)});

#ifdef SONARE_WITH_ACOUSTIC_SIM
    add_command(commands, "estimate-room", true,
                {number_value("aspect-lw", 1.0), number_value("aspect-lh", 1.0),
                 number_value("reference-absorption", 0.15), flag("sabine"),
                 make_option("n-octave-bands", CliOptionArity::RequiredValue,
                             CliOptionScalarType::Integer, null_default(), {"n-bands"})});
    // `absorption` reaches uniform_shoebox, which clamps to [0, 0.999] rather than
    // calling validate_material_coefficient beside it, so the C entry point's
    // refusal of anything outside [0, 1] has no counterpart on this path. `seed`
    // keeps 0 as the library-default sentinel the C ABI spells the same way, and
    // refuses the negatives the handler currently swallows into that default.
    add_command(
        commands, "synthesize-rir", false,
        {number_value("length", 7.0), number_value("width", 5.0), number_value("height", 3.0),
         with_domain(number_value("absorption", 0.2),
                     between(0.0, 1.0, CliOptionDomainStage::Parameter)),
         number_value("source-x", 1.0), number_value("source-y", 1.0),
         number_value("source-z", 1.2), number_value("listener-x", 5.0),
         number_value("listener-y", 4.0), number_value("listener-z", 1.7),
         int_value("sample-rate", 48000), int_value("ism-order", 3),
         with_domain(int_value("seed", 1), at_least(0.0, CliOptionDomainStage::Parameter)),
         number_value("max-seconds", 0.0), required_output(), flag("sabine")});
    add_command(
        commands, "room-morph", true,
        {number_value("length", 7.0), number_value("width", 5.0), number_value("height", 3.0),
         with_domain(number_value("absorption", 0.2),
                     between(0.0, 1.0, CliOptionDomainStage::Parameter)),
         number_value("source-x", 1.0), number_value("source-y", 1.0),
         number_value("source-z", 1.2), number_value("listener-x", 5.0),
         number_value("listener-y", 4.0), number_value("listener-z", 1.7),
         number_value("suppression", 0.5), number_value("wet", 0.5), int_value("ism-order", 3),
         with_domain(int_value("seed", 1), at_least(0.0, CliOptionDomainStage::Parameter)),
         number_value("max-seconds", 0.0), required_output(), flag("sabine")});
#endif

    // Metering and scalar utility leaves.
    add_command(commands, "lufs", true, {flag("series")});
    add_command(commands, "meter", true,
                {number_value("clip-threshold", 0.999), int_value("oversample", 4)});
    add_command(commands, "clipping", true,
                {number_value("threshold", 0.999), int_value("min-region", 1)});
    add_command(commands, "dynamic-range", true,
                {number_value("window-sec", 3.0), number_value("hop-sec", 1.0),
                 number_value("low-percentile", 0.10), number_value("high-percentile", 0.95)});
    add_command(commands, "stereo", true, {path_value("reference")});
    add_command(commands, "phase", true, {path_value("reference")});
    add_command(commands, "frames-to-samples", false,
                {int_value("frames"), global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "samples-to-frames", false,
                {int_value("samples"), global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "power-to-db", false,
                {string_value("values"), number_value("ref", 1.0), number_value("amin", 1e-10),
                 number_value("top-db", 80.0)});
    add_command(commands, "amplitude-to-db", false,
                {string_value("values"), number_value("ref", 1.0), number_value("amin", 1e-5),
                 number_value("top-db", 80.0)});
    add_command(commands, "db-to-power", false, {string_value("values"), number_value("ref", 1.0)});
    add_command(commands, "db-to-amplitude", false,
                {string_value("values"), number_value("ref", 1.0)});
    add_command(commands, "frame-signal", false,
                {string_value("values"), int_value("frame-length"), global_int("n-fft", 2048),
                 global_int("hop-length", 512)});
    add_command(commands, "pad-center", false,
                {string_value("values"), int_value("size"), number_value("pad-value", 0.0)});
    add_command(commands, "fix-length", false,
                {string_value("values"), int_value("size"), number_value("pad-value", 0.0)});
    add_command(
        commands, "fix-frames", false,
        {string_value("values"), int_value("x-min", 0), int_value("x-max", -1), flag("no-pad")});
    add_command(commands, "peak-pick", false,
                {string_value("values"), int_value("pre-max", 1), int_value("post-max", 1),
                 int_value("pre-avg", 1), int_value("post-avg", 1), number_value("delta", 0.0),
                 int_value("wait", 0)});
    add_command(
        commands, "vector-normalize", false,
        {string_value("values"),
         // Indexes NormType (inf, L1, L2, power); an index past the end used to
         // fall through to the inf norm and report success.
         with_domain(int_value("norm-type", 0), enum_index(0, 3, CliOptionDomainStage::Parameter)),
         number_value("threshold", 1e-12)});
    add_command(commands, "pcen", false,
                {string_value("values"), int_value("sample-rate", 22050),
                 number_value("time-constant", 0.4), number_value("gain", 0.98),
                 number_value("bias", 2.0), number_value("power", 0.5), number_value("eps", 1e-6),
                 int_value("n-bins"), int_value("n-frames"), global_int("hop-length", 512)});
    add_command(commands, "info", true, {});

    add_command(commands, "version", false, {});
    add_command(commands, "doctor", false, {});
    add_command(commands, "system-info", false, {});

#ifdef SONARE_WITH_ARRANGEMENT
    // Exactly ten project leaves; there is deliberately no broad `project`
    // option row. Each takes its subcommand as the one positional, so they are
    // registered through a helper rather than each restating that arity.
    const auto add_project_command = [&commands](const char* path,
                                                 std::vector<CliOptionSpec> options,
                                                 CliCommandValidator validate = nullptr) {
      add_command(commands, path, false, std::move(options), {}, validate, 1);
    };
    add_project_command("project.abi", {});
    add_project_command("project.synth-presets", {});
    add_project_command("project.new", {int_value("sample-rate", 0), required_output()});
    add_project_command("project.validate", {flag("strict"), required_path("in"), output_value()});
    add_project_command("project.compile", {required_path("in")});
    add_project_command(
        "project.bounce",
        {required_path("in"), required_output(), int_value("sample-rate"), int_value("frames", 0),
         int_value("block-size", 0), int_value("channels", 2), int_value("instrument-latency", 0),
         optional_string("synth")},
        &validate_project_bounce_channels);
    // `project bounce` with the synth pinned on, as a top-level leaf: it takes
    // no subcommand positional, and `--synth` is a plain value rather than an
    // optional flag because the rendering always goes through NativeSynth and
    // only the preset is in question.
    add_command(commands, "midi-render", false,
                {required_path("in"), required_output(), int_value("sample-rate"),
                 int_value("frames", 0), int_value("block-size", 0), int_value("channels", 2),
                 int_value("instrument-latency", 0), string_value("synth", "")},
                {}, &validate_project_bounce_channels);
    // Every numeric here means "keep the library default" when absent, and the
    // C ABI spells that 0 and refuses an out-of-domain value by name, so the
    // domains stay on that one entry rather than being restated per surface.
    add_command(commands, "transcribe", true,
                {required_output(), number_value("tempo-bpm"), flag("polyphonic"),
                 number_value("reference-hz"), number_value("fmin"), number_value("fmax"),
                 number_value("min-note-ms"), number_value("segmentation-threshold-cents"),
                 number_value("velocity-floor-db"), int_value("fixed-velocity"),
                 int_value("group", 0), int_value("channel", 0)});
    add_project_command("project.export-smf", {required_path("in"), required_output()});
    add_project_command("project.import-smf", {required_path("smf"), required_output()});
    add_project_command("project.export-midi2", {required_path("in"), required_output()});
    add_project_command("project.import-midi2", {required_path("midi2"), required_output()});
#endif
    return commands;
  }();
  return registry;
}

}  // namespace

const CliOptionSpec* option_for_spec(const CliCommandSpec* command, const std::string& option) {
  if (command == nullptr) return nullptr;
  for (const auto& item : command->options) {
    if (item.name == option ||
        std::find(item.aliases.begin(), item.aliases.end(), option) != item.aliases.end()) {
      return &item;
    }
  }
  return nullptr;
}

std::string command_path_for_args(const CliArgs& args) {
  if (args.command == "project" && !args.input_file.empty()) return "project." + args.input_file;
  return args.command;
}

const std::vector<CliCommandSpec>& cli_command_registry() { return build_cli_registry(); }

const CliCommandSpec* cli_command_spec_for_path(const std::string& path) {
  for (const auto& command : cli_command_registry()) {
    if (command.path == path) return &command;
  }
  for (const auto& command : cli_command_registry()) {
    if (std::find(command.aliases.begin(), command.aliases.end(), path) != command.aliases.end())
      return &command;
  }
  return nullptr;
}

const CliOptionSpec* cli_option_spec_for_command(const std::string& command,
                                                 const std::string& option) {
  return option_for_spec(cli_command_spec_for_path(command), option);
}

std::string describe_domain(const CliOptionDomain& domain) {
  if (!domain.choices.empty()) {
    std::string text = "one of ";
    for (size_t index = 0; index < domain.choices.size(); ++index) {
      if (index > 0) text += ", ";
      text += domain.choices[index];
    }
    return text;
  }
  std::ostringstream text;
  if (domain.has_minimum && domain.has_maximum) {
    text << (domain.exclusive_minimum ? "greater than " : "at least ") << domain.minimum << " and "
         << (domain.exclusive_maximum ? "less than " : "at most ") << domain.maximum;
  } else if (domain.has_minimum) {
    text << (domain.exclusive_minimum ? "greater than " : "at least ") << domain.minimum;
  } else {
    text << (domain.exclusive_maximum ? "less than " : "at most ") << domain.maximum;
  }
  return text.str();
}

bool value_matches_choice(const CliOptionSpec& spec, const std::string& value,
                          const std::string& choice) {
  const bool numeric = spec.scalar_type == CliOptionScalarType::Integer ||
                       spec.scalar_type == CliOptionScalarType::Number;
  if (!numeric) return value == choice;
  try {
    return parse_float_strict(spec.name, value) == parse_float_strict(spec.name, choice);
  } catch (const std::exception&) {
    return false;
  }
}

// Checks one supplied value against the option's declared domain. Numeric
// bounds are compared in double after the scalar parse the type already
// guarantees, so an integer option and a number option answer the same way.
std::string domain_error_for(const CliOptionSpec& spec, const std::string& value) {
  if (spec.domain.empty()) return {};
  if (!spec.domain.choices.empty()) {
    for (const auto& choice : spec.domain.choices) {
      if (value_matches_choice(spec, value, choice)) return {};
    }
    return "invalid value for --" + spec.name + ": " + value + " (expected " +
           describe_domain(spec.domain) + ")";
  }
  double parsed = 0.0;
  try {
    parsed = static_cast<double>(parse_float_strict(spec.name, value));
  } catch (const std::exception&) {
    // A value the scalar parse rejects is reported by that check, not here.
    return {};
  }
  const bool below =
      spec.domain.has_minimum && (spec.domain.exclusive_minimum ? parsed <= spec.domain.minimum
                                                                : parsed < spec.domain.minimum);
  const bool above =
      spec.domain.has_maximum && (spec.domain.exclusive_maximum ? parsed >= spec.domain.maximum
                                                                : parsed > spec.domain.maximum);
  if (!below && !above) return {};
  return "value out of range for --" + spec.name + ": " + value + " (expected " +
         describe_domain(spec.domain) + ")";
}

std::string validate_numeric_option_values(const CliArgs& args) {
  const CliCommandSpec* command = cli_command_spec_for_path(command_path_for_args(args));
  for (const auto& [key, value] : args.options) {
    const CliOptionSpec* spec = option_for_spec(command, key);
    if (spec == nullptr) continue;
    try {
      if (spec->scalar_type == CliOptionScalarType::Integer) {
        if (key == "candidates" && value == "true") continue;
        (void)parse_int_strict(key, value);
      } else if (spec->scalar_type == CliOptionScalarType::Number) {
        (void)parse_float_strict(key, value);
      }
    } catch (const std::exception& error) {
      return error.what();
    }
  }
  return {};
}

std::vector<CliOptionMetadata> cli_option_metadata_for_command(const std::string& command) {
  std::vector<CliOptionMetadata> result;
  const CliCommandSpec* spec = cli_command_spec_for_path(command);
  if (spec == nullptr) return result;
  for (const auto& option : spec->options) {
    if (!option.inventory) continue;
    CliOptionMetadata metadata;
    metadata.name = option.name;
    switch (option.scalar_type) {
      case CliOptionScalarType::Boolean:
        metadata.type = "boolean";
        break;
      case CliOptionScalarType::Integer:
        metadata.type = "integer";
        break;
      case CliOptionScalarType::Number:
        metadata.type = "number";
        break;
      case CliOptionScalarType::Path:
        metadata.type = "path";
        break;
      case CliOptionScalarType::String:
        metadata.type = "string";
        break;
    }
    metadata.default_kind = option.default_value.kind;
    metadata.default_boolean = option.default_value.boolean_value;
    metadata.default_integer = option.default_value.integer_value;
    metadata.default_number = option.default_value.number_value;
    metadata.default_string = option.default_value.string_value;
    metadata.default_string_array = option.default_value.string_array_value;
    metadata.aliases = option.aliases;
    metadata.repeatable = option.repeatable;
    metadata.arity = option.arity;
    metadata.scalar_type = option.scalar_type;
    // The published `required` field describes the PARSER contract, which is
    // what the cross-surface checker compares: an option the Python parser
    // declares `required=True` refuses the invocation at parse time (usage),
    // while one its handler refuses after parsing stays optional in its
    // inventory. An option this registry requires with the Parameter stage is
    // the second kind, so it publishes the same `false` -- the fact that the
    // command cannot run without it is declared in
    // tests/conformance/cli_option_domains.json, and the behaviour is pinned by
    // the shared parser cases.
    metadata.required = option.required && option.required_stage == CliOptionDomainStage::Usage;
    metadata.global_lexical = option.global_lexical;
    metadata.inventory = option.inventory;
    metadata.has_implicit_optional_default =
        option.implicit_optional_default.kind != CliOptionDefaultKind::Null;
    metadata.implicit_optional_default = option.implicit_optional_default;
    metadata.domain = option.domain;
    metadata.required_stage = option.required_stage;
    result.push_back(std::move(metadata));
  }
  return result;
}

std::vector<std::string> cli_options_for_command(const std::string& command) {
  std::vector<std::string> result;
  const CliCommandSpec* spec = cli_command_spec_for_path(command);
  if (spec == nullptr) return result;
  for (const auto& option : spec->options) {
    if (!option.inventory || option.name == "json") continue;
    std::string display = "--" + option.name;
    if (option.arity == CliOptionArity::RequiredValue)
      display += " <value>";
    else if (option.arity == CliOptionArity::OptionalValue)
      display += " [value]";
    result.push_back(std::move(display));
  }
  return result;
}

namespace {
/// Whether any leaf hangs below @p command as `<command>.<subcommand>`.
bool command_group_exists(const std::string& command) {
  const std::string prefix = command + ".";
  const auto& registry = cli_command_registry();
  return std::any_of(registry.begin(), registry.end(), [&prefix](const CliCommandSpec& spec) {
    return spec.path.rfind(prefix, 0) == 0;
  });
}
}  // namespace

CliValidationError validate_cli_arguments(const CliArgs& args, bool requires_audio) {
  const CliCommandSpec* command = cli_command_spec_for_path(command_path_for_args(args));
  const auto accepts_option = [&](const std::string& name) {
    return option_for_spec(command, name) != nullptr;
  };
  // Checked before the generic unknown-option sweep: `-o` reaches the option map
  // like any other option now, and this names the actual problem.
  if (!args.output_file.empty() && !accepts_option("output")) {
    return {"Command '" + args.command + "' does not produce a file output; remove -o/--output",
            false};
  }
  for (const auto& key : args.global_options) {
    if (!accepts_option(key))
      return {"Unknown option '--" + key + "' for command '" + args.command + "'", false};
  }
  for (const auto& option : args.options) {
    const std::string& key = option.first;
    if (!accepts_option(key)) {
      return {"Unknown option '" + (key.rfind("-", 0) == 0 ? key : "--" + key) + "' for command '" +
                  args.command + "'",
              false};
    }
  }
  if (!args.missing_value_options.empty())
    return {"Missing value for option '" + args.missing_value_options.front() + "'", false};
  if (const std::string numeric_error = validate_numeric_option_values(args);
      !numeric_error.empty())
    return {numeric_error, false};
  if (command != nullptr) {
    // Presence and domain, for every option of the selected leaf, from the
    // registry alone. A handler adds nothing to this: the same (command,
    // option, value) triple therefore has exactly one verdict and one exit
    // class, whichever handler ends up consuming it.
    for (const auto& option : command->options) {
      if (option.required && !args.has(option.name)) {
        return {"Missing required option '--" + option.name + "'",
                option.required_stage == CliOptionDomainStage::Parameter};
      }
      if (option.domain.empty() || !args.has(option.name)) continue;
      const std::string value = args.get_string(option.name);
      if (const std::string error = domain_error_for(option, value); !error.empty()) {
        return {error, option.domain.stage == CliOptionDomainStage::Parameter};
      }
    }
    if (command->validate != nullptr) {
      if (CliValidationError error = command->validate(args); !error.empty()) return error;
    }
  }

  // The leaf's own arity, so a command that takes a positional of some kind
  // other than an audio file declares it on its registry record instead of
  // being named in a list here that every later such command has to join. An
  // unresolved path still has to admit the subcommand a group reads from its
  // first positional, or `project help` is rejected as an unexpected argument
  // instead of reported as a subcommand that does not exist.
  size_t max_positionals = requires_audio ? 1u : 0u;
  if (command != nullptr) {
    max_positionals = command->positional_count;
  } else if (command_group_exists(args.command)) {
    max_positionals = 1u;
  }
  if (args.positionals.size() > max_positionals) {
    return {"Unexpected positional argument '" + args.positionals[max_positionals] +
                "' for command '" + args.command + "'",
            false};
  }
  return {};
}
