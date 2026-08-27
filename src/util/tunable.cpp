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
/// The point is discovery: a fitter told the knob names by hand goes stale the
/// first time one moves, so the library reports what it actually consulted and
/// `autofit.py --spec auto` builds its list from the run. Written on exit when
/// `SONARE_TUNING_DUMP` names a path, as `key<TAB>default` lines sorted by key.
///
/// The two streams feeding it have different scopes. Patch fields are recorded
/// as the GM fallback tables are built, so those keys are the tables the run
/// reached. Engine constants are not: `SONARE_TUNABLE` declares a
/// namespace-scope `const float` initialised before `main`, so every linked TU's
/// constants appear whether the render used that engine or not. Narrow by the
/// declaring file's stem to get one engine's, as `autofit.py` does.
///
/// Three header line kinds precede the knobs: `#program<TAB>NNN<TAB>key` maps a
/// GM program to its patch, `#mode<TAB>key<TAB>engine` names the engine voicing
/// that patch, and `#bound<TAB>path<TAB>lo<TAB>hi` gives a field's admissible
/// range. A bound is keyed by path alone, being a property of the field rather
/// than the patch; a mode is keyed by patch, being a property of the patch
/// rather than of any one program it answers.
class Recorder {
 public:
  ~Recorder() {
    const char* path = std::getenv("SONARE_TUNING_DUMP");
    if (path == nullptr || *path == '\0') return;
    if (seen_.empty() && programs_.empty()) return;
    std::ofstream out(path);
    if (!out) return;
    // `#program <program> <bank> <patch>`: the bank is the GS variation
    // number, and a variation that falls back to the capital tone is not
    // written, so what appears is exactly the set a fitter can address.
    for (const auto& row : programs_) {
      out << "#program\t" << row.first.first << '\t' << row.first.second << '\t' << row.second
          << '\n';
    }
    // `#mode <patch> <engine>`: keyed by patch, since one patch voices several
    // programs and the engine belongs to the patch.
    for (const auto& row : modes_) {
      out << "#mode\t" << row.first << '\t' << row.second << '\n';
    }
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

  void note_program(int program, int bank, const char* key) { programs_[{program, bank}] = key; }

  void note_patch_mode(const char* key, const char* mode) { modes_[key] = mode; }

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
  std::map<std::pair<int, int>, std::string> programs_;
  std::map<std::string, std::string> modes_;
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

void note_program_key(int program, int bank, const char* key) {
  if (recording() && key != nullptr) Recorder::instance().note_program(program, bank, key);
}

void note_patch_mode(const char* key, const char* mode) {
  if (recording() && key != nullptr && mode != nullptr) {
    Recorder::instance().note_patch_mode(key, mode);
  }
}

void note_bound(const char* path, float lo, float hi) {
  if (recording() && path != nullptr) Recorder::instance().note_bound(std::string(path), lo, hi);
}

}  // namespace tuning
}  // namespace sonare

#else  // !SONARE_TUNING

namespace sonare {
namespace tuning {

/// Unreachable in a normal build (`SONARE_TUNABLE` is a `constexpr float`
/// there), but defined so the declarations in the header always have symbols.
float tunable_value(const char*, const char*, float default_value) { return default_value; }

float tunable_keyed(const char*, float default_value) { return default_value; }

void note_program_key(int, int, const char*) {}

void note_patch_mode(const char*, const char*) {}

void note_bound(const char*, float, float) {}

}  // namespace tuning
}  // namespace sonare

#endif  // SONARE_TUNING
