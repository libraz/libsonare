#pragma once

/// @file processor_params.h
/// @brief Internal helpers shared by named_processor.cpp and insert_factory.cpp
///        for turning a flat list of (key, value) params into processor configs.
///
/// This header is INTERNAL to the mastering API; it is not part of the public
/// surface. It centralizes the param-name -> config-field mapping so the
/// offline (named_processor) and streaming (insert_factory) paths stay in sync.

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
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/ducking_processor.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/dynamics/vocal_rider.h"
#include "mastering/eq/api_style.h"
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/dither.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/maximizer.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/crossover.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/amp_presets.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mastering/saturation/pedal.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/transformer.h"
#include "mastering/saturation/tube.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "mastering/stereo/auto_pan.h"
#include "mastering/stereo/binaural_panner.h"
#include "mastering/stereo/haas_enhancer.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"
#include "mastering/utility/gain.h"
#include "util/constants.h"
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
  const_iterator find(const std::string& key) const {
    probed_.insert(key);
    return map_.find(key);
  }
  const_iterator find(const char* key) const {
    probed_.insert(key);
    return map_.find(key);
  }
  const_iterator end() const { return map_.end(); }

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
  ///        (`key <relation> sibling`).
  void note_depends(const std::string& key, const std::string& sibling, Relation relation) const {
    if (!records_declarations_) return;
    auto& list = depends_[key];
    for (const auto& existing : list) {
      if (existing.key == sibling) return;
    }
    list.push_back(ParamDependency{sibling, relation});
  }

  /// @brief Sibling bounds declared for each key, in declaration order.
  const std::unordered_map<std::string, std::vector<ParamDependency>>& probed_depends() const {
    return depends_;
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
      for (const auto& dependency : list) note_depends(key, dependency.key, dependency.relation);
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

/// @brief Declares `first <relation> second` on both keys, so each names the other as its bound.
inline void note_pair_order(const ParamMap& params, const char* first, Relation relation,
                            const char* second) {
  params.note_depends(first, second, relation);
  params.note_depends(second, first, inverse(relation));
}

/// Most `cutoff<i>Hz` keys a crossover reads, so one fewer than the most bands it splits into.
inline constexpr int kMaxCrossoverCutoffs = 8;
inline constexpr size_t kMaxCrossoverBands = kMaxCrossoverCutoffs + 1;

inline std::vector<float> cutoffs(const ParamMap& params) {
  const multiband::CrossoverConfig defaults;
  std::vector<float> values;
  for (int index = 0; index < kMaxCrossoverCutoffs; ++index) {
    const std::string key = "cutoff" + std::to_string(index) + "Hz";
    params.note_kind(key, ParamKind::Number);
    params.note_meta(key, kHzLog);
    if (index > 0) {
      note_pair_order(params, ("cutoff" + std::to_string(index - 1) + "Hz").c_str(), Relation::Lt,
                      key.c_str());
    }
    // A cutoff beyond the default split has no fallback to publish.
    if (static_cast<size_t>(index) < defaults.cutoffs_hz.size()) {
      params.note_default(key, static_cast<double>(defaults.cutoffs_hz[index]));
    }
    auto it = params.find(key);
    if (it != params.end()) {
      values.push_back(static_cast<float>(it->second));
    }
  }
  return values;
}

inline eq::EqBand eq_band(const ParamMap& params, const std::string& prefix) {
  eq::EqBand band;
  band.type = read_enum(params, (prefix + "type").c_str(), band.type);
  band.coeff_mode = read_enum(params, (prefix + "coeffMode").c_str(), band.coeff_mode);
  band.frequency_hz = f(params, (prefix + "frequencyHz").c_str(), band.frequency_hz, kHzLog);
  band.gain_db = f(params, (prefix + "gainDb").c_str(), band.gain_db, kDb);
  band.q = f(params, (prefix + "q").c_str(), band.q, kNone);
  band.enabled = b(params, (prefix + "enabled").c_str(), true);
  band.slope_db_oct = i(params, (prefix + "slopeDbOct").c_str(), band.slope_db_oct, kDbPerOctave);
  band.placement = read_enum(params, (prefix + "placement").c_str(), band.placement);
  band.phase = read_enum(params, (prefix + "phase").c_str(), band.phase);
  band.soloed = b(params, (prefix + "soloed").c_str(), band.soloed);
  band.bypassed = b(params, (prefix + "bypassed").c_str(), band.bypassed);
  band.proportional_q = b(params, (prefix + "proportionalQ").c_str(), band.proportional_q);
  band.proportional_q_strength =
      f(params, (prefix + "proportionalQStrength").c_str(), band.proportional_q_strength, kNone);
  band.dyn.enabled = b(params, (prefix + "dynamic").c_str(), band.dyn.enabled);
  band.dyn.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.dyn.threshold_db, kDb);
  band.dyn.auto_threshold = b(params, (prefix + "autoThreshold").c_str(), band.dyn.auto_threshold);
  band.dyn.ratio = f(params, (prefix + "ratio").c_str(), band.dyn.ratio, kRatio);
  band.dyn.range_db = f(params, (prefix + "rangeDb").c_str(), band.dyn.range_db, kDb);
  band.dyn.attack_ms = f(params, (prefix + "attackMs").c_str(), band.dyn.attack_ms, kMsLog);
  band.dyn.release_ms = f(params, (prefix + "releaseMs").c_str(), band.dyn.release_ms, kMsLog);
  // "lookaheadMs" is the field's former (misleading) spelling; still accepted
  // so a stored config using it keeps working, but "detectorDelayMs" wins if
  // both are present.
  band.dyn.detector_delay_ms =
      f(params, (prefix + "lookaheadMs").c_str(), band.dyn.detector_delay_ms, kMs);
  band.dyn.detector_delay_ms =
      f(params, (prefix + "detectorDelayMs").c_str(), band.dyn.detector_delay_ms, kMs);
  band.dyn.sidechain_freq_hz =
      f(params, (prefix + "sidechainFreqHz").c_str(), band.dyn.sidechain_freq_hz, kHz);
  band.dyn.sidechain_q = f(params, (prefix + "sidechainQ").c_str(), band.dyn.sidechain_q, kNone);
  return band;
}

/// @brief Publishes one EQ band's keys, types and defaults to the catalog.
/// @details A band the caller supplied no key for is never read, so without
/// this the whole per-band surface — the bulk of the flat parameter set —
/// would carry no declared type and no default. @ref eq_band stays the single
/// source of that list: it is replayed against an empty map, which cannot read
/// or reject anything, and only the recording is kept.
inline void declare_eq_band_params(const ParamMap& params, const std::string& prefix) {
  if (!params.records_declarations()) return;
  ParamMap declaration;
  (void)eq_band(declaration, prefix);
  params.adopt_declarations(declaration);
}

/// @brief Reads one dynamic-EQ band from `<prefix><field>` keys.
/// @details Shared by the single-band DynamicEq insert and the multiband
/// dynamic EQ's per-crossover-band sub-bands, which read the identical field
/// set; the only difference is how the prefix is built.
inline eq::DynamicEqBand dynamic_eq_band(const ParamMap& params, const std::string& prefix) {
  eq::DynamicEqBand band;
  band.type = read_enum(params, (prefix + "type").c_str(), band.type);
  band.frequency_hz = f(params, (prefix + "frequencyHz").c_str(), band.frequency_hz, kHzLog);
  band.static_gain_db = f(params, (prefix + "staticGainDb").c_str(), band.static_gain_db, kDb);
  band.q = f(params, (prefix + "q").c_str(), band.q, kNone);
  band.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.threshold_db, kDb);
  band.ratio = f(params, (prefix + "ratio").c_str(), band.ratio, kRatio);
  band.range_db = f(params, (prefix + "rangeDb").c_str(), band.range_db, kDb);
  band.enabled = b(params, (prefix + "enabled").c_str(), true);
  band.sidechain_q = f(params, (prefix + "sidechainQ").c_str(), band.sidechain_q, kNone);
  band.sidechain_freq_hz =
      f(params, (prefix + "sidechainFreqHz").c_str(), band.sidechain_freq_hz, kHz);
  band.attack_ms = f(params, (prefix + "attackMs").c_str(), band.attack_ms, kMsLog);
  band.release_ms = f(params, (prefix + "releaseMs").c_str(), band.release_ms, kMsLog);
  // "lookaheadMs" is the field's former (misleading) spelling; still accepted
  // so a stored config using it keeps working, but "detectorDelayMs" wins if
  // both are present.
  band.detector_delay_ms = f(params, (prefix + "lookaheadMs").c_str(), band.detector_delay_ms, kMs);
  band.detector_delay_ms =
      f(params, (prefix + "detectorDelayMs").c_str(), band.detector_delay_ms, kMs);
  return band;
}

/// @brief Publishes one dynamic-EQ band's keys, types and defaults. Declaration
/// counterpart of @ref dynamic_eq_band; see @ref declare_eq_band_params.
inline void declare_dynamic_eq_band_params(const ParamMap& params, const std::string& prefix) {
  if (!params.records_declarations()) return;
  ParamMap declaration;
  (void)dynamic_eq_band(declaration, prefix);
  params.adopt_declarations(declaration);
}

/// @brief The field names @p read_band reads, found by replaying it against an
///        empty map under an empty prefix, so no field list is kept by hand.
template <typename ReadBand>
inline std::vector<std::string> band_fields_of(ReadBand read_band) {
  ParamMap declaration;
  read_band(declaration);
  std::vector<std::string> fields(declaration.probed_keys().begin(),
                                  declaration.probed_keys().end());
  insertion_sort(fields.begin(), fields.end());
  return fields;
}

inline const std::vector<std::string>& eq_band_fields() {
  static const std::vector<std::string> fields =
      band_fields_of([](const ParamMap& params) { (void)eq_band(params, ""); });
  return fields;
}

inline const std::vector<std::string>& dynamic_eq_band_fields() {
  static const std::vector<std::string> fields =
      band_fields_of([](const ParamMap& params) { (void)dynamic_eq_band(params, ""); });
  return fields;
}

/// @brief Declares the presence-gated slot at @p prefix and reports whether the
///        caller supplied any of its @p fields, which is what makes it exist.
/// @details Every EQ band and dynamic sub-band goes through here, so the rule
///          construction applies and the one the catalog publishes are one rule.
inline bool supplied_slot(const ParamMap& params, const std::string& prefix,
                          const std::vector<std::string>& fields,
                          const std::string& parent = std::string()) {
  if (params.records_declarations()) {
    params.note_slot(prefix.substr(0, prefix.size() - 1), SlotDeclaration{parent, true, 0});
  }
  for (const std::string& field : fields) {
    if (params.find(prefix + field) != params.end()) return true;
  }
  return false;
}

/// @brief Declares crossover band @p index, which exists once the crossover in
///        effect has at least @p index cutoffs.
inline void note_crossover_band_slot(const ParamMap& params, size_t index) {
  if (!params.records_declarations()) return;
  params.note_slot("band" + std::to_string(index),
                   SlotDeclaration{std::string(), false, static_cast<int>(index)});
}

inline void configure_parametric(eq::ParametricEq& processor, const ParamMap& params,
                                 const std::string& prefix = "band") {
  for (size_t index = 0; index < eq::ParametricEq::kMaxBands; ++index) {
    const std::string band_prefix = prefix + std::to_string(index) + ".";
    declare_eq_band_params(params, band_prefix);
    if (supplied_slot(params, band_prefix, eq_band_fields())) {
      processor.set_band(index, eq_band(params, band_prefix));
    }
  }
}

inline void configure_equalizer(eq::EqualizerProcessor& processor, const ParamMap& params,
                                const std::string& prefix = "band") {
  processor.set_auto_gain_enabled(b(params, "autoGain", processor.auto_gain_enabled()));
  processor.set_gain_scale(f(params, "gainScale", processor.gain_scale(), kNone));
  processor.set_output_gain_db(f(params, "outputGainDb", processor.output_gain_db(), kDb));
  processor.set_output_pan(f(params, "outputPan", processor.output_pan(), kNone));
  processor.set_phase_mode(read_enum(params, "phaseMode", processor.phase_mode()));
  for (size_t index = 0; index < eq::EqualizerProcessor::kMaxBands; ++index) {
    const std::string band_prefix = prefix + std::to_string(index) + ".";
    declare_eq_band_params(params, band_prefix);
    if (supplied_slot(params, band_prefix, eq_band_fields())) {
      processor.set_band(index, eq_band(params, band_prefix));
    }
  }
}

// Expands one SONARE_FIELDS_* table row into a field overlay. A config builder
// is then just `Config config; SONARE_FIELDS_X(SONARE_READ_FIELD); return ...`,
// equivalent to the prior per-field `config.x = f(params, "x", config.x)` lines.
#define SONARE_READ_FIELD(key, member, meta) read_field(params, key, config.member, meta);

inline dynamics::CompressorConfig compressor_config(const ParamMap& params) {
  dynamics::CompressorConfig config;
  SONARE_FIELDS_COMPRESSOR(SONARE_READ_FIELD)
  return config;
}

inline dynamics::LimiterConfig limiter_config(const ParamMap& params) {
  dynamics::LimiterConfig config;
  SONARE_FIELDS_LIMITER(SONARE_READ_FIELD)
  return config;
}

inline multiband::CrossoverConfig crossover_config(const ParamMap& params) {
  multiband::CrossoverConfig config;
  auto values = cutoffs(params);
  if (!values.empty()) {
    config.cutoffs_hz = values;
  }
  config.slope = read_enum(params, "slope", config.slope);
  config.mode = read_enum(params, "mode", config.mode);
  config.fir_kernel_size = i(params, "firKernelSize", config.fir_kernel_size, kSamples);
  return config;
}

// ---------------------------------------------------------------------------
// Multiband per-band population
//
// Each multiband processor exposes its sub-bands through a `band{i}.<field>`
// flat-key convention (matching multiband_exciter_config()). The crossover
// config decides how many bands exist; these helpers first resize the `bands`
// vector to match the crossover (one more band than cutoffs) -- preserving the
// factory defaults at overlapping indices and default-constructing any extra
// bands -- then overlay any caller-supplied per-band fields onto those defaults
// so they are not silently ignored. Resizing here is what lets an arbitrary
// (non-default) cutoff count reach validate_config() with a matching band count
// instead of hard-failing the "band count must match crossover" check.
// ---------------------------------------------------------------------------

// Resize @p bands to `cutoffs + 1` to match the crossover's band count, keeping
// any existing per-band defaults at overlapping indices.
template <typename BandConfig>
inline void resize_bands_to_crossover(std::vector<BandConfig>& bands,
                                      const multiband::CrossoverConfig& crossover) {
  bands.resize(crossover.cutoffs_hz.size() + 1);
}

/// @brief Reads every crossover band the crossover in effect has, and declares
///        the rest up to @ref kMaxCrossoverBands so the catalog lists each band
///        a host can reach by supplying more cutoffs.
/// @details @p read_band(params, prefix, band) overlays one band; a declared
///          band replays it against an empty map and a default-constructed band,
///          which is the band an extra cutoff creates.
template <typename BandConfig, typename ReadBand>
inline void populate_crossover_bands(std::vector<BandConfig>& bands,
                                     const multiband::CrossoverConfig& crossover,
                                     const ParamMap& params, ReadBand read_band) {
  resize_bands_to_crossover(bands, crossover);
  for (size_t index = 0; index < kMaxCrossoverBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    note_crossover_band_slot(params, index);
    if (index < bands.size()) {
      read_band(params, prefix, bands[index]);
    } else if (params.records_declarations()) {
      ParamMap declaration;
      BandConfig band{};
      read_band(declaration, prefix, band);
      params.adopt_declarations(declaration);
    }
  }
}

inline void populate_compressor_bands(multiband::MultibandCompressorConfig& config,
                                      const ParamMap& params) {
  populate_crossover_bands(
      config.bands, config.crossover, params,
      [](const ParamMap& band_params, const std::string& prefix, auto& band) {
        band.threshold_db =
            f(band_params, (prefix + "thresholdDb").c_str(), band.threshold_db, kDb);
        band.ratio = f(band_params, (prefix + "ratio").c_str(), band.ratio, kRatio);
        band.attack_ms = f(band_params, (prefix + "attackMs").c_str(), band.attack_ms, kMsLog);
        band.release_ms = f(band_params, (prefix + "releaseMs").c_str(), band.release_ms, kMsLog);
        band.knee_db = f(band_params, (prefix + "kneeDb").c_str(), band.knee_db, kDb);
        band.makeup_gain_db =
            f(band_params, (prefix + "makeupGainDb").c_str(), band.makeup_gain_db, kDb);
      });
}

inline void populate_expander_bands(multiband::MultibandExpanderConfig& config,
                                    const ParamMap& params) {
  populate_crossover_bands(
      config.bands, config.crossover, params,
      [](const ParamMap& band_params, const std::string& prefix, auto& band) {
        band.threshold_db =
            f(band_params, (prefix + "thresholdDb").c_str(), band.threshold_db, kDb);
        band.ratio = f(band_params, (prefix + "ratio").c_str(), band.ratio, kRatio);
        band.attack_ms = f(band_params, (prefix + "attackMs").c_str(), band.attack_ms, kMsLog);
        band.release_ms = f(band_params, (prefix + "releaseMs").c_str(), band.release_ms, kMsLog);
        band.range_db = f(band_params, (prefix + "rangeDb").c_str(), band.range_db, kDb);
      });
}

inline void populate_limiter_bands(multiband::MultibandLimiterConfig& config,
                                   const ParamMap& params) {
  populate_crossover_bands(
      config.bands, config.crossover, params,
      [](const ParamMap& band_params, const std::string& prefix, auto& band) {
        band.threshold_db =
            f(band_params, (prefix + "thresholdDb").c_str(), band.threshold_db, kDb);
        band.lookahead_ms =
            f(band_params, (prefix + "lookaheadMs").c_str(), band.lookahead_ms, kMs);
        band.release_ms = f(band_params, (prefix + "releaseMs").c_str(), band.release_ms, kMsLog);
      });
}

// Overlay per-band fields (band{i}.driveDb / .mix / .outputGainDb / .type /
// .enabled). `type` selects the algorithm the band delegates to and `enabled`
// bypasses the band, so both must be read here or every band runs the default
// soft clipper and no band can be switched off.
inline void populate_saturation_bands(multiband::MultibandSaturationConfig& config,
                                      const ParamMap& params) {
  populate_crossover_bands(
      config.bands, config.crossover, params,
      [](const ParamMap& band_params, const std::string& prefix, auto& band) {
        band.drive_db = f(band_params, (prefix + "driveDb").c_str(), band.drive_db, kDb);
        band.mix = f(band_params, (prefix + "mix").c_str(), band.mix, kNone);
        band.output_gain_db =
            f(band_params, (prefix + "outputGainDb").c_str(), band.output_gain_db, kDb);
        band.type = read_enum(band_params, (prefix + "type").c_str(), band.type);
        band.enabled = b(band_params, (prefix + "enabled").c_str(), band.enabled);
      });
}

// Resize the imager bands to match the crossover, then overlay per-band fields
// (band{i}.width / .decorrelationAmount / .enabled / .preserveEnergy) so a
// non-default crossover works and per-band settings are not silently dropped.
inline void populate_imager_bands(multiband::MultibandImagerConfig& config,
                                  const ParamMap& params) {
  populate_crossover_bands(
      config.bands, config.crossover, params,
      [](const ParamMap& band_params, const std::string& prefix, auto& band) {
        band.width = f(band_params, (prefix + "width").c_str(), band.width, kNone);
        band.decorrelation_amount = f(band_params, (prefix + "decorrelationAmount").c_str(),
                                      band.decorrelation_amount, kNone);
        band.enabled = b(band_params, (prefix + "enabled").c_str(), band.enabled);
        band.preserve_energy =
            b(band_params, (prefix + "preserveEnergy").c_str(), band.preserve_energy);
      });
}

// Resize the dynamic-EQ crossover bands to match the crossover, then read each
// crossover band's list of dynamic sub-bands from band{i}.dyn{j}.<field> keys.
// A sub-band exists once any of its keys is supplied, as a DynamicEq band does,
// and stays at slot j: absent lower slots hold disabled bands so the published
// band{i}.dyn{j} parameter ids address it.
inline void populate_dynamic_eq_bands(multiband::MultibandDynamicEqConfig& config,
                                      const ParamMap& params) {
  config.bands.resize(config.crossover.cutoffs_hz.size() + 1);
  for (size_t index = 0; index < kMaxCrossoverBands; ++index) {
    const std::string band = "band" + std::to_string(index);
    note_crossover_band_slot(params, index);
    const bool exists = index < config.bands.size();
    if (exists) config.bands[index].clear();
    for (size_t sub = 0; sub < eq::DynamicEq::kMaxBands; ++sub) {
      const std::string prefix = band + ".dyn" + std::to_string(sub) + ".";
      declare_dynamic_eq_band_params(params, prefix);
      if (!exists) {
        // Declared only: probing the keys of a band the crossover lacks would
        // hide them from the unread-key report.
        if (params.records_declarations()) {
          params.note_slot(prefix.substr(0, prefix.size() - 1), SlotDeclaration{band, true, 0});
        }
        continue;
      }
      if (supplied_slot(params, prefix, dynamic_eq_band_fields(), band)) {
        config.bands[index].resize(sub);
        config.bands[index].push_back(dynamic_eq_band(params, prefix));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Dynamics
// ---------------------------------------------------------------------------

inline dynamics::BrickwallLimiterConfig brickwall_limiter_config(const ParamMap& params) {
  dynamics::BrickwallLimiterConfig config;
  SONARE_FIELDS_BRICKWALL_LIMITER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::DeEsserConfig deesser_config(const ParamMap& params) {
  dynamics::DeEsserConfig config;
  SONARE_FIELDS_DEESSER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::ExpanderConfig expander_config(const ParamMap& params) {
  dynamics::ExpanderConfig config;
  SONARE_FIELDS_EXPANDER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::GateConfig gate_config(const ParamMap& params) {
  dynamics::GateConfig config;
  SONARE_FIELDS_GATE(SONARE_READ_FIELD)
  note_pair_order(params, "closeThresholdDb", Relation::Le, "thresholdDb");
  return config;
}

inline dynamics::ParallelCompConfig parallel_comp_config(const ParamMap& params) {
  dynamics::ParallelCompConfig config;
  SONARE_FIELDS_PARALLEL_COMP(SONARE_READ_FIELD)
  return config;
}

inline dynamics::SidechainRouterConfig sidechain_router_config(const ParamMap& params) {
  dynamics::SidechainRouterConfig config;
  SONARE_FIELDS_SIDECHAIN_ROUTER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::DuckingConfig ducking_config(const ParamMap& params) {
  dynamics::DuckingConfig config;
  SONARE_FIELDS_DUCKING(SONARE_READ_FIELD)
  return config;
}

inline dynamics::TransientShaperConfig transient_shaper_config(const ParamMap& params) {
  dynamics::TransientShaperConfig config;
  SONARE_FIELDS_TRANSIENT_SHAPER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::UpwardCompressorConfig upward_compressor_config(const ParamMap& params) {
  dynamics::UpwardCompressorConfig config;
  SONARE_FIELDS_UPWARD_COMPRESSOR(SONARE_READ_FIELD)
  return config;
}

inline dynamics::UpwardExpanderConfig upward_expander_config(const ParamMap& params) {
  dynamics::UpwardExpanderConfig config;
  SONARE_FIELDS_UPWARD_EXPANDER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::VocalRiderConfig vocal_rider_config(const ParamMap& params) {
  dynamics::VocalRiderConfig config;
  SONARE_FIELDS_VOCAL_RIDER(SONARE_READ_FIELD)
  return config;
}

// ---------------------------------------------------------------------------
// EQ (setter-based and config-based)
// ---------------------------------------------------------------------------

inline void configure_tilt(eq::TiltEq& p, const ParamMap& params) {
  p.set_tilt_db(f(params, "tiltDb", 0.0f, display_range(kDb, -12, 12)));
  p.set_pivot_hz(f(params, "pivotHz", 1000.0f, display_range(kHzLog, 20, 20000)));
}

inline void configure_api_style(eq::ApiStyleEq& p, const ParamMap& params) {
  p.set_band(eq::ApiStyleEq::Band::Low, f(params, "lowFrequencyHz", 100.0f, kHzLog),
             f(params, "lowGainDb", 0.0f, kDb));
  p.set_band(eq::ApiStyleEq::Band::LowMid, f(params, "lowMidFrequencyHz", 400.0f, kHzLog),
             f(params, "lowMidGainDb", 0.0f, kDb));
  p.set_band(eq::ApiStyleEq::Band::HighMid, f(params, "highMidFrequencyHz", 3000.0f, kHzLog),
             f(params, "highMidGainDb", 0.0f, kDb));
  p.set_band(eq::ApiStyleEq::Band::High, f(params, "highFrequencyHz", 10000.0f, kHzLog),
             f(params, "highGainDb", 0.0f, kDb));
}

inline void configure_minimum_phase(eq::MinimumPhaseEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::MinimumPhaseEq::kMaxBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    declare_eq_band_params(params, prefix);
    if (supplied_slot(params, prefix, eq_band_fields())) {
      p.set_band(index, eq_band(params, prefix));
    }
  }
}

inline eq::LinearPhaseEqConfig linear_phase_config(const ParamMap& params) {
  eq::LinearPhaseEqConfig config;
  config.resolution = read_enum(params, "resolution", config.resolution);
  config.fft_size = i(params, "fftSize", config.fft_size, kSamples);
  config.kernel_size = i(params, "kernelSize", config.kernel_size, kSamples);
  note_pair_order(params, "kernelSize", Relation::Le, "fftSize");
  config.use_partitioned_convolution =
      b(params, "usePartitionedConvolution", config.use_partitioned_convolution);
  config.partition_size = i(params, "partitionSize", config.partition_size, kSamples);
  return config;
}

inline eq::EqualizerProcessorConfig equalizer_config(const ParamMap& params, int max_channels) {
  eq::EqualizerProcessorConfig config;
  config.max_channels = max_channels;
  config.linear_phase_config = linear_phase_config(params);
  return config;
}

inline void configure_linear_phase_bands(eq::LinearPhaseEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::LinearPhaseEq::kMaxBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    declare_eq_band_params(params, prefix);
    if (supplied_slot(params, prefix, eq_band_fields())) {
      p.set_band(index, eq_band(params, prefix));
    }
  }
}

inline void configure_dynamic_eq_bands(eq::DynamicEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::DynamicEq::kMaxBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    declare_dynamic_eq_band_params(params, prefix);
    if (supplied_slot(params, prefix, dynamic_eq_band_fields())) {
      p.set_band(index, dynamic_eq_band(params, prefix));
    }
  }
}

inline void configure_pultec(eq::PultecEq& p, const ParamMap& params) {
  p.set_low_frequency(f(params, "lowFrequencyHz", 60.0f, kHzLog));
  p.set_low_boost(f(params, "lowBoost", 0.0f, kNone));
  p.set_low_attenuation(f(params, "lowAttenuation", 0.0f, kNone));
  p.set_high_boost(f(params, "highBoostFrequencyHz", 8000.0f, kHzLog),
                   f(params, "highBoost", 0.0f, kNone), f(params, "highBandwidth", 0.5f, kNone));
  p.set_high_attenuation(f(params, "highAttenuationFrequencyHz", 10000.0f, kHzLog),
                         f(params, "highAttenuation", 0.0f, kNone));
  p.set_component_model(read_enum(params, "componentModel", eq::PultecComponentModel::CurveOnly));
  p.set_output_drive(f(params, "outputDrive", 0.0f, kNone));
}

inline void configure_cut_filter(eq::CutFilter& p, const ParamMap& params) {
  p.set_high_pass(f(params, "highPassFrequencyHz", 20.0f, kHzLog),
                  f(params, "highPassQ", constants::kButterworthQ, kNone),
                  read_enum(params, "highPassSlope", eq::CutFilterSlope::Db12PerOct),
                  b(params, "highPassEnabled", false));
  p.set_low_pass(f(params, "lowPassFrequencyHz", 20000.0f, kHzLog),
                 f(params, "lowPassQ", constants::kButterworthQ, kNone),
                 read_enum(params, "lowPassSlope", eq::CutFilterSlope::Db12PerOct),
                 b(params, "lowPassEnabled", false));
}

inline void configure_band_pass(eq::BandPassEq& p, const ParamMap& params) {
  p.set_band_pass(f(params, "bandPassFrequencyHz", 1000.0f, kHzLog),
                  f(params, "bandPassQ", 1.0f, kNone), b(params, "bandPassEnabled", true));
  p.set_notch(f(params, "notchFrequencyHz", 1000.0f, kHzLog), f(params, "notchQ", 1.0f, kNone),
              b(params, "notchEnabled", false));
}

inline void configure_shelving(eq::ShelvingEq& p, const ParamMap& params) {
  p.set_low_shelf(f(params, "lowFrequencyHz", 100.0f, kHzLog), f(params, "lowGainDb", 0.0f, kDb),
                  f(params, "lowQ", constants::kButterworthQ, kNone),
                  b(params, "lowEnabled", true));
  p.set_high_shelf(
      f(params, "highFrequencyHz", 10000.0f, kHzLog), f(params, "highGainDb", 0.0f, kDb),
      f(params, "highQ", constants::kButterworthQ, kNone), b(params, "highEnabled", true));
}

inline void configure_graphic(eq::GraphicEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::GraphicEq::kNumBands; ++index) {
    const std::string key = "band" + std::to_string(index) + "GainDb";
    // Read unconditionally so every slider's type and flat default reach the
    // catalog; only applying it is conditional, so an unsupplied band keeps the
    // processor's own gain. The read cannot throw and probes the key the
    // presence test probes anyway, so nothing else changes.
    const float gain_db = f(params, key.c_str(), 0.0f, kDb);
    if (params.find(key) != params.end()) p.set_gain_db(index, gain_db);
  }
}

inline void configure_mid_side(eq::MidSideEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::MidSideEq::kMaxBands; ++index) {
    const std::string mid = "midBand" + std::to_string(index) + ".";
    const std::string side = "sideBand" + std::to_string(index) + ".";
    declare_eq_band_params(params, mid);
    declare_eq_band_params(params, side);
    if (supplied_slot(params, mid, eq_band_fields())) {
      p.set_mid_band(index, eq_band(params, mid));
    }
    if (supplied_slot(params, side, eq_band_fields())) {
      p.set_side_band(index, eq_band(params, side));
    }
  }
}

// ---------------------------------------------------------------------------
// Saturation
// ---------------------------------------------------------------------------

inline saturation::TapeConfig tape_config(const ParamMap& params) {
  saturation::TapeConfig config;
  SONARE_FIELDS_TAPE(SONARE_READ_FIELD)
  return config;
}

inline saturation::ExciterConfig exciter_config(const ParamMap& params) {
  saturation::ExciterConfig config;
  SONARE_FIELDS_EXCITER(SONARE_READ_FIELD)
  return config;
}

inline saturation::BitCrusherConfig bitcrusher_config(const ParamMap& params) {
  saturation::BitCrusherConfig config;
  SONARE_FIELDS_BITCRUSHER(SONARE_READ_FIELD)
  return config;
}

inline saturation::HardClipperConfig hard_clipper_config(const ParamMap& params) {
  saturation::HardClipperConfig config;
  SONARE_FIELDS_HARD_CLIPPER(SONARE_READ_FIELD)
  return config;
}

inline saturation::SoftClipperConfig soft_clipper_config(const ParamMap& params) {
  saturation::SoftClipperConfig config;
  SONARE_FIELDS_SOFT_CLIPPER(SONARE_READ_FIELD)
  return config;
}

inline saturation::WaveshaperConfig waveshaper_config(const ParamMap& params) {
  saturation::WaveshaperConfig config;
  SONARE_FIELDS_WAVESHAPER(SONARE_READ_FIELD)
  return config;
}

inline saturation::TubeConfig tube_config(const ParamMap& params) {
  saturation::TubeConfig config;
  SONARE_FIELDS_TUBE(SONARE_READ_FIELD)
  return config;
}

inline saturation::TransformerConfig transformer_config(const ParamMap& params) {
  saturation::TransformerConfig config;
  SONARE_FIELDS_TRANSFORMER(SONARE_READ_FIELD)
  return config;
}

inline saturation::OverdriveConfig overdrive_config(const ParamMap& params) {
  saturation::OverdriveConfig config;
  SONARE_FIELDS_PEDAL(SONARE_READ_FIELD)
  return config;
}

inline saturation::DistortionConfig distortion_config(const ParamMap& params) {
  saturation::DistortionConfig config;
  SONARE_FIELDS_PEDAL(SONARE_READ_FIELD)
  return config;
}

inline saturation::MultibandExciterConfig multiband_exciter_config(const ParamMap& params) {
  saturation::MultibandExciterConfig config;
  config.crossover = crossover_config(params);
  populate_crossover_bands(
      config.bands, config.crossover, params,
      [](const ParamMap& band_params, const std::string& prefix, auto& band) {
        band.frequency_hz =
            f(band_params, (prefix + "frequencyHz").c_str(), band.frequency_hz, kHzLog);
        band.drive_db = f(band_params, (prefix + "driveDb").c_str(), band.drive_db, kDb);
        band.amount = f(band_params, (prefix + "amount").c_str(), band.amount, kNone);
        band.q = f(band_params, (prefix + "q").c_str(), band.q, kNone);
        band.even_odd_mix =
            f(band_params, (prefix + "evenOddMix").c_str(), band.even_odd_mix, kNone);
      });
  return config;
}

/// Applies the synthesized-cabinet keys to a constructed amp. Shared so the
/// offline path and the insert factory cannot diverge on which of them a caller
/// can reach: a supplied base64 capture stays on the JSON side-channel, which
/// is the one thing a flat numeric list cannot carry, but a cabinet the model
/// synthesizes needs nothing but numbers.
inline void apply_amp_cab_ir(const ParamMap& params, saturation::AmpSim& amp) {
  const bool generate = b(params, "cabIrGenerate", false);
  const bool drivers = b(params, "cabIrDrivers", true);
  if (!generate) return;
  const saturation::AmpSimConfig& configured = amp.amp_config();
  saturation::CabIrSpec spec;
  spec.cab_model = configured.cab_model;
  spec.mic_model = configured.mic_model;
  spec.mic_axis = configured.mic_axis;
  spec.mic_distance_cm = configured.mic_distance_cm;
  spec.presence_db = configured.presence_db;
  spec.multi_driver = drivers;
  amp.load_generated_cab_ir(spec);
}

/// @param base Starting point every key rides on top of — a preset's config, or
///        a default-constructed one. An unset key keeps the base's value, which
///        is what lets `{"preset":"britStack","drive":0.5}` mean "that rig, but
///        turned down" rather than "that rig, with every other control reset".
inline saturation::AmpSimConfig amp_sim_config(const ParamMap& params,
                                               saturation::AmpSimConfig base = {}) {
  // The rig, numerically. `preset` names one as a string on the JSON
  // side-channel, which the insert factory resolves into `base` before calling
  // here — but that channel is C++-only, so the flat list every binding and the
  // CLI speak had no way to choose an amplifier at all, only to turn the knobs
  // of whichever one it was given. The index is `amp_preset_names()`'s order.
  const int preset = i(params, "presetIndex", -1, kNone);
  saturation::AmpSimConfig config =
      preset >= 0 && preset < static_cast<int>(saturation::amp_preset_names().size())
          ? saturation::amp_preset_config(static_cast<saturation::AmpPreset>(preset))
          : base;
  config.drive = f(params, "drive", config.drive, kNone);
  config.bass_db = f(params, "bassDb", config.bass_db, kDb);
  config.mid_db = f(params, "midDb", config.mid_db, kDb);
  config.treble_db = f(params, "trebleDb", config.treble_db, kDb);
  config.presence_db = f(params, "presenceDb", config.presence_db, kDb);
  config.cab = b(params, "cab", config.cab);
  config.cab_model = read_enum(params, "cabModel", config.cab_model);
  config.amp_model = read_enum(params, "ampModel", config.amp_model);
  config.input_db = f(params, "inputDb", config.input_db, kDb);
  config.level_db = f(params, "levelDb", config.level_db, kDb);
  config.power = f(params, "power", config.power, kNone);
  config.sag = f(params, "sag", config.sag, kNone);
  config.transformer = f(params, "transformer", config.transformer, kNone);
  config.nfb = f(params, "nfb", config.nfb, kNone);
  config.mic_model = read_enum(params, "micModel", config.mic_model);
  config.mic_axis = f(params, "micAxis", config.mic_axis, kNone);
  config.mic_distance_cm = f(params, "micDistanceCm", config.mic_distance_cm, kCentimeters);
  config.mic_blend = f(params, "micBlend", config.mic_blend, kNone);
  config.mic_b_model = read_enum(params, "micBModel", config.mic_b_model);
  config.mic_b_axis = f(params, "micBAxis", config.mic_b_axis, kNone);
  config.mic_b_distance_cm = f(params, "micBDistanceCm", config.mic_b_distance_cm, kCentimeters);
  config.mic_b_invert = b(params, "micBInvert", config.mic_b_invert);
  config.cone = f(params, "cone", config.cone, kNone);
  config.doppler = f(params, "doppler", config.doppler, kNone);
  config.topology = read_enum(params, "topology", config.topology);
  config.preamp_stages = i(params, "preampStages", config.preamp_stages, kCount);
  config.bias_shift = f(params, "biasShift", config.bias_shift, kNone);
  config.crossover = f(params, "crossover", config.crossover, kNone);
  config.power_tube = read_enum(params, "powerTube", config.power_tube);
  return config;
}

// ---------------------------------------------------------------------------
// Spectral
// ---------------------------------------------------------------------------

inline spectral::AirBandConfig air_band_config(const ParamMap& params) {
  spectral::AirBandConfig config;
  SONARE_FIELDS_AIR_BAND(SONARE_READ_FIELD)
  return config;
}

inline spectral::LowEndFocusConfig low_end_focus_config(const ParamMap& params) {
  spectral::LowEndFocusConfig config;
  SONARE_FIELDS_LOW_END_FOCUS(SONARE_READ_FIELD)
  return config;
}

inline spectral::PresenceEnhancerConfig presence_enhancer_config(const ParamMap& params) {
  spectral::PresenceEnhancerConfig config;
  SONARE_FIELDS_PRESENCE_ENHANCER(SONARE_READ_FIELD)
  return config;
}

inline spectral::SpectralShaperConfig spectral_shaper_config(const ParamMap& params) {
  spectral::SpectralShaperConfig config;
  SONARE_FIELDS_SPECTRAL_SHAPER(SONARE_READ_FIELD)
  note_pair_order(params, "frequencyHz", Relation::Lt, "highFrequencyHz");
  return config;
}

// ---------------------------------------------------------------------------
// Stereo
// ---------------------------------------------------------------------------

inline stereo::AutoPanConfig auto_pan_config(const ParamMap& params) {
  stereo::AutoPanConfig config;
  SONARE_FIELDS_AUTO_PAN(SONARE_READ_FIELD)
  return config;
}

inline stereo::BinauralPannerConfig binaural_panner_config(const ParamMap& params) {
  stereo::BinauralPannerConfig config;
  SONARE_FIELDS_BINAURAL_PANNER(SONARE_READ_FIELD)
  return config;
}

inline stereo::HaasEnhancerConfig haas_enhancer_config(const ParamMap& params) {
  stereo::HaasEnhancerConfig config;
  SONARE_FIELDS_HAAS_ENHANCER(SONARE_READ_FIELD)
  return config;
}

inline stereo::ImagerConfig imager_config(const ParamMap& params) {
  stereo::ImagerConfig config;
  SONARE_FIELDS_IMAGER(SONARE_READ_FIELD)
  SONARE_CHECK_RANGE("stereo.imager.width", config.width, 0.0f, 2.0f);
  SONARE_CHECK_RANGE("stereo.imager.decorrelationAmount", config.decorrelation_amount, 0.0f, 1.0f);
  return config;
}

inline stereo::MonoMakerConfig mono_maker_config(const ParamMap& params) {
  stereo::MonoMakerConfig config;
  SONARE_FIELDS_MONO_MAKER(SONARE_READ_FIELD)
  return config;
}

inline stereo::PhaseAlignConfig phase_align_config(const ParamMap& params) {
  stereo::PhaseAlignConfig config;
  SONARE_FIELDS_PHASE_ALIGN(SONARE_READ_FIELD)
  return config;
}

inline stereo::StereoBalanceConfig stereo_balance_config(const ParamMap& params) {
  stereo::StereoBalanceConfig config;
  SONARE_FIELDS_STEREO_BALANCE(SONARE_READ_FIELD)
  return config;
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

inline utility::GainConfig gain_config(const ParamMap& params) {
  utility::GainConfig config;
  SONARE_FIELDS_GAIN(SONARE_READ_FIELD)
  return config;
}

// ---------------------------------------------------------------------------
// Maximizer
// ---------------------------------------------------------------------------

inline maximizer::MaximizerConfig maximizer_config(const ParamMap& params) {
  maximizer::MaximizerConfig config;
  SONARE_FIELDS_MAXIMIZER(SONARE_READ_FIELD)
  return config;
}

inline maximizer::TruePeakLimiterConfig true_peak_limiter_config(const ParamMap& params) {
  maximizer::TruePeakLimiterConfig config;
  SONARE_FIELDS_TRUE_PEAK_LIMITER(SONARE_READ_FIELD)
  return config;
}

inline maximizer::SoftKneeMaxConfig soft_knee_max_config(const ParamMap& params) {
  maximizer::SoftKneeMaxConfig config;
  SONARE_FIELDS_SOFT_KNEE_MAX(SONARE_READ_FIELD)
  return config;
}

inline maximizer::AdaptiveReleaseConfig adaptive_release_config(const ParamMap& params) {
  maximizer::AdaptiveReleaseConfig config;
  SONARE_FIELDS_ADAPTIVE_RELEASE(SONARE_READ_FIELD)
  note_pair_order(params, "minReleaseMs", Relation::Le, "maxReleaseMs");
  note_pair_order(params, "crestLow", Relation::Le, "crestHigh");
  return config;
}

// ---------------------------------------------------------------------------
// Repair
//
// Each builder validates what it read, so a value the stage would refuse is
// refused where it is first accepted, on the named path and in the catalog's
// measurement alike.
// ---------------------------------------------------------------------------

inline repair::DeclickConfig declick_config(const ParamMap& params) {
  repair::DeclickConfig config;
  SONARE_FIELDS_DECLICK(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DeclipConfig declip_config(const ParamMap& params) {
  repair::DeclipConfig config;
  SONARE_FIELDS_DECLIP(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DecrackleConfig decrackle_config(const ParamMap& params) {
  repair::DecrackleConfig config;
  SONARE_FIELDS_DECRACKLE(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DehumConfig dehum_config(const ParamMap& params) {
  repair::DehumConfig config;
  SONARE_FIELDS_DEHUM(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

inline repair::DenoiseClassicalConfig denoise_classical_config(const ParamMap& params) {
  repair::DenoiseClassicalConfig config;
  SONARE_FIELDS_DENOISE_CLASSICAL(SONARE_READ_FIELD)
  note_pair_order(params, "hopLength", Relation::Le, "nFft");
  repair::validate_config(config);
  return config;
}

inline repair::DereverbClassicalConfig dereverb_classical_config(const ParamMap& params) {
  repair::DereverbClassicalConfig config;
  SONARE_FIELDS_DEREVERB_CLASSICAL(SONARE_READ_FIELD)
  note_pair_order(params, "hopLength", Relation::Le, "nFft");
  repair::validate_config(config);
  return config;
}

inline repair::TrimSilenceConfig trim_silence_config(const ParamMap& params) {
  repair::TrimSilenceConfig config;
  SONARE_FIELDS_TRIM_SILENCE(SONARE_READ_FIELD)
  repair::validate_config(config);
  return config;
}

// ---------------------------------------------------------------------------
// Table coverage
//
// One assertion per field table, checked against the config struct the table
// builds. Adding a field to any config below without adding its row fails the
// build here rather than shipping a parameter no binding can reach. See
// SONARE_ASSERT_TABLE_COVERS in param_field_tables.h for the contract; the
// two chain-only tables are asserted in chain_params.cpp, where their stage
// structs are visible.
//
// Every count is currently zero: each table exposes its whole config.
// ---------------------------------------------------------------------------

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_COMPRESSOR, dynamics::CompressorConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_LIMITER, dynamics::LimiterConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_BRICKWALL_LIMITER, dynamics::BrickwallLimiterConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEESSER, dynamics::DeEsserConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_EXPANDER, dynamics::ExpanderConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_GATE, dynamics::GateConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_PARALLEL_COMP, dynamics::ParallelCompConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_SIDECHAIN_ROUTER, dynamics::SidechainRouterConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DUCKING, dynamics::DuckingConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TRANSIENT_SHAPER, dynamics::TransientShaperConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_UPWARD_COMPRESSOR, dynamics::UpwardCompressorConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_UPWARD_EXPANDER, dynamics::UpwardExpanderConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_VOCAL_RIDER, dynamics::VocalRiderConfig, 0);

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TAPE, saturation::TapeConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_EXCITER, saturation::ExciterConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_BITCRUSHER, saturation::BitCrusherConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_HARD_CLIPPER, saturation::HardClipperConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_SOFT_CLIPPER, saturation::SoftClipperConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_WAVESHAPER, saturation::WaveshaperConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TUBE, saturation::TubeConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TRANSFORMER, saturation::TransformerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_PEDAL, saturation::OverdriveConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_PEDAL, saturation::DistortionConfig, 0);

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_AIR_BAND, spectral::AirBandConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_LOW_END_FOCUS, spectral::LowEndFocusConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_PRESENCE_ENHANCER, spectral::PresenceEnhancerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_SPECTRAL_SHAPER, spectral::SpectralShaperConfig, 0);

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_AUTO_PAN, stereo::AutoPanConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_BINAURAL_PANNER, stereo::BinauralPannerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_HAAS_ENHANCER, stereo::HaasEnhancerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_IMAGER, stereo::ImagerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_MONO_MAKER, stereo::MonoMakerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_PHASE_ALIGN, stereo::PhaseAlignConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_STEREO_BALANCE, stereo::StereoBalanceConfig, 0);

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_GAIN, utility::GainConfig, 0);

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_MAXIMIZER, maximizer::MaximizerConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TRUE_PEAK_LIMITER, maximizer::TruePeakLimiterConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_SOFT_KNEE_MAX, maximizer::SoftKneeMaxConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_ADAPTIVE_RELEASE, maximizer::AdaptiveReleaseConfig, 0);

SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECLICK, repair::DeclickConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECLIP, repair::DeclipConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DECRACKLE, repair::DecrackleConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEHUM, repair::DehumConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DENOISE_CLASSICAL, repair::DenoiseClassicalConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_DEREVERB_CLASSICAL, repair::DereverbClassicalConfig, 0);
SONARE_ASSERT_TABLE_COVERS(SONARE_FIELDS_TRIM_SILENCE, repair::TrimSilenceConfig, 0);

}  // namespace sonare::mastering::api::detail
