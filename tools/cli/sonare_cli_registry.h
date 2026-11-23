#pragma once

/// @file sonare_cli_registry.h
/// @brief The immutable command/option contract and the validation it drives.

#include <limits>
#include <string>
#include <vector>

#include "sonare_cli_args.h"
#include "util/constants.h"

/// Validates command-specific option names, required option values, option
/// value domains, and positional arity. Returns an empty error when the
/// invocation is valid. Every option check the CLI performs before handler
/// dispatch runs here, driven by the registry: a handler never repeats one.
CliValidationError validate_cli_arguments(const CliArgs& args, bool requires_audio);
std::vector<std::string> cli_options_for_command(const std::string& command);

enum class CliOptionArity { Flag, RequiredValue, OptionalValue };
enum class CliOptionScalarType { Boolean, Integer, Number, Path, String };
enum class CliOptionDefaultKind { Null, Boolean, Integer, Number, String, StringArray };

/// A typed value in the immutable CLI registry.  `Null` means that the option
/// has no static default; it is also how required values are represented.
struct CliOptionValue {
  CliOptionDefaultKind kind = CliOptionDefaultKind::Null;
  bool boolean_value = false;
  int integer_value = 0;
  double number_value = 0.0;
  std::string string_value;
  std::vector<std::string> string_array_value;
};

/// The accepted value set for one option, beyond what its scalar type parses.
///
/// An empty domain accepts every value the type parses, which is the state of
/// an option whose contract has not been narrowed. Bounds apply to Integer and
/// Number options; `choices` is compared as a parsed number for those and as a
/// literal for String and Path options. Exclusive bounds exist because the
/// Python CLI expresses several domains that way (`> 0`, `> 0 and <= 1`).
struct CliOptionDomain {
  bool has_minimum = false;
  double minimum = 0.0;
  bool exclusive_minimum = false;
  bool has_maximum = false;
  double maximum = 0.0;
  bool exclusive_maximum = false;
  std::vector<std::string> choices;

  bool empty() const noexcept { return !has_minimum && !has_maximum && choices.empty(); }
};

/// One option contract.  `global_lexical` means the parser may recognize the
/// spelling before the command/path is known; validation still gates it by the
/// selected leaf's option list.  `implicit_optional_default` is used for a
/// bare optional-value occurrence (for example `--synth`).  `domain` and
/// `required` are the option's accepted-value and presence contracts;
/// `validate_cli_arguments` is the only place either is enforced.
struct CliOptionSpec {
  std::string name;
  std::vector<std::string> aliases;
  CliOptionArity arity = CliOptionArity::RequiredValue;
  CliOptionScalarType scalar_type = CliOptionScalarType::String;
  CliOptionValue default_value;
  CliOptionValue implicit_optional_default;
  bool required = false;
  CliOptionDomain domain;
  bool repeatable = false;
  bool global_lexical = false;
  bool inventory = true;
  /// An Integer option that also reads a mode name (`major`, `dorian`, ...) as
  /// the scale mask of that mode. The domain is checked on the resolved mask.
  bool accepts_mode_name = false;
  /// An Integer option backed by an `int64_t` field: parsed at 64 bits, and a lower
  /// bound alone leaves it unbounded above rather than ending at INT_MAX.
  bool int64_width = false;
  /// What the library takes when the option is left out, as `--help` states it. Set only on an
  /// option with no registry default, whose handler then keeps the library's own value.
  std::string stated_default;
  /// Short description shown beside the option in command help.
  std::string description;
};

/// A command-level check for a constraint that spans two options (for example
/// `--f0-min-hz` against `--f0-max-hz`), which no per-option domain can express. Returns
/// an empty error when the combination is accepted.
using CliCommandValidator = CliValidationError (*)(const CliArgs&);

/// `CliCommandSpec::positional_count` of a leaf that takes any number of positionals.
constexpr size_t kUnboundedPositionals = std::numeric_limits<size_t>::max();

/// A leaf command/path contract.  Project routes are represented as the ten
/// `project.<subcommand>` leaves rather than one broad project schema.
struct CliCommandSpec {
  std::string path;
  std::vector<std::string> aliases;
  std::vector<CliOptionSpec> options;
  bool requires_audio = false;
  /// How many positional arguments this leaf accepts. `requires_audio` is one
  /// of them -- the audio file -- so the count is carried here rather than
  /// derived, and a leaf whose positional is something else (the `project`
  /// subcommand, a preset document, a MIDI value) says so on its own record.
  size_t positional_count = 0;
  /// Names the last positional (`project.json`) when the leaf requires all
  /// `positional_count` of them, so a missing one is refused by name; empty
  /// leaves any shortfall to the handler.
  std::string positional_name;
  /// True when the handler carries a two-channel input through as a stereo
  /// pair. Every audio leaf is handed the mono downmix main() decodes, so a
  /// leaf that re-reads the file to keep both channels says so here rather than
  /// being recognized by name wherever that difference matters.
  bool preserves_stereo_input = false;
  /// True when the handler reads every plane of the input file itself (1, 2, 6
  /// or 8 channels), rather than the mono downmix main() decodes. Generalizes
  /// `preserves_stereo_input` to the wider widths a speaker-layout command
  /// accepts, so main()'s downmix-warning suppression covers both from one
  /// leaf-declared flag instead of a name check for each width.
  bool preserves_multichannel_input = false;
  bool inventory = true;
  /// Cross-option constraint, run after every per-option check passes.
  CliCommandValidator validate = nullptr;
};

/// The single native CLI option/path registry. The returned vector and all of
/// its records are immutable after first initialization.
const std::vector<CliCommandSpec>& cli_command_registry();
const CliCommandSpec* cli_command_spec_for_path(const std::string& path);
const CliOptionSpec* cli_option_spec_for_command(const std::string& command,
                                                 const std::string& option);

/// The default `--help` states for @p option, and the text the inventory publishes: the
/// stated library default, else the registry default (a number keeps one decimal place when
/// integral). Empty for a flag, a required or repeatable option, and an empty string.
std::string cli_option_stated_default(const CliOptionSpec& option);

// The command parser and hidden contract inventory project this metadata from
// the same registry. These fields retain the stable JSON-facing projection;
// the richer arity/required/default-binding fields are available on the
// registry records above.
struct CliOptionMetadata {
  std::string name;
  std::string type;
  CliOptionDefaultKind default_kind = CliOptionDefaultKind::String;
  bool default_boolean = false;
  int default_integer = 0;
  double default_number = 0.0;
  std::string default_string;
  std::vector<std::string> default_string_array;
  std::vector<std::string> aliases;
  bool repeatable = false;
  CliOptionArity arity = CliOptionArity::RequiredValue;
  CliOptionScalarType scalar_type = CliOptionScalarType::String;
  bool required = false;
  bool global_lexical = false;
  bool inventory = true;
  bool has_implicit_optional_default = false;
  CliOptionValue implicit_optional_default;
  /// Projected verbatim so the contract dump publishes the accepted value set,
  /// which is what lets the cross-surface checker compare it with the Python
  /// parser's `choices=` / `type=` domains instead of assuming they agree.
  CliOptionDomain domain;
  /// The names the option reads beside its domain (the mode names of a scale mask).
  std::vector<std::string> names;
  /// `cli_option_stated_default`, the text `--help` prints.
  std::string stated_default;
};

std::vector<CliOptionMetadata> cli_option_metadata_for_command(const std::string& command);

/// Return the hidden machine-readable command/option inventory used by the
/// cross-surface CLI contract checker.
std::string dump_cli_contract_json();

/// Declared here rather than kept file-local because the parser and the CliArgs
/// accessors resolve every occurrence against the leaf these three select.
CliOptionValue null_default();
const CliOptionSpec* option_for_spec(const CliCommandSpec* command, const std::string& option);
std::string command_path_for_args(const CliArgs& args);

/// The mixer rate `mix` and `suggest-mix` run at when `--sample-rate` is left out.
inline constexpr int kCliMixerSampleRate =
    static_cast<int>(sonare::constants::kDefaultDawSampleRate);

/// The `filter --type` tokens, indexed by the SonareBiquadFilterType each one selects.
inline const std::vector<std::string>& filter_type_tokens() {
  static const std::vector<std::string> tokens = {"high-pass", "low-pass", "band-pass", "notch"};
  return tokens;
}

#ifdef SONARE_WITH_MASTERING
/// The first refusal of `repair`'s mode switches, `--stages` and `--params`, decided from
/// the command line alone, or empty; defined in sonare_cli_mastering_args.cpp.
std::string repair_argument_refusal(const CliArgs& args);
/// The refusal of a `mastering-streaming --platforms` document, prefixed
/// "argument --platforms: ", or empty; defined in sonare_cli_mastering_args.cpp.
std::string streaming_platforms_refusal(const CliArgs& args);
#endif

#if defined(SONARE_WITH_MIXING) || defined(SONARE_WITH_MIXING_ASSISTANT)
/// The id a bare `--input` path stands for: the file name without its last extension.
std::string bare_path_id(const std::string& entry);
#endif
#ifdef SONARE_WITH_MIXING
/// The first malformed `mix --input [ID=]WAV` entry, or empty.
std::string mix_input_refusal(const CliArgs& args);
#endif
#ifdef SONARE_WITH_MIXING_ASSISTANT
/// Split `[ID=]WAV` into its two halves; a bare path takes the file's own name as the id.
void split_track_entry(const std::string& entry, std::string* id, std::string* path);
/// The first refusal of `suggest-mix`'s `--input` entries and `--params` list, or its
/// `--tempo-bpm` clash with `--params`, decided from the command line alone, or empty.
std::string suggest_mix_argument_refusal(const CliArgs& args);
#endif
