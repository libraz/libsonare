#pragma once

/// @file sonare_cli_args.h
/// @brief The parsed native CLI invocation and the parser that produces it.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/types.h"

/// One option occurrence the parser accepted, in command-line order, recorded
/// as it was written rather than as it will be interpreted.
///
/// What an occurrence means -- its canonical name, whether it is repeatable,
/// the value a bare optional-value occurrence stands for -- comes from the
/// selected command's registry entry, and that entry is only resolvable once
/// the command token (and, for `project`, its subcommand) has been read. An
/// occurrence written before the command token has no spec to consult at the
/// moment it is parsed, so classifying it there makes an option's meaning
/// depend on its position: a pre-command `--set` was landing in `options` as a
/// last-one-wins scalar and never reaching `repeated_options`, leaving the
/// handler with zero assignments and an exit code of 0. The parser records what
/// it consumed; ArgParser::parse resolves the whole list once, at the end,
/// against the command it now knows.
struct CliOptionOccurrence {
  enum class Kind {
    Value,          ///< An explicit value was consumed from the command line.
    ImplicitValue,  ///< A bare optional-value occurrence; the spec supplies the value.
    Flag,           ///< A flag occurrence, or a bare short option.
    FlagOff,        ///< `--flag=false`, which cancels any earlier occurrence.
    MissingValue,   ///< A required value was absent; recorded so the key still exists.
  };

  /// The option name the parser read, without its leading dashes. A global
  /// option is recorded under its registry name, because the lexical scan has
  /// already resolved it against a command-independent list; every other
  /// occurrence keeps the spelling it was given, so resolution is the one place
  /// an alias is mapped onto its canonical name. A bare short option matching
  /// no spec keeps its dash and stays verbatim.
  std::string spelling;
  std::string value;
  Kind kind = Kind::Value;
};

struct CliArgs {
  std::string command;
  std::string input_file;
  std::string output_file;
  // Populated after the input has been decoded by the native CLI. This is
  // runtime metadata, not a user-facing option; zero means that no source
  // channel probe was available.
  int source_channels = 0;
  bool json_output = false;
  bool quiet = false;
  bool help = false;

  int n_fft = 2048;
  bool n_fft_explicit = false;
  int hop_length = 512;
  int n_mels = 128;
  float fmin = 0.0f;
  float fmax = 0.0f;

  // Every option occurrence the parser accepted, in command-line order. This is
  // the parser's own record; `options` and `repeated_options` below are derived
  // from it once the command is known and are what handlers read.
  std::vector<CliOptionOccurrence> option_occurrences;
  std::map<std::string, std::string> options;
  // Every occurrence of a repeatable option, in command-line order. A
  // repeatable option also lands in `options` (last occurrence wins) so that
  // name-based lookups such as has() and the validators stay uniform, but the
  // occurrences must never be folded into one string: a `--set` value is
  // arbitrary JSON, so any separator a fold could pick is also a legal byte
  // inside the value.
  std::map<std::string, std::vector<std::string>> repeated_options;
  // Global DSP options are parsed into dedicated fields above, but their
  // spelling must still be validated against the selected command.
  std::vector<std::string> global_options;
  std::vector<std::string> positionals;
  std::vector<std::string> missing_value_options;

  float get_float(const std::string& k, float def) const;
  int get_int(const std::string& k, int def) const;
  /// Reads an optional integer option and rejects a value outside
  /// [@p minimum, @p maximum].
  ///
  /// Handlers must not narrow a raw get_int result to an unsigned type: a
  /// negative value becomes a huge positive bound, which reads as a nonsensical
  /// threshold downstream and surfaces as a generic internal failure rather
  /// than a rejected parameter. Range checking belongs here, once, so the
  /// message names the option and the rejected value the same way for every
  /// command.
  /// @throws sonare::SonareException(InvalidParameter) — CLI exit 3.
  int get_int_in_range(const std::string& k, int minimum, int maximum, int def) const;
  /// Range-checked integer option with no usable default. Absent is rejected
  /// too: a command that cannot produce meaningful output without the option
  /// must fail rather than succeed with empty output.
  /// @throws sonare::SonareException(InvalidParameter) — CLI exit 3.
  int require_int_in_range(const std::string& k, int minimum, int maximum) const;
  bool has(const std::string& k) const;
  std::string get_string(const std::string& k, const std::string& def = "") const;
  /// Reads every occurrence of a repeatable option, in command-line order.
  /// Each element is one raw value exactly as it was given on the command
  /// line, so a value carrying its own commas stays intact.
  std::vector<std::string> get_string_list(const std::string& k) const;
};

/// One rejected invocation. `message` is empty when the invocation is valid.
///
/// `invalid_parameter` selects between the two exit codes the CLI contract
/// publishes for a refused argument: false is the usage code (2), true is the
/// invalid-parameter code (3). Which one an option uses is part of that
/// option's contract rather than a property of the checking code, because the
/// two CLIs must agree per option -- see CliOptionDomainStage.
struct CliValidationError {
  std::string message;
  bool invalid_parameter = false;

  bool empty() const noexcept { return message.empty(); }
};

/// Maps a core error to the stable native CLI exit-code contract.  The legacy
/// mode intentionally folds every error to one, matching the Python CLI.
int cli_exit_code_for_error(sonare::ErrorCode error, bool legacy_mode) noexcept;

class ArgParser {
 public:
  static CliArgs parse(int argc, char* argv[]);

 private:
  static bool try_parse_global_option(CliArgs& args, const std::string& arg, char* argv[], int& i,
                                      int argc);
  static void parse_option(CliArgs& args, const std::string& key, char* argv[], int& i, int argc,
                           const std::string* inline_value = nullptr);
};

/// Parse-time argument errors use the CLI usage exit code (2), rather than the
/// handler-level invalid-parameter code.  Keep this distinct from
/// std::invalid_argument because handlers also use that exception for semantic
/// validation after parsing.
class CliUsageError final : public std::invalid_argument {
 public:
  explicit CliUsageError(const std::string& message) : std::invalid_argument(message) {}
};

/// Declared here rather than kept file-local because the registry's value
/// checks parse a candidate value the same way the accessors above do.
float parse_float_strict(const std::string& option, const std::string& value);
int parse_int_strict(const std::string& option, const std::string& value);
