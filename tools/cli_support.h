#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/types.h"

class JsonBuilder {
 public:
  JsonBuilder& begin_object();
  JsonBuilder& end_object();
  JsonBuilder& begin_array();
  JsonBuilder& end_array();
  JsonBuilder& key(const std::string& k);
  JsonBuilder& value(const std::string& v);
  JsonBuilder& value(const char* v);
  JsonBuilder& value(int v);
  JsonBuilder& value(size_t v);
  JsonBuilder& value(float v);
  JsonBuilder& value(double v);
  JsonBuilder& value(bool v);
  JsonBuilder& null_value();
  JsonBuilder& kv(const std::string& k, const std::string& v);
  JsonBuilder& kv(const std::string& k, const char* v);
  JsonBuilder& kv(const std::string& k, int v);
  JsonBuilder& kv(const std::string& k, size_t v);
  JsonBuilder& kv(const std::string& k, float v);
  JsonBuilder& kv(const std::string& k, double v);
  JsonBuilder& kv(const std::string& k, bool v);
  JsonBuilder& float_array(const std::vector<float>& arr);
  std::string build() const;
  void print() const;

 private:
  void append_separator();
  static std::string escape(const std::string& s);

  std::ostringstream ss_;
  std::vector<bool> needs_comma_;
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

/// Validates command-specific option names, required option values, and
/// positional arity. Returns an empty string when the invocation is valid.
std::string validate_cli_arguments(const CliArgs& args, bool requires_audio);
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

/// One option contract.  `global_lexical` means the parser may recognize the
/// spelling before the command/path is known; validation still gates it by the
/// selected leaf's option list.  `implicit_optional_default` is used for a
/// bare optional-value occurrence (for example `--synth`).
struct CliOptionSpec {
  std::string name;
  std::vector<std::string> aliases;
  CliOptionArity arity = CliOptionArity::RequiredValue;
  CliOptionScalarType scalar_type = CliOptionScalarType::String;
  CliOptionValue default_value;
  CliOptionValue implicit_optional_default;
  bool required = false;
  bool repeatable = false;
  bool global_lexical = false;
  bool inventory = true;
};

/// A leaf command/path contract.  Project routes are represented as the ten
/// `project.<subcommand>` leaves rather than one broad project schema.
struct CliCommandSpec {
  std::string path;
  std::vector<std::string> aliases;
  std::vector<CliOptionSpec> options;
  bool requires_audio = false;
  bool inventory = true;
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
};

std::vector<CliOptionMetadata> cli_option_metadata_for_command(const std::string& command);

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

/// Return the hidden machine-readable command/option inventory used by the
/// cross-surface CLI contract checker.
std::string dump_cli_contract_json();

struct Stats {
  float mean = 0.0f;
  float std = 0.0f;
  float min = 0.0f;
  float max = 0.0f;

  static Stats compute(const std::vector<float>& v);
};

namespace color {
/// Configure ANSI escape sequences for interactive terminals. `NO_COLOR` and
/// either redirected standard stream disable them, keeping CLI output safe for
/// pipes, files, and machine parsers.
void configure();
extern const char* reset;
extern const char* bold;
extern const char* cyan;
extern const char* green;
extern const char* magenta;
extern const char* yellow;
extern const char* blue;
extern const char* red;
}  // namespace color

namespace system_info {
int logical_cores();
int physical_cores();
size_t total_memory_bytes();
size_t available_memory_bytes();
std::string parallel_strategy();
int parallel_workers();
bool parallel_enabled();
}  // namespace system_info

struct StageInfo {
  int number;
  int total;
  const char* description;
};

StageInfo get_stage_info(const char* stage);
void progress_callback(float progress, const char* stage);
void clear_progress();
std::string describe_level(float value, const char* low, const char* mid, const char* high);
std::string basename(const std::string& path);
