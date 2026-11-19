#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sonare_cli_registry.h"
#include "util/refusal_predicates.h"

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

// The domain in the library's predicate vocabulary (util/refusal_predicates.h), so an
// option and the library argument it feeds refuse in the same words after the subject.
// An integer domain's exclusive end is the next integer in, as the library states it.
std::string domain_predicate(const CliOptionSpec& spec) {
  namespace refusal = sonare::util::refusal;
  const CliOptionDomain& domain = spec.domain;
  if (!domain.choices.empty()) {
    std::string text = "must be one of ";
    for (size_t index = 0; index < domain.choices.size(); ++index) {
      if (index > 0) text += ", ";
      text += domain.choices[index];
    }
    return text;
  }
  if (spec.scalar_type == CliOptionScalarType::Integer && domain.has_minimum) {
    const double low = domain.exclusive_minimum ? domain.minimum + 1.0 : domain.minimum;
    if (!domain.has_maximum)
      return refusal::integer_at_least_predicate(static_cast<long long>(low));
    const double high = domain.exclusive_maximum ? domain.maximum - 1.0 : domain.maximum;
    return refusal::integer_range_predicate(low, high);
  }
  if (domain.has_minimum && domain.has_maximum) {
    return refusal::interval_predicate(domain.minimum, !domain.exclusive_minimum, domain.maximum,
                                       !domain.exclusive_maximum);
  }
  if (domain.has_minimum) {
    return domain.exclusive_minimum ? refusal::greater_than_predicate(domain.minimum)
                                    : refusal::at_least_predicate(domain.minimum);
  }
  return domain.exclusive_maximum ? refusal::less_than_predicate(domain.maximum)
                                  : refusal::at_most_predicate(domain.maximum);
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
    return option_subject(spec.name) + " " + domain_predicate(spec);
  }
  double parsed = 0.0;
  try {
    parsed = parse_double_strict(spec.name, value);
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
  return option_subject(spec.name) + " " + domain_predicate(spec);
}

std::string validate_numeric_option_values(const CliArgs& args) {
  const CliCommandSpec* command = cli_command_spec_for_path(command_path_for_args(args));
  for (const auto& [key, value] : args.options) {
    const CliOptionSpec* spec = option_for_spec(command, key);
    if (spec == nullptr) continue;
    try {
      if (spec->scalar_type == CliOptionScalarType::Integer) {
        if (key == "candidates" && value == "true") continue;
        if (spec->accepts_mode_name) {
          (void)parse_mode_mask_option(key, value);
          continue;
        }
        // An option whose declared domain reaches past int is parsed at 64 bits,
        // so the domain below is what refuses the value rather than the parser's
        // width silently doing it first with a message that names no range.
        if (spec->int64_width ||
            (spec->domain.has_maximum &&
             spec->domain.maximum > static_cast<double>(std::numeric_limits<int>::max()))) {
          (void)parse_int64_strict(key, value);
        } else {
          (void)parse_int_strict(key, value);
        }
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
    metadata.stated_default = cli_option_stated_default(option);
    metadata.repeatable = option.repeatable;
    metadata.arity = option.arity;
    metadata.scalar_type = option.scalar_type;
    metadata.required = option.required;
    metadata.global_lexical = option.global_lexical;
    metadata.inventory = option.inventory;
    metadata.has_implicit_optional_default =
        option.implicit_optional_default.kind != CliOptionDefaultKind::Null;
    metadata.implicit_optional_default = option.implicit_optional_default;
    metadata.domain = option.domain;
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
    const std::string stated = cli_option_stated_default(option);
    if (!stated.empty()) display += "  (default: " + stated + ")";
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
    return {"Command '" + args.command + "' does not produce a file output; remove -o/--output"};
  }
  for (const auto& key : args.global_options) {
    if (!accepts_option(key))
      return {"Unknown option '--" + key + "' for command '" + args.command + "'"};
  }
  for (const auto& option : args.options) {
    const std::string& key = option.first;
    if (!accepts_option(key)) {
      return {"Unknown option '" + (key.rfind("-", 0) == 0 ? key : "--" + key) + "' for command '" +
              args.command + "'"};
    }
  }
  if (!args.missing_value_options.empty())
    return {"Missing value for option '" + args.missing_value_options.front() + "'"};
  if (const std::string numeric_error = validate_numeric_option_values(args);
      !numeric_error.empty())
    return {numeric_error};
  if (command != nullptr) {
    // Presence and domain, for every option of the selected leaf, from the
    // registry alone. A handler adds nothing to this: the same (command,
    // option, value) triple therefore has exactly one verdict and one exit
    // class, whichever handler ends up consuming it.
    for (const auto& option : command->options) {
      if (option.required && !args.has(option.name)) {
        return {"Missing required option '--" + option.name + "'"};
      }
      if (option.domain.empty() || !args.has(option.name)) continue;
      std::string value = args.get_string(option.name);
      if (option.accepts_mode_name) {
        value = std::to_string(parse_mode_mask_option(option.name, value));
      }
      if (const std::string error = domain_error_for(option, value); !error.empty()) {
        return {error};
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
            "' for command '" + args.command + "'"};
  }
  if (command != nullptr && !command->positional_name.empty() &&
      args.positionals.size() < command->positional_count) {
    return {"Missing required positional argument <" + command->positional_name +
            "> for command '" + args.command + "'"};
  }
  return {};
}
