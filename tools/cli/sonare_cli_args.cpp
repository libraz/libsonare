#include "sonare_cli_args.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "analysis/key_profiles.h"
#include "c_api/sonare_c_error_mapping.h"
#include "sonare_cli_registry.h"
#include "util/exception.h"
#include "util/refusal_predicates.h"

int cli_exit_code_for_error(sonare::ErrorCode error) noexcept {
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
    case sonare::ErrorCode::NotSupported:
      return 8;
    case sonare::ErrorCode::InvalidState:
      return 9;
    case sonare::ErrorCode::Cancelled:
      return 11;
    case sonare::ErrorCode::EncodeFailed:
      return 12;
    // Exit codes are not error codes: error 11 (StaleResult) exits 14, exit 11 is a cancellation.
    case sonare::ErrorCode::StaleResult:
      return 14;
    case sonare::ErrorCode::AbiMismatch:
      return 13;
    case sonare::ErrorCode::Unknown:
    case sonare::ErrorCode::Ok:
    default:
      return 10;
  }
}

int cli_exit_code_for_c_error(SonareError error) noexcept {
  return cli_exit_code_for_error(sonare_c_detail::error_code_from_c_error(error));
}

namespace {

bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

size_t skip_ascii_space(const std::string& text) {
  size_t i = 0;
  while (i < text.size() && is_ascii_space(text[i])) ++i;
  return i;
}

// Leading ASCII space, an optional sign, decimal digits, and nothing after them.
bool is_integer_text(const std::string& text) {
  size_t i = skip_ascii_space(text);
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
  const size_t digits = i;
  while (i < text.size() && is_ascii_digit(text[i])) ++i;
  return i > digits && i == text.size();
}

// The integer grammar's decimal-number sibling: digits with an optional point and
// exponent, or inf / infinity / nan in any letter case. No hex, no separators,
// no trailing text -- the set the Python front-end reads as well.
bool is_number_text(const std::string& text) {
  size_t i = skip_ascii_space(text);
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
  std::string word = text.substr(i);
  std::transform(word.begin(), word.end(), word.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (word == "inf" || word == "infinity" || word == "nan") return true;
  size_t mantissa_digits = 0;
  while (i < text.size() && is_ascii_digit(text[i])) ++i, ++mantissa_digits;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && is_ascii_digit(text[i])) ++i, ++mantissa_digits;
  }
  if (mantissa_digits == 0) return false;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    const size_t exponent = i;
    while (i < text.size() && is_ascii_digit(text[i])) ++i;
    if (i == exponent) return false;
  }
  return i == text.size();
}

[[noreturn]] void refuse_value(const std::string& subject, const char* requirement) {
  throw std::invalid_argument(subject + " " + requirement);
}

template <typename Int>
[[noreturn]] void refuse_integer_range(const std::string& subject) {
  throw std::invalid_argument(subject + " must be an integer in [" +
                              std::to_string(std::numeric_limits<Int>::min()) + ", " +
                              std::to_string(std::numeric_limits<Int>::max()) + "]");
}

long long read_integer_text(const std::string& subject, const std::string& value, bool* overflow) {
  if (!is_integer_text(value)) refuse_value(subject, "must be an integer");
  errno = 0;
  const long long parsed = std::strtoll(value.c_str(), nullptr, 10);
  *overflow = errno == ERANGE;
  return parsed;
}

}  // namespace

std::string option_subject(const std::string& option) { return "argument --" + option + ":"; }

double parse_double_text(const std::string& subject, const std::string& value) {
  if (!is_number_text(value)) refuse_value(subject, "must be a number");
  const double parsed = std::strtod(value.c_str(), nullptr);
  if (!std::isfinite(parsed)) refuse_value(subject, "must be finite");
  return parsed;
}

float parse_float_text(const std::string& subject, const std::string& value) {
  const double parsed = parse_double_text(subject, value);
  // The library's own bound for a float argument; an underflow to 0 is a number.
  if (std::fabs(parsed) > static_cast<double>(std::numeric_limits<float>::max())) {
    refuse_value(subject, "must be finite");
  }
  return std::strtof(value.c_str(), nullptr);
}

int parse_int_text(const std::string& subject, const std::string& value) {
  bool overflow = false;
  const long long parsed = read_integer_text(subject, value, &overflow);
  if (overflow || parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    refuse_integer_range<int>(subject);
  }
  return static_cast<int>(parsed);
}

long long parse_int64_text(const std::string& subject, const std::string& value) {
  bool overflow = false;
  const long long parsed = read_integer_text(subject, value, &overflow);
  if (overflow) refuse_integer_range<long long>(subject);
  return parsed;
}

float parse_float_strict(const std::string& option, const std::string& value) {
  return parse_float_text(option_subject(option), value);
}

int parse_int_strict(const std::string& option, const std::string& value) {
  return parse_int_text(option_subject(option), value);
}

std::string number_list_refusal(const std::string& option, const std::string& text, bool integral,
                                size_t* count) {
  size_t read = 0;
  size_t begin = 0;
  while (begin <= text.size()) {
    size_t end = text.find(',', begin);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(begin, end - begin);
    begin = end + 1;
    if (item.empty()) continue;
    try {
      if (integral) {
        parse_int_strict(option, item);
      } else {
        parse_float_strict(option, item);
      }
    } catch (const std::invalid_argument&) {
      read = 0;
      break;
    }
    ++read;
  }
  if (count != nullptr) *count = read;
  if (read > 0) return {};
  return option_subject(option) + " must be a comma-separated list of finite numbers";
}

std::string voice_set_refusal(const std::vector<std::string>& assignments) {
  for (const auto& assignment : assignments) {
    if (assignment.empty()) continue;
    const auto eq = assignment.find('=');
    if (eq == std::string::npos || eq == 0) return "invalid --set assignment: " + assignment;
    if (assignment.find_first_not_of('.') >= eq) return "empty --set path";
  }
  return {};
}

// The flag literals the parser's own `--flag=false` form accepts, so one spelling does not mean
// two things depending on where it is written.
static bool parse_edit_flag(const std::string& field, const std::string& value) {
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") return true;
  if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") return false;
  throw std::invalid_argument("argument --edit: " + field +
                              " must be one of true, 1, yes, on, false, 0, no, off");
}

// One --edit occurrence is one NOTE.FIELD=VALUE assignment: the dot path and the single
// assignment per occurrence are --set's grammar, with the note index as the path's first element.
PolyphonicEdit parse_polyphonic_edit(const std::string& assignment) {
  const auto equals = assignment.find('=');
  if (equals == std::string::npos || equals == 0) {
    throw std::invalid_argument("argument --edit: must be NOTE.FIELD=VALUE");
  }
  const std::string path = assignment.substr(0, equals);
  const std::string value = assignment.substr(equals + 1);
  const auto dot = path.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == path.size()) {
    throw std::invalid_argument("argument --edit: must be NOTE.FIELD=VALUE");
  }
  PolyphonicEdit edit;
  edit.note = parse_int_text("argument --edit: NOTE", path.substr(0, dot));
  edit.field = path.substr(dot + 1);
  const std::string& field = edit.field;
  if (field == "time_offset_samples") {
    // An int is the whole reachable range: the chain refuses audio longer than an int can
    // index, so a wider offset only ever moves a note off the end.
    edit.integer = parse_int_text("argument --edit: time_offset_samples", value);
  } else if (field == "muted") {
    edit.flag = parse_edit_flag(field, value);
  } else if (field == "pitch_shift_semitones" || field == "gain_db" ||
             field == "time_stretch_ratio" || field == "formant_shift_semitones" ||
             field == "vibrato_depth_change" || field == "drift_change") {
    edit.number = parse_float_text("argument --edit: " + field, value);
  } else {
    // The amplitude envelope is the one edit field missing: it is a curve, and nothing on a
    // command line states one.
    throw std::invalid_argument(
        "argument --edit: FIELD must be one of pitch_shift_semitones, gain_db, "
        "time_offset_samples, time_stretch_ratio, formant_shift_semitones, "
        "vibrato_depth_change, drift_change, muted");
  }
  return edit;
}

std::string polyphonic_edit_refusal(const std::vector<std::string>& assignments) {
  try {
    for (const auto& assignment : assignments) parse_polyphonic_edit(assignment);
  } catch (const std::invalid_argument& refusal) {
    return refusal.what();
  }
  return {};
}

std::optional<std::string> find_cli_token(std::string_view value,
                                          const std::vector<std::string>& tokens) {
  const auto fold = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  for (const auto& token : tokens) {
    if (token.size() == value.size() &&
        std::equal(token.begin(), token.end(), value.begin(),
                   [&](char a, char b) { return fold(a) == fold(b); })) {
      return token;
    }
  }
  return std::nullopt;
}

namespace {

// The CLI's mode-name table, in the order a refusal lists it.
struct CliModeName {
  const char* name;
  sonare::Mode mode;
};
constexpr CliModeName kCliModeNames[] = {
    {"major", sonare::Mode::Major},
    {"maj", sonare::Mode::Major},
    {"minor", sonare::Mode::Minor},
    {"min", sonare::Mode::Minor},
    {"m", sonare::Mode::Minor},
    {"dorian", sonare::Mode::Dorian},
    {"phrygian", sonare::Mode::Phrygian},
    {"lydian", sonare::Mode::Lydian},
    {"mixolydian", sonare::Mode::Mixolydian},
    {"locrian", sonare::Mode::Locrian},
};

std::string cli_mode_name_list() {
  std::string list;
  for (const auto& entry : kCliModeNames) {
    if (!list.empty()) list += ", ";
    list += entry.name;
  }
  return list;
}

}  // namespace

sonare::Mode parse_mode_option(const std::string& value) {
  std::string key = value;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& entry : kCliModeNames) {
    if (key == entry.name) return entry.mode;
  }
  throw std::invalid_argument("invalid mode: " + value);
}

std::vector<int> parse_meter_numerators(const std::string& text) {
  std::vector<int> numerators;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const size_t begin = item.find_first_not_of(" \t");
    if (begin == std::string::npos) continue;
    const size_t end = item.find_last_not_of(" \t");
    const std::string trimmed = item.substr(begin, end - begin + 1);
    try {
      size_t consumed = 0;
      const int numerator = std::stoi(trimmed, &consumed);
      if (consumed != trimmed.size()) throw std::invalid_argument(trimmed);
      numerators.push_back(numerator);
    } catch (const std::exception&) {
      throw std::invalid_argument("invalid meter numerator: " + trimmed);
    }
  }
  return numerators;
}

namespace {

// Mirrors Python's int(text, 0), which the Python CLI reads this option with:
// surrounding whitespace, an optional sign, and a 0x / 0o / 0b prefix or plain
// decimal. A single underscore may sit between digits (and after a prefix); a
// decimal with a leading zero is refused unless every digit is zero. A value
// beyond int saturates, so the option's domain refuses it as Python's does.
bool parse_python_int0(const std::string& text, int* out) {
  size_t begin = 0;
  size_t end = text.size();
  const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  bool negative = false;
  if (begin < end && (text[begin] == '+' || text[begin] == '-')) {
    negative = text[begin] == '-';
    ++begin;
  }
  int base = 10;
  bool prefixed = false;
  if (end - begin >= 2 && text[begin] == '0') {
    const char marker =
        static_cast<char>(std::tolower(static_cast<unsigned char>(text[begin + 1])));
    if (marker == 'x') base = 16;
    if (marker == 'o') base = 8;
    if (marker == 'b') base = 2;
    if (base != 10) {
      prefixed = true;
      begin += 2;
    }
  }
  if (begin >= end) return false;
  long long value = 0;
  bool previous_digit = false;
  bool previous_underscore = false;
  bool all_zero = true;
  bool first = true;
  for (size_t i = begin; i < end; ++i) {
    const char c = text[i];
    if (c == '_') {
      // Allowed between digits, and straight after a prefix.
      if (previous_underscore || (first && !prefixed)) return false;
      previous_underscore = true;
      continue;
    }
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (std::isalpha(static_cast<unsigned char>(c))) {
      digit = std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
    } else {
      return false;
    }
    if (digit >= base) return false;
    previous_underscore = false;
    previous_digit = true;
    first = false;
    if (digit != 0) all_zero = false;
    if (value <= std::numeric_limits<int>::max()) value = value * base + digit;
  }
  if (!previous_digit || previous_underscore) return false;
  if (base == 10 && !all_zero && text[begin] == '0') return false;
  if (value > std::numeric_limits<int>::max()) value = std::numeric_limits<int>::max();
  *out = static_cast<int>(negative ? -value : value);
  return true;
}

}  // namespace

int parse_mode_mask_option(const std::string& option, const std::string& value) {
  int parsed = 0;
  if (parse_python_int0(value, &parsed)) return parsed;
  try {
    return static_cast<int>(sonare::scale_mask_for_mode(parse_mode_option(value)));
  } catch (const std::invalid_argument&) {
    throw std::invalid_argument(option_subject(option) + " must be an integer mask or one of " +
                                cli_mode_name_list());
  }
}

double parse_double_strict(const std::string& option, const std::string& value) {
  return parse_double_text(option_subject(option), value);
}

long long parse_int64_strict(const std::string& option, const std::string& value) {
  return parse_int64_text(option_subject(option), value);
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
      case CliOptionOccurrence::Kind::Value: {
        std::string value = occurrence.value;
        // A closed token set is read in any case and stored as the token it names.
        if (spec != nullptr && spec->scalar_type == CliOptionScalarType::String) {
          if (auto token = find_cli_token(value, spec->domain.choices)) value = std::move(*token);
        }
        if (spec != nullptr && spec->repeatable) args.repeated_options[canonical].push_back(value);
        args.options[canonical] = value;
        break;
      }
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

double CliArgs::get_double(const std::string& k, double def) const {
  const auto it = option_value_for(*this, k);
  if (it != options.end()) {
    const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(*this), k);
    return parse_double_strict(spec == nullptr ? k : spec->name, it->second);
  }
  const CliOptionValue value = static_default_for(*this, k);
  return value.kind == CliOptionDefaultKind::Number ? value.number_value : def;
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

long long CliArgs::get_int64(const std::string& k, long long def) const {
  const auto it = option_value_for(*this, k);
  if (it != options.end()) {
    const CliOptionSpec* spec = cli_option_spec_for_command(command_path_for_args(*this), k);
    return parse_int64_strict(spec == nullptr ? k : spec->name, it->second);
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
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        option_subject(name) + " " +
            sonare::util::refusal::integer_range_predicate(
                0.0, static_cast<double>(std::numeric_limits<unsigned>::max())));
  }
  return static_cast<unsigned>(parsed);
}

namespace {

[[noreturn]] void reject_option_range(const std::string& option, int minimum, int maximum) {
  namespace refusal = sonare::util::refusal;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                option_subject(option) + " " +
                                    (maximum == std::numeric_limits<int>::max()
                                         ? refusal::integer_at_least_predicate(minimum)
                                         : refusal::integer_range_predicate(minimum, maximum)));
}

}  // namespace

int CliArgs::get_int_in_range(const std::string& k, int minimum, int maximum, int def) const {
  if (!has(k)) {
    const int fallback = get_int(k, def);
    // A registry default outside the caller's range is a registry bug, not user
    // input; clamping would hide it, so report it the same way.
    if (fallback < minimum || fallback > maximum) {
      reject_option_range(k, minimum, maximum);
    }
    return fallback;
  }
  const int value = get_int(k, def);
  if (value < minimum || value > maximum) {
    reject_option_range(k, minimum, maximum);
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

  switch (spec->scalar_type) {
    case CliOptionScalarType::Integer: {
      const int parsed = parse_int_strict(spec->name, value);
      if (spec->name == "n-fft") {
        args.n_fft = parsed;
      } else if (spec->name == "hop-length") {
        args.hop_length = parsed;
      } else if (spec->name == "n-mels") {
        args.n_mels = parsed;
      }
      break;
    }
    case CliOptionScalarType::Number:
      if (spec->name == "fmin-hz") args.fmin = parse_float_strict(spec->name, value);
      if (spec->name == "fmax-hz") args.fmax = parse_float_strict(spec->name, value);
      break;
    case CliOptionScalarType::Path:
      args.output_file = value;
      break;
    case CliOptionScalarType::Boolean:
    case CliOptionScalarType::String:
      break;
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
