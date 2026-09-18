#include "sonare_cli_args.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "sonare_cli_registry.h"
#include "util/exception.h"

int cli_exit_code_for_error(sonare::ErrorCode error, bool legacy_mode) noexcept {
  if (legacy_mode) return 1;
  switch (error) {
    case sonare::ErrorCode::FileNotFound:
      return 4;
    case sonare::ErrorCode::InvalidFormat:
      return 5;
    case sonare::ErrorCode::DecodeFailed:
      return 6;
    case sonare::ErrorCode::InvalidParameter:
      return 3;
    case sonare::ErrorCode::OutOfMemory:
      return 7;
    case sonare::ErrorCode::NotImplemented:
      return 8;
    case sonare::ErrorCode::InvalidState:
      return 9;
    case sonare::ErrorCode::Cancelled:
      return 11;
    case sonare::ErrorCode::EncodeFailed:
      return 12;
    case sonare::ErrorCode::Ok:
    default:
      return 10;
  }
}

// Every numeric CLI value converts here, so a rejection always reads the same
// way. std::stof / std::stoi report two different failures for one option:
// a value with a numeric prefix ("1.5x") returns a partial conversion, while a
// value with none ("abc") throws from the standard library itself. Letting the
// second escape produced raw text such as "stoi: no conversion", with no option
// name and no rejected value, for the same option that reported properly on the
// first. Both are caught here and rephrased identically.
float parse_float_strict(const std::string& option, const std::string& value) {
  size_t consumed = 0;
  float parsed = 0.0f;
  try {
    parsed = std::stof(value, &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid float value for --" + option + ": " + value);
  }
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid float value for --" + option + ": " + value);
  }
  if (!std::isfinite(parsed)) {
    throw std::invalid_argument("numeric value for --" + option + " must be finite: " + value);
  }
  return parsed;
}

int parse_int_strict(const std::string& option, const std::string& value) {
  size_t consumed = 0;
  int parsed = 0;
  try {
    parsed = std::stoi(value, &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid integer value for --" + option + ": " + value);
  }
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid integer value for --" + option + ": " + value);
  }
  return parsed;
}

double parse_double_strict(const std::string& option, const std::string& value) {
  size_t consumed = 0;
  double parsed = 0.0;
  try {
    parsed = std::stod(value, &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid float value for --" + option + ": " + value);
  }
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid float value for --" + option + ": " + value);
  }
  if (!std::isfinite(parsed)) {
    throw std::invalid_argument("numeric value for --" + option + " must be finite: " + value);
  }
  return parsed;
}

long long parse_int64_strict(const std::string& option, const std::string& value) {
  size_t consumed = 0;
  long long parsed = 0;
  try {
    parsed = std::stoll(value, &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid integer value for --" + option + ": " + value);
  }
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid integer value for --" + option + ": " + value);
  }
  return parsed;
}

namespace {

const CliOptionSpec* lexical_option_for_spelling(const std::string& spelling) {
  const std::string option = spelling.rfind("--", 0) == 0  ? spelling.substr(2)
                             : spelling.rfind("-", 0) == 0 ? spelling.substr(1)
                                                           : spelling;
  for (const auto& command : cli_command_registry()) {
    for (const auto& item : command.options) {
      if (!item.global_lexical) continue;
      if (item.name == option ||
          std::find(item.aliases.begin(), item.aliases.end(), option) != item.aliases.end()) {
        return &item;
      }
    }
  }
  return nullptr;
}

std::string canonical_option_name(const CliCommandSpec* command, const std::string& option) {
  const CliOptionSpec* spec = option_for_spec(command, option);
  return spec == nullptr ? option : spec->name;
}

bool is_negative_number(const std::string& value) {
  if (value.size() <= 1 || value[0] != '-') return false;
  char* end = nullptr;
  std::strtod(value.c_str(), &end);
  return std::isdigit(static_cast<unsigned char>(value[1])) || value[1] == '.' ||
         (end != value.c_str() && end != nullptr && *end == '\0');
}

bool is_false_flag_literal(const std::string& value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (char c : value)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off";
}

void record_option(CliArgs& args, const std::string& spelling, const std::string& value,
                   CliOptionOccurrence::Kind kind) {
  args.option_occurrences.push_back({spelling, value, kind});
}

/// Projects the recorded occurrences onto `options` and `repeated_options`.
///
/// Run once, after the command token and the `project` subcommand are known, so
/// that every occurrence is classified against the same registry entry no
/// matter where on the command line it appeared. Handlers read only the two
/// derived containers, so this is the single point where an occurrence acquires
/// a canonical name and a repeatable option acquires its value list.
void resolve_option_occurrences(CliArgs& args) {
  const CliCommandSpec* command = cli_command_spec_for_path(command_path_for_args(args));
  args.options.clear();
  args.repeated_options.clear();
  for (const auto& occurrence : args.option_occurrences) {
    const std::string canonical = canonical_option_name(command, occurrence.spelling);
    const CliOptionSpec* spec = option_for_spec(command, canonical);
    switch (occurrence.kind) {
      case CliOptionOccurrence::Kind::FlagOff:
        args.options.erase(canonical);
        break;
      case CliOptionOccurrence::Kind::Flag:
        args.options[canonical] = occurrence.value;
        break;
      case CliOptionOccurrence::Kind::ImplicitValue:
        args.options[canonical] =
            spec != nullptr && spec->implicit_optional_default.kind == CliOptionDefaultKind::String
                ? spec->implicit_optional_default.string_value
                : occurrence.value;
        break;
      case CliOptionOccurrence::Kind::MissingValue:
        args.options[canonical] = "";
        break;
      case CliOptionOccurrence::Kind::Value:
        if (spec != nullptr && spec->repeatable)
          args.repeated_options[canonical].push_back(occurrence.value);
        args.options[canonical] = occurrence.value;
        break;
    }
  }
}

CliOptionValue static_default_for(const CliArgs& args, const std::string& key) {
  const CliCommandSpec* command = cli_command_spec_for_path(command_path_for_args(args));
  const CliOptionSpec* spec = option_for_spec(command, key);
  return spec == nullptr ? null_default() : spec->default_value;
}

std::map<std::string, std::string>::const_iterator option_value_for(const CliArgs& args,
                                                                    const std::string& key) {
  auto it = args.options.find(key);
  if (it != args.options.end()) return it;
  const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(args), key);
  if (spec == nullptr) return args.options.end();
  it = args.options.find(spec->name);
  if (it != args.options.end()) return it;
  for (const auto& alias : spec->aliases) {
    it = args.options.find(alias);
    if (it != args.options.end()) return it;
  }
  return args.options.end();
}

const std::vector<std::string>* repeated_values_for(const CliArgs& args, const std::string& key) {
  auto it = args.repeated_options.find(key);
  if (it != args.repeated_options.end()) return &it->second;
  const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(args), key);
  if (spec == nullptr) return nullptr;
  it = args.repeated_options.find(spec->name);
  if (it != args.repeated_options.end()) return &it->second;
  for (const auto& alias : spec->aliases) {
    it = args.repeated_options.find(alias);
    if (it != args.repeated_options.end()) return &it->second;
  }
  return nullptr;
}

}  // namespace

float CliArgs::get_float(const std::string& k, float def) const {
  const auto it = option_value_for(*this, k);
  if (it != options.end()) {
    const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(*this), k);
    return parse_float_strict(spec == nullptr ? k : spec->name, it->second);
  }
  const CliOptionValue value = static_default_for(*this, k);
  return value.kind == CliOptionDefaultKind::Number ? static_cast<float>(value.number_value) : def;
}

int CliArgs::get_int(const std::string& k, int def) const {
  const auto it = option_value_for(*this, k);
  if (it != options.end()) {
    const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(*this), k);
    return parse_int_strict(spec == nullptr ? k : spec->name, it->second);
  }
  const CliOptionValue value = static_default_for(*this, k);
  return value.kind == CliOptionDefaultKind::Integer ? value.integer_value : def;
}

unsigned CliArgs::get_uint32(const std::string& k, unsigned def) const {
  const auto it = option_value_for(*this, k);
  if (it == options.end()) {
    const CliOptionValue value = static_default_for(*this, k);
    return value.kind == CliOptionDefaultKind::Integer ? static_cast<unsigned>(value.integer_value)
                                                       : def;
  }
  const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(*this), k);
  const std::string& name = spec == nullptr ? k : spec->name;
  const long long parsed = parse_int64_strict(name, it->second);
  // The option's domain is what refuses a value outside the field; this is the
  // width backstop for a handler reaching here without one.
  if (parsed < 0 || parsed > static_cast<long long>(std::numeric_limits<unsigned>::max())) {
    std::ostringstream message;
    message << "value out of range for --" << name << ": " << it->second << " (expected 0 to "
            << std::numeric_limits<unsigned>::max() << ")";
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, message.str());
  }
  return static_cast<unsigned>(parsed);
}

namespace {

[[noreturn]] void reject_option_range(const std::string& option, const std::string& value,
                                      int minimum, int maximum) {
  std::ostringstream message;
  message << "value out of range for --" << option << ": " << value << " (expected ";
  if (maximum == std::numeric_limits<int>::max()) {
    message << minimum << " or greater)";
  } else {
    message << minimum << " to " << maximum << ")";
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, message.str());
}

}  // namespace

int CliArgs::get_int_in_range(const std::string& k, int minimum, int maximum, int def) const {
  if (!has(k)) {
    const int fallback = get_int(k, def);
    // A registry default outside the caller's range is a registry bug, not user
    // input; clamping would hide it, so report it the same way.
    if (fallback < minimum || fallback > maximum) {
      reject_option_range(k, std::to_string(fallback), minimum, maximum);
    }
    return fallback;
  }
  const int value = get_int(k, def);
  if (value < minimum || value > maximum) {
    reject_option_range(k, std::to_string(value), minimum, maximum);
  }
  return value;
}

int CliArgs::require_int_in_range(const std::string& k, int minimum, int maximum) const {
  if (!has(k)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "--" + k + " is required for this command");
  }
  return get_int_in_range(k, minimum, maximum, minimum);
}

bool CliArgs::has(const std::string& k) const {
  return option_value_for(*this, k) != options.end();
}

std::string CliArgs::get_string(const std::string& k, const std::string& def) const {
  const auto it = option_value_for(*this, k);
  if (it != options.end()) return it->second;
  const CliOptionValue value = static_default_for(*this, k);
  return value.kind == CliOptionDefaultKind::String ? value.string_value : def;
}

std::vector<std::string> CliArgs::get_string_list(const std::string& k) const {
  const std::vector<std::string>* values = repeated_values_for(*this, k);
  if (values != nullptr) return *values;
  const CliOptionValue value = static_default_for(*this, k);
  return value.kind == CliOptionDefaultKind::StringArray ? value.string_array_value
                                                         : std::vector<std::string>{};
}

CliArgs ArgParser::parse(int argc, char* argv[]) {
  CliArgs args;
  bool end_of_options = false;

  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--") {
        end_of_options = true;
      } else if (!end_of_options && (arg == "--help" || arg == "-h")) {
        args.help = true;
      } else if (!end_of_options && arg == "--json") {
        args.json_output = true;
      } else if (!end_of_options && (arg == "--quiet" || arg == "-q")) {
        args.quiet = true;
      } else if (!end_of_options && try_parse_global_option(args, arg, argv, i, argc)) {
        // Handled by the global lexical projection.
      } else if (!end_of_options && arg.size() > 2 && arg.substr(0, 2) == "--") {
        const size_t equals = arg.find('=');
        if (equals == std::string::npos) {
          parse_option(args, arg.substr(2), argv, i, argc);
        } else {
          const std::string key = arg.substr(2, equals - 2);
          const std::string value = arg.substr(equals + 1);
          parse_option(args, key, argv, i, argc, &value);
        }
      } else if (!end_of_options && try_parse_attached_short_option(args, arg, argv, i, argc)) {
        // Handled as a short option carrying its value.
      } else if (!end_of_options && arg.size() > 1 && arg[0] == '-') {
        record_option(args, arg, "true", CliOptionOccurrence::Kind::Flag);
      } else if (args.command.empty()) {
        args.command = arg;
      } else {
        args.positionals.push_back(arg);
        if (args.input_file.empty()) args.input_file = arg;
      }
    }
  } catch (const CliUsageError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    throw CliUsageError(error.what());
  } catch (const std::out_of_range& error) {
    throw CliUsageError(error.what());
  }
  resolve_option_occurrences(args);
  return args;
}

bool ArgParser::try_parse_global_option(CliArgs& args, const std::string& arg, char* argv[], int& i,
                                        int argc) {
  const size_t equals = arg.find('=');
  const std::string spelling = equals == std::string::npos ? arg : arg.substr(0, equals);
  const CliOptionSpec* spec = lexical_option_for_spelling(spelling);
  if (spec == nullptr) return false;
  if (spec->arity == CliOptionArity::Flag) {
    if (equals == std::string::npos) return false;
    const std::string value = arg.substr(equals + 1);
    if (spec->name == "json") args.json_output = !is_false_flag_literal(value);
    if (spec->name == "quiet") args.quiet = !is_false_flag_literal(value);
    if (spec->name == "help") args.help = !is_false_flag_literal(value);
    return true;
  }

  std::string value;
  if (equals != std::string::npos) {
    value = arg.substr(equals + 1);
    if (value.empty() && spec->name == "output") {
      args.missing_value_options.push_back(spelling);
      return true;
    }
  } else {
    if (i + 1 >= argc) {
      args.missing_value_options.push_back(spelling);
      return true;
    }
    const std::string next = argv[i + 1];
    if (next.size() > 1 && next[0] == '-' && !is_negative_number(next)) {
      args.missing_value_options.push_back(spelling);
      return true;
    }
    value = argv[++i];
    if (value.empty() && spec->name == "output") {
      args.missing_value_options.push_back(spelling);
      return true;
    }
  }

  try {
    switch (spec->scalar_type) {
      case CliOptionScalarType::Integer: {
        const int parsed = parse_int_strict(spec->name, value);
        if (spec->name == "n-fft") {
          args.n_fft = parsed;
          args.n_fft_explicit = true;
        } else if (spec->name == "hop-length") {
          args.hop_length = parsed;
        } else if (spec->name == "n-mels") {
          args.n_mels = parsed;
        }
        break;
      }
      case CliOptionScalarType::Number:
        if (spec->name == "fmin") args.fmin = parse_float_strict(spec->name, value);
        if (spec->name == "fmax") args.fmax = parse_float_strict(spec->name, value);
        break;
      case CliOptionScalarType::Path:
        args.output_file = value;
        break;
      case CliOptionScalarType::Boolean:
      case CliOptionScalarType::String:
        break;
    }
  } catch (const std::out_of_range&) {
    throw std::invalid_argument("value out of range for " + spelling + ": " + value);
  }
  // A global option is projected into a dedicated field above, but it also has
  // to land in the generic option map: everything downstream -- has(),
  // get_int()/get_float(), the numeric check, and the registry domain check --
  // is keyed off that map. Without this, `--hop-length 1` set the field and
  // then get_int("hop-length", ...) missed it, fell through to the registry
  // default, and silently computed with 512; the same hole hid every domain
  // this layer declares for a global option.
  record_option(args, spec->name, value, CliOptionOccurrence::Kind::Value);
  if (spec->name != "output") args.global_options.push_back(spec->name);
  return true;
}

bool ArgParser::try_parse_attached_short_option(CliArgs& args, const std::string& arg, char* argv[],
                                                int& i, int argc) {
  if (arg.size() <= 2 || arg[0] != '-' || arg[1] == '-') return false;
  const std::string letter = arg.substr(1, 1);
  std::string value = arg.substr(2);
  // `-o=out.wav` reaches the global handler first; a command-scoped short
  // option written the same way arrives here with the separator still attached.
  if (value.front() == '=') value.erase(0, 1);

  const CliOptionSpec* global = lexical_option_for_spelling("-" + letter);
  if (global != nullptr) {
    if (global->arity == CliOptionArity::Flag) return false;
    return try_parse_global_option(args, "-" + letter + "=" + value, argv, i, argc);
  }
  const CliOptionSpec* spec =
      option_for_spec(cli_command_spec_for_path(command_path_for_args(args)), letter);
  if (spec == nullptr || spec->arity == CliOptionArity::Flag) return false;
  parse_option(args, letter, argv, i, argc, &value);
  return true;
}

void ArgParser::parse_option(CliArgs& args, const std::string& key, char* argv[], int& i, int argc,
                             const std::string* inline_value) {
  const CliCommandSpec* command = cli_command_spec_for_path(command_path_for_args(args));
  const CliOptionSpec* spec = option_for_spec(command, key);
  const CliOptionArity arity = spec == nullptr ? CliOptionArity::RequiredValue : spec->arity;

  if (arity == CliOptionArity::Flag) {
    record_option(args, key, "true",
                  inline_value != nullptr && is_false_flag_literal(*inline_value)
                      ? CliOptionOccurrence::Kind::FlagOff
                      : CliOptionOccurrence::Kind::Flag);
    return;
  }
  if (inline_value != nullptr) {
    if (inline_value->empty() && arity == CliOptionArity::RequiredValue) {
      record_option(args, key, "", CliOptionOccurrence::Kind::MissingValue);
      args.missing_value_options.push_back("--" + key);
    } else {
      record_option(args, key, *inline_value, CliOptionOccurrence::Kind::Value);
    }
    return;
  }
  if (i + 1 < argc) {
    const std::string next = argv[i + 1];
    const bool is_option = next.size() > 1 && next[0] == '-' && !is_negative_number(next);
    if (!is_option) {
      record_option(args, key, argv[++i], CliOptionOccurrence::Kind::Value);
      return;
    }
  }
  if (arity == CliOptionArity::OptionalValue) {
    record_option(args, key, "true", CliOptionOccurrence::Kind::ImplicitValue);
  } else {
    record_option(args, key, "", CliOptionOccurrence::Kind::MissingValue);
    args.missing_value_options.push_back("--" + key);
  }
}
