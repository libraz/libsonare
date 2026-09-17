#pragma once

/// @file sonare_cli_registry.h
/// @brief The immutable command/option contract and the validation it drives.

#include <string>
#include <vector>

#include "sonare_cli_args.h"

/// Validates command-specific option names, required option values, option
/// value domains, and positional arity. Returns an empty error when the
/// invocation is valid. Every option check the CLI performs before handler
/// dispatch runs here, driven by the registry: a handler never repeats one.
CliValidationError validate_cli_arguments(const CliArgs& args, bool requires_audio);
std::vector<std::string> cli_options_for_command(const std::string& command);

enum class CliOptionArity { Flag, RequiredValue, OptionalValue };
enum class CliOptionScalarType { Boolean, Integer, Number, Path, String };
enum class CliOptionDefaultKind { Null, Boolean, Integer, Number, String, StringArray };

/// Where a refused option value or a missing required option is reported.
///
/// `Usage` is the parse-time class (exit 2) and mirrors an argparse-level
/// rejection on the Python CLI -- a `type=` callable or a `choices=` tuple.
/// `Parameter` is the semantic class (exit 3) and mirrors a Python handler that
/// raises after parsing. Both CLIs publish one exit-code contract for a `shared`
/// command, so the stage has to be declared per option next to the domain
/// itself; deciding it inside the validator would make every option that needs
/// the other class an exception written somewhere else.
enum class CliOptionDomainStage { Usage, Parameter };

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
  CliOptionDomainStage stage = CliOptionDomainStage::Usage;

  bool empty() const noexcept { return !has_minimum && !has_maximum && choices.empty(); }
};

/// One option contract.  `global_lexical` means the parser may recognize the
/// spelling before the command/path is known; validation still gates it by the
/// selected leaf's option list.  `implicit_optional_default` is used for a
/// bare optional-value occurrence (for example `--synth`).  `domain` and
/// `required_stage` are the option's accepted-value and presence contracts;
/// `validate_cli_arguments` is the only place either is enforced.
struct CliOptionSpec {
  std::string name;
  std::vector<std::string> aliases;
  CliOptionArity arity = CliOptionArity::RequiredValue;
  CliOptionScalarType scalar_type = CliOptionScalarType::String;
  CliOptionValue default_value;
  CliOptionValue implicit_optional_default;
  bool required = false;
  /// Which exit class a missing required option reports. A registry-required
  /// option is a usage error by default; an option the Python CLI leaves
  /// optional in its parser and rejects in its handler declares Parameter so
  /// both CLIs refuse the same invocation with the same code.
  CliOptionDomainStage required_stage = CliOptionDomainStage::Usage;
  CliOptionDomain domain;
  bool repeatable = false;
  bool global_lexical = false;
  bool inventory = true;
};

/// A command-level check for a constraint that spans two options (for example
/// `--fmin` against `--fmax`), which no per-option domain can express. Returns
/// an empty error when the combination is accepted.
using CliCommandValidator = CliValidationError (*)(const CliArgs&);

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
  CliOptionDomainStage required_stage = CliOptionDomainStage::Usage;
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
