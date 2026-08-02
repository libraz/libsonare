#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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
  // Global DSP options are parsed into dedicated fields above, but their
  // spelling must still be validated against the selected command.
  std::vector<std::string> global_options;
  std::vector<std::string> positionals;
  std::vector<std::string> missing_value_options;

  float get_float(const std::string& k, float def) const;
  int get_int(const std::string& k, int def) const;
  bool has(const std::string& k) const;
  std::string get_string(const std::string& k, const std::string& def = "") const;
};

/// Validates command-specific option names, required option values, and
/// positional arity. Returns an empty string when the invocation is valid.
std::string validate_cli_arguments(const CliArgs& args, bool requires_audio);
std::vector<std::string> cli_options_for_command(const std::string& command);

class ArgParser {
 public:
  static CliArgs parse(int argc, char* argv[]);

 private:
  static bool try_parse_global_option(CliArgs& args, const std::string& arg, char* argv[], int& i,
                                      int argc);
  static void parse_option(CliArgs& args, const std::string& key, char* argv[], int& i, int argc,
                           const std::string* inline_value = nullptr);
};

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
