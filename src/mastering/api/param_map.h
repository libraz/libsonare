#pragma once

/// @file param_map.h
/// @brief Internal: the flat parameter map, its typed accessors and the declarations a config
///        builder records while it reads (kind, default, bound, dependency).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mastering/api/named_processor.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/param_meta.h"
#include "mastering/common/parameter_domain.h"
#include "util/exception.h"
#include "util/insertion_sort.h"

namespace sonare::mastering::api::detail {

/// @brief How a config builder reads a flat mastering parameter.
/// @details Derived from the accessor and the destination field's declared
///          type, never from the key's spelling: `b()` and a `bool` config
///          field mean @c Boolean, `i()` and an integral/enum field mean
///          @c Integer, every other accessor means @c Number. @c String and
///          @c Array mark a key read from the JSON side-channel rather than
///          from the flat map (@ref note_side_channel_key).
///
/// @c Integer reports as `"number"` (or `"enum"` for a named enum), because the
/// flat param surface carries every value as a `double` and a host may send
/// `2.0` for an integral field. The distinction is kept internally because it
/// is what lets a measured bound be reported as the integer it actually is
/// instead of as the midpoint between two integers.
enum class ParamKind : std::uint8_t { Boolean, Integer, Number, String, Array };

/// @brief The fallback a config builder used for a key it probed.
/// @details A builder run against an empty map falls back to the config field's
///          own initializer for every key it reads, so probing records the
///          design default for free. @c ambiguous marks a key two builders
///          probed with different fallbacks; the catalog then publishes no
///          default rather than picking one of two answers.
struct ParamDefault {
  double value = 0.0;
  bool ambiguous = false;
};

/// @brief A sibling key whose live value bounds another key's.
struct ParamDependency {
  std::string key;
  Relation relation = Relation::Le;
  /// Multiplier on the sibling's value: the bound is `key <relation> factor * sibling`.
  double factor = 1.0;
};

/// @brief A group of keys that exists only under a condition: an EQ band, a
///        multiband crossover band, or a dynamic sub-band inside one.
/// @details Recorded by the builder at the point it decides whether the group
///          exists, so the catalog reports the rule construction applies rather
///          than one inferred from key spelling.
struct SlotDeclaration {
  /// Enclosing slot's name, or empty for a top-level slot.
  std::string parent;
  /// True when the slot exists once any of its keys is supplied; false when it
  /// exists unconditionally (subject to @ref min_crossover_cutoffs).
  bool any_key = false;
  /// Crossover cutoffs in effect needed for the slot to exist; 0 for none.
  int min_crossover_cutoffs = 0;
};

/// @brief Flat (key -> value) param store that records which keys a config
/// builder probes, and what C++ type it read each one as.
///
/// Every config builder reads a param through `find()` (directly, or via the
/// `f` / `i` / `b` / `read_field` helpers below), and it probes a key even when
/// that key is absent (it then falls back to the field's default). Recording
/// each probed key therefore yields, for free and with no hand-maintained list:
///   - the set of keys a processor *consumes* (probe an empty map -> the names
///     it reads for a default configuration; see insert_param_names),
///   - the public type of each of those keys (see @ref probed_kinds), and
///   - the keys a caller supplied that *no* builder read (build the real map ->
///     `unprobed_keys()`; these took no effect and are surfaced as warnings).
/// The wrapper is a drop-in for the previous `std::unordered_map` alias: only
/// `find` / `end` / `operator[]` are used across the param surface.
class ParamMap {
 public:
  using Map = std::unordered_map<std::string, double>;
  using const_iterator = Map::const_iterator;

  /// @brief Inserts/overwrites a value (used only when building the map).
  double& operator[](const std::string& key) { return map_[key]; }

  /// @brief Looks up @p key, recording it as probed (whether present or not).
  /// @details A map that stopped recording declarations is a bounds probe, which never reads
  ///          the probed set back, so it skips the insert.
  const_iterator find(const std::string& key) const {
    if (records_declarations_) probed_.insert(key);
    return map_.find(key);
  }
  const_iterator find(const char* key) const {
    if (records_declarations_) probed_.insert(key);
    return map_.find(key);
  }
  const_iterator end() const { return map_.end(); }

  /// @brief Whether @p key was supplied, without recording it as probed.
  bool has(const std::string& key) const { return map_.find(key) != map_.end(); }

  /// @brief Whether a supplied key is @p prefix followed by one of @p fields, without recording
  ///        anything as probed. Scans the supplied keys, so it suits the one- or two-key maps of
  ///        a bounds probe better than a lookup per field.
  bool has_prefixed_field(const std::string& prefix, const std::vector<std::string>& fields) const {
    for (const auto& entry : map_) {
      const std::string& key = entry.first;
      if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0) continue;
      for (const std::string& field : fields) {
        if (key.compare(prefix.size(), std::string::npos, field) == 0) return true;
      }
    }
    return false;
  }

  /// @brief Records the C++ type a builder read @p key as.
  /// @details Called by the accessors below, so the recorded kind is the
  ///          declared type of the destination field (or of the accessor the
  ///          builder chose), not an inference from the key name. The widest
  ///          kind any builder used wins (@c Boolean < @c Integer < @c Number):
  ///          a wider read is evidence that the value is not a toggle or a
  ///          whole number, while a narrower read of the same key would only be
  ///          an aliasing bug.
  void note_kind(const std::string& key, ParamKind kind) const {
    auto [it, inserted] = kinds_.emplace(key, kind);
    if (!inserted && kind > it->second) it->second = kind;
  }

  /// @brief Public type of every key probed while building, keyed by name.
  const std::unordered_map<std::string, ParamKind>& probed_kinds() const { return kinds_; }

  /// @brief Records the fallback a builder used for @p key.
  /// @details Recorded on every probe, present or absent, so building against
  ///          an empty map yields the design default of every key a processor
  ///          consumes. Two probes that disagree mark the key ambiguous.
  void note_default(const std::string& key, double value) const {
    auto [it, inserted] = defaults_.emplace(key, ParamDefault{value, false});
    if (!inserted && !(it->second.value == value)) it->second.ambiguous = true;
  }

  /// @brief Design default of every key probed while building, keyed by name.
  const std::unordered_map<std::string, ParamDefault>& probed_defaults() const { return defaults_; }

  /// Effective construction values, including accessor fallbacks and aliases.
  /// Later writes win when a builder overlays the same config field twice.
  void note_effective(const std::string& key, double value) const {
    if (records_declarations_) effective_[key] = value;
  }
  const std::unordered_map<std::string, double>& effective_values() const { return effective_; }

  /// @brief Records the unit, scale and display range a builder declared for @p key.
  /// @details The first record wins: every reader of one key states the same meaning.
  void note_meta(const std::string& key, ParamMeta meta) const {
    if (records_declarations_) meta_.emplace(key, meta);
  }

  /// @brief Declared metadata of every numeric key probed with one, keyed by name.
  const std::unordered_map<std::string, ParamMeta>& probed_meta() const { return meta_; }

  /// @brief Records that @p key's value is bounded by @p sibling's under @p relation
  ///        (`key <relation> factor * sibling`).
  void note_depends(const std::string& key, const std::string& sibling, Relation relation,
                    double factor = 1.0) const {
    if (!records_declarations_) return;
    auto& list = depends_[key];
    for (const auto& existing : list) {
      if (existing.key == sibling) return;
    }
    list.push_back(ParamDependency{sibling, relation, factor});
  }

  /// @brief Sibling bounds declared for each key, in declaration order.
  const std::unordered_map<std::string, std::vector<ParamDependency>>& probed_depends() const {
    return depends_;
  }

  /// @brief Records that @p sibling's value moves @p key's accepted range in a way a
  ///        `note_depends` relation cannot state.
  void note_coupled(const std::string& key, const std::string& sibling) const {
    if (!records_declarations_) return;
    auto& list = coupled_[key];
    if (std::find(list.begin(), list.end(), sibling) == list.end()) list.push_back(sibling);
  }

  /// @brief Siblings declared as coupled to each key, in declaration order.
  const std::unordered_map<std::string, std::vector<std::string>>& probed_coupled() const {
    return coupled_;
  }

  /// @brief Records the declared values of the enum a builder read @p key as.
  /// @details The first record wins: a later one comes from the same enum type.
  void note_choices(const std::string& key, std::vector<EnumChoice> choices) const {
    choices_.emplace(key, std::move(choices));
  }

  /// @brief Declared enum values of every enum-typed key probed while building.
  const std::unordered_map<std::string, std::vector<EnumChoice>>& probed_choices() const {
    return choices_;
  }

  /// @brief Records slot @p name (its key prefix without the trailing dot).
  /// @details The first record wins: every builder reaching the same slot
  ///          applies the same rule to it.
  void note_slot(const std::string& name, SlotDeclaration declaration) const {
    for (const auto& slot : slots_) {
      if (slot.first == name) return;
    }
    slots_.emplace_back(name, std::move(declaration));
  }

  /// @brief Every slot declared while building, in declaration order.
  const std::vector<std::pair<std::string, SlotDeclaration>>& declared_slots() const {
    return slots_;
  }

  /// @brief Turns off the declaration-only work the catalog needs.
  /// @details A bounds probe builds the same processor thousands of times and
  ///          asks one question of each build — did construction throw. It has
  ///          already read the declarations off the first build, so replaying
  ///          them per probe is pure waste; the band declarations below skip
  ///          themselves for a map in this state.
  void stop_recording_declarations() { records_declarations_ = false; }
  bool records_declarations() const noexcept { return records_declarations_; }

  /// @brief Merges another map's recorded kinds and defaults into this one.
  /// @details The band readers below run against a throwaway EMPTY map to
  ///          declare a band's keys — an empty map reads nothing and throws
  ///          nothing, so what the run leaves behind is the key list with each
  ///          key's type and its config initializer, and nothing else. Adopting
  ///          that here publishes the band without probing the caller's values,
  ///          which keeps @ref unprobed_keys and every value-dependent throw
  ///          exactly as they were.
  void adopt_declarations(const ParamMap& other) const {
    for (const auto& [key, kind] : other.kinds_) note_kind(key, kind);
    for (const auto& [key, meta] : other.meta_) note_meta(key, meta);
    for (const auto& [key, list] : other.depends_) {
      for (const auto& dependency : list) {
        note_depends(key, dependency.key, dependency.relation, dependency.factor);
      }
    }
    for (const auto& [key, list] : other.coupled_) {
      for (const auto& sibling : list) note_coupled(key, sibling);
    }
    for (const auto& [key, value] : other.effective_) effective_.try_emplace(key, value);
    for (const auto& [key, choices] : other.choices_) note_choices(key, choices);
    for (const auto& [key, fallback] : other.defaults_) {
      if (!fallback.ambiguous) note_default(key, fallback.value);
    }
    for (const auto& [name, declaration] : other.slots_) note_slot(name, declaration);
  }

  /// @brief Keys this processor read (probed) when built; reflects an empty map
  /// as the names consumed for a default configuration.
  const std::unordered_set<std::string>& probed_keys() const { return probed_; }

  /// @brief Supplied keys that no builder probed, sorted for determinism. These
  /// were silently ignored by the processor and are reported as warnings.
  std::vector<std::string> unprobed_keys() const {
    std::vector<std::string> out;
    for (const auto& [key, value] : map_) {
      (void)value;
      if (probed_.find(key) == probed_.end()) out.push_back(key);
    }
    insertion_sort(out.begin(), out.end());
    return out;
  }

 private:
  Map map_;
  mutable std::unordered_set<std::string> probed_;
  mutable std::unordered_map<std::string, ParamKind> kinds_;
  mutable std::unordered_map<std::string, ParamMeta> meta_;
  mutable std::unordered_map<std::string, std::vector<ParamDependency>> depends_;
  mutable std::unordered_map<std::string, std::vector<std::string>> coupled_;
  mutable std::unordered_map<std::string, ParamDefault> defaults_;
  mutable std::unordered_map<std::string, double> effective_;
  mutable std::unordered_map<std::string, std::vector<EnumChoice>> choices_;
  mutable std::vector<std::pair<std::string, SlotDeclaration>> slots_;
  bool records_declarations_ = true;
};

inline ParamMap make_map(const std::vector<Param>& params) {
  validate_params(params.data(), params.size());
  ParamMap map;
  for (const auto& param : params) {
    map[param.key] = param.value;
  }
  return map;
}

/// @brief Refuses the supplied keys @p name never read, naming each one; no-op when empty.
inline void refuse_unread_params(const std::string& name, const std::vector<std::string>& keys) {
  if (keys.empty()) return;
  std::string joined;
  for (const std::string& key : keys) {
    if (!joined.empty()) joined += ", ";
    joined += key;
  }
  throw SonareException(ErrorCode::InvalidParameter,
                        name + " does not read parameter(s): " + joined);
}

inline float f(const ParamMap& params, const char* key, float default_value) {
  params.note_kind(key, ParamKind::Number);
  params.note_default(key, static_cast<double>(default_value));
  auto it = params.find(key);
  const float value = it == params.end() ? default_value : static_cast<float>(it->second);
  params.note_effective(key, value);
  return value;
}

inline float f(const ParamMap& params, const char* key, float default_value, ParamMeta meta) {
  params.note_meta(key, meta);
  return f(params, key, default_value);
}

inline int i(const ParamMap& params, const char* key, int default_value) {
  params.note_kind(key, ParamKind::Integer);
  params.note_default(key, static_cast<double>(default_value));
  auto it = params.find(key);
  if (it == params.end()) {
    params.note_effective(key, default_value);
    return default_value;
  }
  int converted = 0;
  if (numeric::checked_integral_cast(it->second, &converted)) {
    params.note_effective(key, converted);
    return converted;
  }
  // Named, because the key is in hand: a caller who wrote 512.7 needs to know
  // which field refused it and that a whole number is what it wants.
  reject_integer_param(key, it->second);
}

inline int i(const ParamMap& params, const char* key, int default_value, ParamMeta meta) {
  params.note_meta(key, meta);
  return i(params, key, default_value);
}

/// @brief Reads a whole number whose domain is the full uint32 range (a seed).
/// @details @ref i would refuse the upper half and a cast of its result would wrap a negative
///          onto it, so a negative is refused by name and every uint32 value is accepted.
inline std::uint32_t u32(const ParamMap& params, const char* key, std::uint32_t default_value) {
  params.note_kind(key, ParamKind::Integer);
  params.note_default(key, static_cast<double>(default_value));
  auto it = params.find(key);
  if (it == params.end()) {
    params.note_effective(key, static_cast<double>(default_value));
    return default_value;
  }
  if (it->second < 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be non-negative");
  }
  std::uint32_t converted = 0;
  if (!numeric::checked_integral_cast(it->second, &converted)) {
    reject_integer_param(key, it->second);
  }
  params.note_effective(key, static_cast<double>(converted));
  return converted;
}

inline std::uint32_t u32(const ParamMap& params, const char* key, std::uint32_t default_value,
                         ParamMeta meta) {
  params.note_meta(key, meta);
  return u32(params, key, default_value);
}

inline bool b(const ParamMap& params, const char* key, bool default_value) {
  params.note_kind(key, ParamKind::Boolean);
  params.note_default(key, default_value ? 1.0 : 0.0);
  auto it = params.find(key);
  if (it != params.end() && !common::valid_switch_value(it->second)) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be 0 or 1");
  }
  const bool value = it == params.end() ? default_value : it->second == 1.0;
  params.note_effective(key, value ? 1.0 : 0.0);
  return value;
}

/// @brief Reads a flat enum selector, refusing a value no enumerator declares.
template <typename Enum>
inline Enum read_enum(const ParamMap& params, const char* key, Enum fallback) {
  params.note_kind(key, ParamKind::Integer);
  params.note_default(key, field_as_double(fallback));
  if (params.records_declarations()) params.note_choices(key, enum_choices<Enum>());
  auto it = params.find(key);
  if (it == params.end()) {
    params.note_effective(key, field_as_double(fallback));
    return fallback;
  }
  int converted = 0;
  if (!numeric::checked_integral_cast(it->second, &converted)) {
    reject_integer_param(key, it->second);
  }
  const Enum candidate = static_cast<Enum>(converted);
  if (!enum_value_declared(candidate)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(key) + " is not a declared value");
  }
  params.note_effective(key, converted);
  return candidate;
}

/// @brief Declares a key a builder reads from the JSON side-channel, whose
///        value (a string or an array) the flat map cannot hold.
inline void note_side_channel_key(const ParamMap& params, const char* key, ParamKind kind) {
  params.note_kind(key, kind);
  (void)params.find(key);
}

/// @brief Parameter kind implied by a config member's declared type.
/// @details The single place the field-type -> @ref ParamKind mapping lives, so
/// no consumer of the SONARE_FIELDS_* tables has to keep a list of which keys
/// are toggles and which are whole numbers.
template <typename T>
inline constexpr ParamKind field_param_kind() {
  using Field = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<Field, bool>) {
    return ParamKind::Boolean;
  } else if constexpr (std::is_integral_v<Field> || std::is_enum_v<Field>) {
    return ParamKind::Integer;
  } else {
    return ParamKind::Number;
  }
}

/// @brief @ref read_field for an enum member, held as its wire value.
inline void read_enum_field(const ParamMap& params, const char* key, int& dst, EnumNameFn name_of) {
  params.note_kind(key, ParamKind::Integer);
  params.note_default(key, static_cast<double>(dst));
  if (params.records_declarations()) params.note_choices(key, enum_choices(name_of));
  auto it = params.find(key);
  if (it != params.end()) assign_enum_value(dst, it->second, name_of);
  params.note_effective(key, dst);
}

/// @brief Overlays a flat param onto a config field, leaving it untouched when
/// the key is absent. Paired with the SONARE_FIELDS_* tables so a config
/// builder is a single table expansion instead of one line per field.
/// @details The destination's declared type is what decides the parameter's
///          public kind, so a config field that is a C++ `bool` is published as
///          a boolean without anyone maintaining a second list of which keys
///          those are — and its initializer is what the catalog publishes as the
///          parameter's default.
template <typename T>
inline void read_field(const ParamMap& params, const char* key, T& dst) {
  if constexpr (std::is_enum_v<T>) {
    int value = static_cast<int>(dst);
    read_enum_field(params, key, value, &enum_choice_name_of<T>);
    dst = static_cast<T>(value);
  } else {
    params.note_kind(key, field_param_kind<T>());
    // Read before the overlay: `dst` still holds the config struct's own field
    // initializer here, which is exactly the default this key falls back to.
    params.note_default(key, field_as_double(dst));
    auto it = params.find(key);
    if (it != params.end()) assign_field(dst, it->second);
    params.note_effective(key, field_as_double(dst));
  }
}

/// @brief @ref read_field for a field table row, which also declares the key's metadata.
template <typename T>
inline void read_field(const ParamMap& params, const char* key, T& dst, ParamMeta meta) {
  params.note_meta(key, meta);
  read_field(params, key, dst);
}

/// @brief Declares `first <relation> factor * second` on both keys, so each names the other as
///        its bound.
inline void note_pair_order(const ParamMap& params, const char* first, Relation relation,
                            const char* second, double factor = 1.0) {
  params.note_depends(first, second, relation, factor);
  params.note_depends(second, first, inverse(relation), 1.0 / factor);
}

// Expands one SONARE_FIELDS_* table row into a field overlay. A config builder
// is then just `Config config; SONARE_FIELDS_X(SONARE_READ_FIELD); return ...`,
// equivalent to the prior per-field `config.x = f(params, "x", config.x)` lines.
#define SONARE_READ_FIELD(key, member, meta) read_field(params, key, config.member, meta);

}  // namespace sonare::mastering::api::detail
