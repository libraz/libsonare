#include "cli_support.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "core/channel_layout.h"
#include "util/exception.h"

#ifdef SONARE_WITH_MASTERING
#include "mastering/assistant/platform_targets.h"
#endif

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

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

JsonBuilder::JsonBuilder() {
  // A std::ostringstream formats through the global C++ locale, not through
  // LC_NUMERIC, so a host that has run std::locale::global with a de_DE-style
  // locale makes every `--json` command emit `"lufs": -14,5` -- syntactically
  // invalid JSON that no parser accepts, from a CLI that reports success. The
  // matching policy for the core serializer is in util/json.h.
  ss_.imbue(std::locale::classic());
}

JsonBuilder& JsonBuilder::begin_object() {
  append_separator();
  ss_ << "{";
  needs_comma_.push_back(false);
  return *this;
}

JsonBuilder& JsonBuilder::end_object() {
  ss_ << "}";
  needs_comma_.pop_back();
  if (!needs_comma_.empty()) needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::begin_array() {
  append_separator();
  ss_ << "[";
  needs_comma_.push_back(false);
  return *this;
}

JsonBuilder& JsonBuilder::end_array() {
  ss_ << "]";
  needs_comma_.pop_back();
  if (!needs_comma_.empty()) needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::key(const std::string& k) {
  append_separator();
  ss_ << "\"" << escape(k) << "\": ";
  needs_comma_.back() = false;
  return *this;
}

JsonBuilder& JsonBuilder::value(const std::string& v) {
  append_separator();
  ss_ << "\"" << escape(v) << "\"";
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(const char* v) { return value(std::string(v)); }

JsonBuilder& JsonBuilder::value(int v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(size_t v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(float v) {
  append_separator();
  // RFC 8259 forbids NaN/Infinity as JSON numbers; emit `null` (as util/json.h
  // does) so a non-finite reading -- e.g. a -inf LUFS/true-peak for a fully
  // silent input -- yields valid JSON that json.loads/jq/JSON.parse can read.
  if (std::isfinite(v)) {
    ss_ << std::setprecision(std::numeric_limits<float>::max_digits10) << v;
  } else {
    ss_ << "null";
  }
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(double v) {
  append_separator();
  // See value(float): non-finite numbers serialize as JSON null, not "nan"/"inf".
  if (std::isfinite(v)) {
    ss_ << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
  } else {
    ss_ << "null";
  }
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(bool v) {
  append_separator();
  ss_ << (v ? "true" : "false");
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::null_value() {
  append_separator();
  ss_ << "null";
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::kv(const std::string& k, const std::string& v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, const char* v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, int v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, size_t v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, float v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, double v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, bool v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::float_array(const std::vector<float>& arr) {
  begin_array();
  for (float v : arr) value(v);
  end_array();
  return *this;
}

std::string JsonBuilder::build() const { return ss_.str(); }

void JsonBuilder::print() const { std::cout << ss_.str() << "\n"; }

void JsonBuilder::append_separator() {
  if (!needs_comma_.empty() && needs_comma_.back()) {
    ss_ << ", ";
  }
}

std::string JsonBuilder::escape(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (c < 0x20u) {
          constexpr char kHex[] = "0123456789abcdef";
          result += "\\u00";
          result += kHex[(c >> 4u) & 0x0Fu];
          result += kHex[c & 0x0Fu];
        } else {
          result += static_cast<char>(c);
        }
    }
  }
  return result;
}

namespace {

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

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Immutable native CLI registry
// ---------------------------------------------------------------------------

CliOptionValue null_default() { return {}; }

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
  if (channels == mono || channels == stereo) return {};
  return {"invalid value for --channels: " + std::to_string(channels) + " (expected one of " +
              std::to_string(mono) + ", " + std::to_string(stereo) + ")",
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

void add_command(std::vector<CliCommandSpec>& registry, const char* path, bool requires_audio,
                 std::vector<CliOptionSpec> options, std::vector<std::string> aliases = {},
                 CliCommandValidator validate = nullptr) {
  registry.push_back(
      {path, std::move(aliases), with_json(std::move(options)), requires_audio, true, validate});
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
                 string_value("genre-hint")});
    add_command(commands, "chords", true,
                {global_int("n-fft", 2048), global_int("hop-length", 512),
                 number_value("min-duration", 0.3), number_value("smoothing-window", 2.0),
                 number_value("threshold", 0.5), flag("triads-only"), flag("nnls"),
                 flag("no-beat-sync"), flag("use-hmm"), int_value("hmm-beam-width", 24),
                 flag("key-context"), string_value("key-root", "C"),
                 string_value("key-mode", "major"), flag("detect-inversions")});
    add_command(commands, "sections", true,
                {number_value("min-duration", 4.0), number_value("threshold", 0.3),
                 global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "dynamics", true,
                {number_value("window-sec", 0.4), global_int("hop-length", 512)});
    add_command(
        commands, "rhythm", true,
        {number_value("start-bpm", 120.0), number_value("bpm-min", 60.0),
         number_value("bpm-max", 200.0), global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "melody", true,
                {number_value("threshold", 0.1), global_int("hop-length", 512),
                 global_number("fmin", 80.0), global_number("fmax", 1000.0)});
    add_command(commands, "boundaries", true,
                {number_value("threshold", 0.3), int_value("kernel-size", 64),
                 number_value("min-distance", 2.0), global_int("n-fft", 2048),
                 global_int("hop-length", 512)});

    // Processing leaves.
    add_command(commands, "pitch-shift", true,
                {number_value("semitones"), required_output(), global_int("n-fft", 2048),
                 global_int("hop-length", 512)});
    add_command(commands, "time-stretch", true,
                {number_value("rate"), required_output(), global_int("n-fft", 2048),
                 global_int("hop-length", 512)});
    add_command(
        commands, "pitch-correct", true,
        {number_value("current-midi", 69.0), number_value("target-midi", 69.0), required_output()});
    add_command(commands, "note-stretch", true,
                {int_value("onset", 0), int_value("offset", 0), number_value("ratio", 1.0),
                 required_output()});
    add_command(commands, "voice-change", true,
                {string_value("preset", ""), path_value("preset-json"), path_value("preset-pack"),
                 string_value("set", "", false, true), number_value("pitch-semitones"),
                 number_value("formant-factor"), required_output()});
    add_command(commands, "voice-presets", false, {});
    add_command(commands, "voice-preset", false, {string_value("preset", "neutral-monitor")});
    add_command(
        commands, "voice-preset-validate", false,
        {path_value("preset-json"), string_value("preset"), string_value("set", "", false, true)});
    add_command(
        commands, "hpss", true,
        {int_value("kernel-harmonic", 31), int_value("kernel-percussive", 31), required_output(),
         flag("harmonic-only"), flag("percussive-only"), flag("with-residual"), flag("hard-mask"),
         global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "preemphasis", true, {number_value("coef", 0.97), required_output()});
    add_command(commands, "deemphasis", true, {number_value("coef", 0.97), required_output()});
    add_command(commands, "trim-silence", true,
                {number_value("threshold-db"), number_value("top-db"), output_value(),
                 global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(
        commands, "split-silence", true,
        {number_value("top-db", 60.0), global_int("n-fft", 2048), global_int("hop-length", 512)});
    add_command(commands, "normalize", true,
                {string_value("mode", "peak"), number_value("target-db"), required_output()});
    add_command(commands, "gain", true, {number_value("gain-db"), required_output()});
    add_command(commands, "fade", true,
                {number_value("fade-in"), number_value("fade-out"), required_output()});
    add_command(commands, "filter", true,
                {string_value("type"), int_value("order", 2), number_value("cutoff", 0.0),
                 number_value("center", 0.0), number_value("bandwidth", 0.0), required_output(),
                 flag("zero-phase")});
    add_command(commands, "resample", true,
                {required_int("target-rate", {"target-sr"}), required_output()});
    add_command(commands, "tone", false,
                {number_value("frequency"), int_value("sr", 22050), number_value("duration", 1.0),
                 number_value("phase", 0.0), number_value("amplitude", 1.0), required_output()});
    add_command(commands, "chirp", false,
                {int_value("sr", 22050), number_value("duration", 1.0), required_output(),
                 flag("exponential"), global_number("fmin", 0.0), global_number("fmax", 0.0)});
    add_command(commands, "clicks", false,
                {string_value("times"), int_value("sr", 22050), int_value("length", 0),
                 number_value("frequency", 1000.0), number_value("click-duration", 0.1),
                 required_output()});

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
                 number_value("speech-mono-amount", 1.0)});
    add_command(commands, "mastering-processor", true,
                {required_string("processor"), string_value("params"), bits_value(), output_value(),
                 flag("stereo")});
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
    add_command(commands, "mastering-processors", false, {});
    add_command(commands, "mastering-pair-processors", false, {});
    add_command(commands, "mastering-pair-analyses", false, {});
    add_command(commands, "mastering-stereo-analyses", false, {});
#endif
#ifdef SONARE_WITH_MIXING
    // `mix` is the deprecated spelling of `mix-strip` and resolves through the
    // alias path, which is why there is one row rather than two. Two rows that
    // each named the other as an alias never used that path -- path lookup wins
    // -- so each name was validated against its own copy of the option list, and
    // an option added to one became an unknown option under the other.
    add_command(commands, "mix-strip", true,
                {number_value("input-trim-db", 0.0), number_value("fader-db", 0.0),
                 number_value("pan", 0.0), string_value("pan-mode", "balance"),
                 number_value("width", 1.0), output_value()},
                {"mix"});
    add_command(commands, "mixing-presets", false, {});
    // The advertised default has to be one the command can actually run: an
    // empty string reaches the preset lookup and fails, and the handler's own
    // fallback never applied because the registry default wins over it.
    add_command(commands, "mixing-preset", false, {string_value("preset", "vocalReverbSend")});
#endif

    // Feature leaves.
    add_command(
        commands, "mel", true,
        {global_int("n-fft", 2048), global_int("hop-length", 512), global_int("n-mels", 128),
         global_number("fmin", 0.0), global_number("fmax", 0.0), flag("htk")});
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
    add_command(commands, "cqt", true,
                {int_value("n-bins", 84), int_value("bins-per-octave", 12),
                 global_int("hop-length", 512), global_number("fmin", 0.0)});
    add_command(commands, "vqt", true,
                {int_value("n-bins", 84), int_value("bins-per-octave", 12),
                 number_value("gamma", 0.0), number_value("filter-scale", 1.0),
                 global_int("hop-length", 512), global_number("fmin", 0.0)});
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
    add_command(commands, "synthesize-rir", false,
                {number_value("length", 7.0), number_value("width", 5.0),
                 number_value("height", 3.0), number_value("absorption", 0.2),
                 number_value("source-x", 1.0), number_value("source-y", 1.0),
                 number_value("source-z", 1.2), number_value("listener-x", 5.0),
                 number_value("listener-y", 4.0), number_value("listener-z", 1.7),
                 int_value("sample-rate", 48000), int_value("ism-order", 3), int_value("seed", 1),
                 number_value("max-seconds", 0.0), required_output(), flag("sabine")});
    add_command(
        commands, "room-morph", true,
        {number_value("length", 7.0), number_value("width", 5.0), number_value("height", 3.0),
         number_value("absorption", 0.2), number_value("source-x", 1.0),
         number_value("source-y", 1.0), number_value("source-z", 1.2),
         number_value("listener-x", 5.0), number_value("listener-y", 4.0),
         number_value("listener-z", 1.7), number_value("suppression", 0.5),
         number_value("wet", 0.5), int_value("ism-order", 3), int_value("seed", 1),
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
    // option row.
    add_command(commands, "project.abi", false, {});
    add_command(commands, "project.synth-presets", false, {});
    add_command(commands, "project.new", false, {int_value("sample-rate", 0), required_output()});
    add_command(commands, "project.validate", false,
                {flag("strict"), required_path("in"), output_value()});
    add_command(commands, "project.compile", false, {required_path("in")});
    add_command(commands, "project.bounce", false,
                {required_path("in"), required_output(), int_value("sample-rate"),
                 int_value("frames", 0), int_value("block-size", 0), int_value("channels", 2),
                 int_value("instrument-latency", 0), optional_string("synth")},
                {}, &validate_project_bounce_channels);
    add_command(commands, "project.export-smf", false, {required_path("in"), required_output()});
    add_command(commands, "project.import-smf", false, {required_path("smf"), required_output()});
    add_command(commands, "project.export-midi2", false, {required_path("in"), required_output()});
    add_command(commands, "project.import-midi2", false,
                {required_path("midi2"), required_output()});
#endif
    return commands;
  }();
  return registry;
}

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

std::string command_path_for_args(const CliArgs& args) {
  if (args.command == "project" && !args.input_file.empty()) return "project." + args.input_file;
  return args.command;
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

}  // namespace

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

namespace {

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

  size_t max_positionals = requires_audio ? 1u : 0u;
  if (args.command == "project" || args.command == "voice-preset-validate") max_positionals = 1u;
  if (args.positionals.size() > max_positionals) {
    return {"Unexpected positional argument '" + args.positionals[max_positionals] +
                "' for command '" + args.command + "'",
            false};
  }
  return {};
}

Stats Stats::compute(const std::vector<float>& v) {
  Stats s{};
  if (v.empty()) return s;

  s.min = *std::min_element(v.begin(), v.end());
  s.max = *std::max_element(v.begin(), v.end());
  s.mean = std::accumulate(v.begin(), v.end(), 0.0f) / static_cast<float>(v.size());

  float var = 0.0f;
  for (float x : v) var += (x - s.mean) * (x - s.mean);
  s.std = std::sqrt(var / static_cast<float>(v.size()));

  return s;
}

namespace color {
const char* reset = "";
const char* bold = "";
const char* cyan = "";
const char* green = "";
const char* magenta = "";
const char* yellow = "";
const char* blue = "";
const char* red = "";

void configure() {
#if defined(_WIN32)
  const bool interactive = _isatty(_fileno(stdout)) && _isatty(_fileno(stderr));
#else
  const bool interactive = isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);
#endif
  const bool enabled = std::getenv("NO_COLOR") == nullptr && interactive;
  reset = enabled ? "\033[0m" : "";
  bold = enabled ? "\033[1m" : "";
  cyan = enabled ? "\033[36m" : "";
  green = enabled ? "\033[32m" : "";
  magenta = enabled ? "\033[35m" : "";
  yellow = enabled ? "\033[33m" : "";
  blue = enabled ? "\033[34m" : "";
  red = enabled ? "\033[31m" : "";
}
}  // namespace color

namespace system_info {

int logical_cores() {
  int n = static_cast<int>(std::thread::hardware_concurrency());
  return n > 0 ? n : 1;
}

int physical_cores() {
#ifdef __APPLE__
  int cores = 0;
  size_t len = sizeof(cores);
  if (sysctlbyname("hw.physicalcpu", &cores, &len, nullptr, 0) == 0 && cores > 0) {
    return cores;
  }
#elif __linux__
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  std::vector<int> core_ids;
  while (std::getline(f, line)) {
    if (line.find("core id") == 0) {
      auto pos = line.find(':');
      if (pos != std::string::npos) {
        int id = std::stoi(line.substr(pos + 1));
        if (std::find(core_ids.begin(), core_ids.end(), id) == core_ids.end()) {
          core_ids.push_back(id);
        }
      }
    }
  }
  if (!core_ids.empty()) return static_cast<int>(core_ids.size());
#endif
  return logical_cores();
}

size_t total_memory_bytes() {
#ifdef __APPLE__
  int64_t mem = 0;
  size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) {
    return static_cast<size_t>(mem);
  }
#elif __linux__
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("MemTotal:") == 0) {
      size_t kb = std::stoull(line.substr(line.find(':') + 1));
      return kb * 1024;
    }
  }
#endif
  return 0;
}

size_t available_memory_bytes() {
#ifdef __APPLE__
  mach_port_t host = mach_host_self();
  vm_statistics64_data_t stats;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) ==
      KERN_SUCCESS) {
    return (stats.free_count + stats.inactive_count) * vm_page_size;
  }
#elif __linux__
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("MemAvailable:") == 0) {
      size_t kb = std::stoull(line.substr(line.find(':') + 1));
      return kb * 1024;
    }
  }
#endif
  return 0;
}

std::string parallel_strategy() {
  int cores = logical_cores();
  if (cores >= 8) return "aggressive_parallel";
  if (cores >= 4) return "balanced_parallel";
  if (cores >= 2) return "conservative_parallel";
  return "sequential_only";
}

int parallel_workers() {
  int cores = logical_cores();
  if (cores >= 8) return cores - 2;
  if (cores >= 4) return std::min(cores, 8);
  if (cores >= 2) return std::min(cores, 3);
  return 1;
}

bool parallel_enabled() { return logical_cores() >= 2; }

}  // namespace system_info

StageInfo get_stage_info(const char* stage) {
  static const std::map<std::string, StageInfo> stages = {
      {"features", {1, 9, "Computing features"}}, {"bpm", {2, 9, "Detecting BPM"}},
      {"key", {3, 9, "Detecting key"}},           {"beats", {4, 9, "Detecting beats"}},
      {"chords", {5, 9, "Analyzing chords"}},     {"sections", {6, 9, "Analyzing sections"}},
      {"timbre", {7, 9, "Analyzing timbre"}},     {"dynamics", {8, 9, "Analyzing dynamics"}},
      {"rhythm", {9, 9, "Analyzing rhythm"}},     {"complete", {9, 9, "Complete"}},
  };
  auto it = stages.find(stage);
  if (it != stages.end()) {
    return it->second;
  }
  return {0, 0, stage};
}

void progress_callback(float progress, const char* stage) {
  StageInfo info = get_stage_info(stage);
  int pct = static_cast<int>(progress * 100.0f);

  constexpr int bar_len = 30;
  int filled = static_cast<int>(progress * bar_len);
  std::string bar(filled, '#');
  bar += std::string(bar_len - filled, '-');

  if (info.number > 0) {
    fprintf(stderr, "\r%s[%s] %3d%% [%d/%d] %s...%s                ", color::blue, bar.c_str(), pct,
            info.number, info.total, info.description, color::reset);
  } else {
    fprintf(stderr, "\r%s[%s] %3d%% %s...%s          ", color::blue, bar.c_str(), pct, stage,
            color::reset);
  }
  fflush(stderr);
}

void clear_progress() {
  std::cerr << "\r                                                              \r" << std::flush;
}

std::string describe_level(float value, const char* low, const char* mid, const char* high) {
  if (value < 0.33f) return low;
  if (value < 0.67f) return mid;
  return high;
}

std::string basename(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}
