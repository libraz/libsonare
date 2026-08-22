#include "util/tunable.h"

// Everything below the parser is compiled only into a tuning build. This
// translation unit is linked into every target including the WebAssembly
// module, where <fstream>/<sstream>/<unordered_map> would pull real weight into
// a binary under a size gate — for code that a shipped build never reaches,
// since `SONARE_TUNABLE` expands to a `constexpr float` there and nothing calls
// `tunable_value` at all.
#if defined(SONARE_TUNING) && SONARE_TUNING

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sonare {
namespace tuning {

namespace {

/// Parse `name=value` pairs from `text`, separated by commas or newlines.
/// Whitespace around either side is trimmed; blank entries and `#` comments are
/// skipped. An unparseable value is dropped rather than aborting — the caller
/// then keeps its compiled-in default, which is the safe direction for a
/// development-only path.
void parse_into(const std::string& text, std::unordered_map<std::string, float>& out) {
  auto trim = [](const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  };
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t next = text.find_first_of(",\n", pos);
    const std::string entry =
        trim(text.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
    pos = (next == std::string::npos) ? text.size() + 1 : next + 1;
    if (entry.empty() || entry[0] == '#') continue;
    const size_t eq = entry.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(entry.substr(0, eq));
    const std::string val = trim(entry.substr(eq + 1));
    if (key.empty() || val.empty()) continue;
    // strtof rather than stof: this TU is linked into the WebAssembly module,
    // where a `catch` is elided at compile time unless the file opts in, so an
    // unparseable value must be a return code and not an exception.
    char* end = nullptr;
    const float parsed = std::strtof(val.c_str(), &end);
    if (end != nullptr && *end == '\0') out[key] = parsed;
  }
}

/// The override table, built on first use.
///
/// A function-local static rather than a namespace-scope one because every
/// `SONARE_TUNABLE` in a tuning build runs during static initialisation, and a
/// namespace-scope table in this translation unit would not be guaranteed to be
/// constructed before another TU's constants read it.
const std::unordered_map<std::string, float>& overrides() {
  static const std::unordered_map<std::string, float> table = [] {
    std::unordered_map<std::string, float> t;
    const char* spec = std::getenv("SONARE_TUNING_OVERRIDES");
    if (spec == nullptr || *spec == '\0') return t;
    if (spec[0] == '@') {
      std::ifstream in(spec + 1);
      if (in) {
        std::ostringstream buf;
        buf << in.rdbuf();
        parse_into(buf.str(), t);
      }
      return t;
    }
    parse_into(std::string(spec), t);
    return t;
  }();
  return table;
}

/// Every key any `SONARE_TUNABLE` or override layer has asked for, with the
/// value it would have used had no override been set.
///
/// The point is discovery: the knob space is a few hundred calibration
/// constants plus a patch field table, spread over fifteen engine files and a
/// program table, and a fitter that had to be told the names by hand would go
/// stale the first time one moved. Instead the library reports what it
/// actually consulted, so `autofit.py --spec auto` builds its knob list from
/// the run rather than from a parse of the source.
///
/// Written on exit when `SONARE_TUNING_DUMP` names a path, as
/// `key<TAB>default` lines sorted by key. Only the keys reached by the render
/// appear, which is the useful set: a knob belonging to an engine the program
/// does not use cannot affect it.
///
/// Two kinds of header line precede the knobs: `#program<TAB>NNN<TAB>key` maps
/// a GM program to the patch that voices it, and `#bound<TAB>path<TAB>lo<TAB>hi`
/// gives a patch field's admissible range. A bound is keyed by the path alone
/// because it is a property of the field, not of the patch — every patch with a
/// `bowed_string.bow_force` accepts the same interval.
class Recorder {
 public:
  ~Recorder() {
    const char* path = std::getenv("SONARE_TUNING_DUMP");
    if (path == nullptr || *path == '\0') return;
    if (seen_.empty() && programs_.empty()) return;
    std::ofstream out(path);
    if (!out) return;
    std::vector<std::pair<int, std::string>> progs(programs_.begin(), programs_.end());
    std::sort(progs.begin(), progs.end());
    for (const auto& row : progs) out << "#program\t" << row.first << '\t' << row.second << '\n';
    std::vector<std::pair<std::string, std::pair<float, float>>> bounds(bounds_.begin(),
                                                                        bounds_.end());
    std::sort(bounds.begin(), bounds.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& row : bounds) {
      out << "#bound\t" << row.first << '\t' << row.second.first << '\t' << row.second.second
          << '\n';
    }
    std::vector<std::pair<std::string, float>> rows(seen_.begin(), seen_.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& row : rows) out << row.first << '\t' << row.second << '\n';
  }

  void note(const std::string& key, float default_value) { seen_.emplace(key, default_value); }

  void note_program(int program, const char* key) { programs_[program] = key; }

  void note_bound(const std::string& path, float lo, float hi) {
    bounds_.emplace(path, std::make_pair(lo, hi));
  }

  static Recorder& instance() {
    static Recorder r;
    return r;
  }

 private:
  std::unordered_map<std::string, float> seen_;
  std::unordered_map<std::string, std::pair<float, float>> bounds_;
  std::map<int, std::string> programs_;
};

/// True when a dump was requested; checked once so the recording path costs
/// nothing on a normal tuning run.
bool recording() {
  static const bool on = [] {
    const char* path = std::getenv("SONARE_TUNING_DUMP");
    return path != nullptr && *path != '\0';
  }();
  return on;
}

/// The stem of a `__FILE__` path: no directory, no extension. The compiler may
/// hand us an absolute or a relative path depending on how the tree was
/// configured, so both are reduced to the same scope.
std::string file_scope(const char* file) {
  std::string s(file);
  const size_t slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s.erase(0, slash + 1);
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos && dot != 0) s.erase(dot);
  return s;
}

}  // namespace

float tunable_keyed(const char* key, float default_value) {
  const std::string k(key);
  if (recording()) Recorder::instance().note(k, default_value);
  const auto& table = overrides();
  const auto it = table.find(k);
  return it == table.end() ? default_value : it->second;
}

float tunable_value(const char* file, const char* name, float default_value) {
  const auto& table = overrides();
  if (table.empty() && !recording()) return default_value;
  const std::string key = file_scope(file) + "." + name;
  if (recording()) Recorder::instance().note(key, default_value);
  const auto it = table.find(key);
  return it == table.end() ? default_value : it->second;
}

void note_program_key(int program, const char* key) {
  if (recording() && key != nullptr) Recorder::instance().note_program(program, key);
}

void note_bound(const char* path, float lo, float hi) {
  if (recording() && path != nullptr) Recorder::instance().note_bound(std::string(path), lo, hi);
}

bool tuning_enabled() { return true; }

}  // namespace tuning
}  // namespace sonare

#else  // !SONARE_TUNING

namespace sonare {
namespace tuning {

/// Unreachable in a normal build (`SONARE_TUNABLE` is a `constexpr float`
/// there), but defined so the declarations in the header always have symbols.
float tunable_value(const char*, const char*, float default_value) { return default_value; }

float tunable_keyed(const char*, float default_value) { return default_value; }

void note_program_key(int, const char*) {}

void note_bound(const char*, float, float) {}

bool tuning_enabled() { return false; }

}  // namespace tuning
}  // namespace sonare

#endif  // SONARE_TUNING
