// Parameter, slot and chain catalogs: measured descriptors of every insert and chain key.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_construction.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/legacy_ids.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/processor_params.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
#include "util/number_format.h"

namespace sonare::mastering::api {

using detail::build_insert;
using detail::ParamKind;
using detail::ParamMap;

namespace {

// Every key an empty build read or declared, sorted. A band's keys are read only
// while the band is active, so an empty build declares them instead.
std::vector<std::string> construction_keys(const ParamMap& params) {
  std::unordered_set<std::string> keys(params.probed_keys().begin(), params.probed_keys().end());
  for (const auto& [key, kind] : params.probed_kinds()) keys.insert(key);
  std::vector<std::string> names(keys.begin(), keys.end());
  insertion_sort(names.begin(), names.end());
  return names;
}

}  // namespace

std::vector<std::string> insert_param_names(const std::string& requested_name) {
  const std::string name = canonical_processor_id(requested_name);
  // Build the processor against an empty param map: every config builder probes
  // or declares the keys it reads (falling back to defaults when absent), so
  // those are exactly the parameter names this processor consumes. The
  // throwaway processor is discarded immediately.
  ParamMap params;
  auto processor = build_insert(name, params);
  if (processor == nullptr) {
    return {};
  }
  return construction_keys(params);
}

namespace {

// Reads and validates the configuration of repair stage @p name from @p params; false when
// @p name is no repair stage. A repair stage is not an insert, so the catalog measures this
// builder where it would otherwise measure an insert's construction.
bool read_repair_config(const std::string& name, const ParamMap& params) {
  if (name == "repair.declick") {
    (void)detail::declick_config(params);
  } else if (name == "repair.declip") {
    (void)detail::declip_config(params);
  } else if (name == "repair.decrackle") {
    (void)detail::decrackle_config(params);
  } else if (name == "repair.dehum") {
    (void)detail::dehum_config(params);
  } else if (name == "repair.denoiseClassical") {
    (void)detail::denoise_classical_config(params);
  } else if (name == "repair.dereverbClassical") {
    (void)detail::dereverb_classical_config(params);
  } else if (name == "repair.trimSilence") {
    (void)detail::trim_silence_config(params);
  } else {
    return false;
  }
  return true;
}

// Set while the offline descriptor of a repair stage is measured. A stage with an insert form
// is otherwise measured as that insert, which refuses the offline-only settings.
thread_local bool measuring_offline_repair = false;

class OfflineRepairMeasurement {
 public:
  OfflineRepairMeasurement() : previous_(measuring_offline_repair) {
    measuring_offline_repair = true;
  }
  ~OfflineRepairMeasurement() { measuring_offline_repair = previous_; }
  OfflineRepairMeasurement(const OfflineRepairMeasurement&) = delete;
  OfflineRepairMeasurement& operator=(const OfflineRepairMeasurement&) = delete;

 private:
  bool previous_;
};

// A flat chain stage: its key prefix and the catalog processor its keys are, or nullptr.
struct ChainStage {
  const char* prefix;
  const char* processor;
};

// In execution order. The multiband prefix covers the crossover; its bands are kChainBandList.
constexpr ChainStage kChainStages[] = {
    {"repair.declick", "repair.declick"},
    {"repair.declip", "repair.declip"},
    {"repair.decrackle", "repair.decrackle"},
    {"repair.dehum", "repair.dehum"},
    {"repair.dereverb", "repair.dereverbClassical"},
    {"repair.denoise", "repair.denoiseClassical"},
    {"eq.tilt", "eq.tilt"},
    {"dynamics.deesser", "dynamics.deesser"},
    {"dynamics.transientShaper", "dynamics.transientShaper"},
    {"dynamics.compressor", "dynamics.compressor"},
    {"dynamics.multibandComp", "multiband.compressor"},
    {"saturation.tape", "saturation.tape"},
    {"saturation.exciter", "saturation.exciter"},
    {"spectral.airBand", "spectral.airBand"},
    {"stereo.imager", "stereo.imager"},
    {"stereo.monoMaker", "stereo.monoMaker"},
    {"maximizer.truePeakLimiter", "maximizer.truePeakLimiter"},
    {"loudness", nullptr},
};

// A multiband band is the chain compressor's fields at any index, past the insert's nine bands.
constexpr const char* kChainBandList = "dynamics.multibandComp.bands";
constexpr const char* kChainBandProcessor = "dynamics.compressor";
constexpr const char* kChainCutoffList = "dynamics.multibandComp.crossover.cutoffsHz";

// The name the chain-only keys are measured under; no processor id starts with '#'.
constexpr const char* kChainOnlyProbe = "#chain";

std::string chain_enabled_key(const ChainStage& stage) {
  return std::string(stage.prefix) + ".enabled";
}

// Declares the flat chain keys no processor describes and applies @p params through the chain's
// own parser and validator, so a measured bound is the one the chain refuses past.
bool read_chain_only_config(const ParamMap& params) {
  const MasteringChainConfig defaults;
  std::vector<Param> supplied;
  const auto supply_if_given = [&](const std::string& key, const std::string& spelled,
                                   double value) {
    if (params.find(key) != params.end()) supplied.push_back(Param{spelled, value});
  };
  for (const ChainStage& stage : kChainStages) {
    const std::string key = chain_enabled_key(stage);
    bool enabled = chain_config_parameter_value(defaults, key).value_or(0.0) != 0.0;
    detail::read_field(params, key.c_str(), enabled);
    supply_if_given(key, key, enabled ? 1.0 : 0.0);
  }
  // The field table names its metadata as detail does.
  using detail::kDb;
  using detail::kDbfs;
  using detail::kLufs;
  using detail::kMsLog;
  using detail::kNone;
  using detail::kRatio;
#define X(jkey, member, meta)                                                            \
  {                                                                                      \
    auto field = defaults.loudness.member;                                               \
    detail::read_field(params, "loudness." jkey, field, meta);                           \
    supply_if_given("loudness." jkey, "loudness." jkey, detail::field_as_double(field)); \
  }
  SONARE_FIELDS_LOUDNESS(X)
#undef X
  // The chain caps the kernel at kMaxFirKernelSize, which the multiband insert does not.
  const std::string fir_key = "dynamics.multibandComp.firKernelSize";
  int fir_kernel_size = defaults.dynamics.multiband_comp.config.crossover.fir_kernel_size;
  detail::read_field(params, fir_key.c_str(), fir_kernel_size, detail::kSamples);
  supply_if_given(fir_key, fir_key, fir_kernel_size);
  // A cutoff element and the legacy linear floor have no single default: declared, not read.
  const std::pair<std::string, detail::ParamMeta> undefaulted[] = {
      {kChainCutoffList, detail::kHzLog}, {"repair.denoise.gainFloor", detail::kNone}};
  for (const auto& [key, meta] : undefaulted) {
    detail::declare_number(params, key, meta);
    const auto given = params.find(key);
    if (given == params.end()) continue;
    supplied.push_back(Param{key == kChainCutoffList ? key + ".0" : key, given->second});
  }
  validate_mastering_chain_config(parse_chain_config_params(supplied.data(), supplied.size()));
  return true;
}

// Whether @p name, an insert or a repair stage, constructs from @p params. Throws what
// construction throws.
bool constructs(const std::string& name, const ParamMap& params) {
  if (name == kChainOnlyProbe) return read_chain_only_config(params);
  if (measuring_offline_repair) return read_repair_config(name, params);
  return build_insert(name, params) != nullptr || read_repair_config(name, params);
}

// What an insert's empty build declares, memoized because every probe of the
// catalog's measurement asks for it.
const ParamMap& empty_build(const std::string& name) {
  static thread_local std::unordered_map<std::string, ParamMap> memo;
  const std::string key = measuring_offline_repair ? name + "#offline" : name;
  const auto cached = memo.find(key);
  if (cached != memo.end()) return cached->second;
  ParamMap params;
  (void)constructs(name, params);
  return memo.emplace(key, std::move(params)).first->second;
}

using SlotList = std::vector<std::pair<std::string, detail::SlotDeclaration>>;

const std::pair<std::string, detail::SlotDeclaration>* find_slot(const SlotList& slots,
                                                                 const std::string& name) {
  for (const auto& slot : slots) {
    if (slot.first == name) return &slot;
  }
  return nullptr;
}

// The innermost slot @p key belongs to: the longest declared name it extends
// by a dot. Membership is by prefix because a slot's keys are built as
// `<slot>.<field>`.
const std::pair<std::string, detail::SlotDeclaration>* slot_of(const SlotList& slots,
                                                               const std::string& key) {
  const std::pair<std::string, detail::SlotDeclaration>* best = nullptr;
  for (const auto& slot : slots) {
    const std::string& prefix = slot.first;
    if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0 &&
        key[prefix.size()] == '.' && (best == nullptr || prefix.size() > best->first.size())) {
      best = &slot;
    }
  }
  return best;
}

// Cutoffs the crossover must have for @p key's slot, and each enclosing one, to exist.
int required_crossover_cutoffs(const SlotList& slots, const std::string& key) {
  int required = 0;
  for (const auto* slot = slot_of(slots, key); slot != nullptr;
       slot = slot->second.parent.empty() ? nullptr : find_slot(slots, slot->second.parent)) {
    required = std::max(required, slot->second.min_crossover_cutoffs);
  }
  return required;
}

}  // namespace

std::vector<Param> insert_probe_params(const std::string& requested_name, const std::string& key,
                                       double value) {
  const std::string name = canonical_processor_id(requested_name);
  std::vector<Param> params{Param{key, value}};
  if (name == "effects.gsEfx") {
    // TYPE is a paired selector, so a single-key probe gets the other byte at zero.
    if (key == "typeMsb") params.push_back(Param{"typeLsb", 0.0});
    if (key == "typeLsb") params.push_back(Param{"typeMsb", 0.0});
  }
  // A presence-gated slot exists once @p key itself is supplied. A crossover
  // band past the default split needs more cutoffs: the default ones as they
  // are, then the rest spread log-evenly above the last of them.
  const ParamMap& declared = empty_build(name);
  const int required = required_crossover_cutoffs(declared.declared_slots(), key);
  const std::vector<float> defaults = multiband::CrossoverConfig{}.cutoffs_hz;
  const int default_count = static_cast<int>(defaults.size());
  if (required > default_count) {
    constexpr double kHighestCutoffHz = 16000.0;
    const double last_default = defaults.empty() ? 20.0 : static_cast<double>(defaults.back());
    const int extra = required - default_count;
    for (int index = 0; index < required; ++index) {
      const double cutoff =
          index < default_count
              ? static_cast<double>(defaults[static_cast<size_t>(index)])
              : last_default * std::pow(kHighestCutoffHz / last_default,
                                        static_cast<double>(index - default_count + 1) / extra);
      params.push_back(Param{"cutoff" + std::to_string(index) + "Hz", cutoff});
    }
  }
  return params;
}

namespace {

// ---------------------------------------------------------------------------
// Parameter bound measurement
//
// Validation is hand-written per processor as a throw out of the config
// constructor, with no declared bounds interface, so the catalog MEASURES the
// bounds rather than mirroring them — the same discipline as the synth's clamp
// probe. A candidate goes through `build_insert` with every other parameter at
// its default, and whether construction throws is the answer. What is published
// is a hard constraint (outside it is an error, not a clip), not a recommended
// UI range: an unvalidated gain reports as unbounded.
//
// Measuring at the DEFAULT configuration makes a bound conservative rather than
// wrong: a validator coupling two parameters yields the interval one accepts
// while the other sits at its default (`maximizer.adaptiveRelease`), and a
// sample-rate-derived bound reflects the pre-prepare rate (the EQ band ceiling
// reads 24 kHz and rises once prepared higher). The siblings that move a bound beyond what a
// `dependsOn` relation states are published as `coupledWith`.
//
// Measuring rather than declaring costs three things. The probe visits
// |value| <= kBoundProbeLimit, and anything beyond reports null, which reads the
// same as "no validation". A boundary is bisected and rounded, so an EXCLUSIVE
// bound publishes its limit value — a `> 0` validator reports `min: 0` and still
// rejects 0 — which is why each published bound is probed once more and
// flagged exclusive when it is itself rejected. Parameters the config builder
// never reads, and booleans, are not probed at all.
// ---------------------------------------------------------------------------

constexpr double kBoundProbeLimit = 1.0e6;
constexpr double kBoundZeroSnap = 1.0e-9;
constexpr double kBoundAbsoluteTolerance = 1.0e-12;
constexpr double kBoundRelativeTolerance = 1.0e-9;
constexpr int kBoundSignificantDigits = 6;
constexpr int kBoundMaxBisections = 64;

// Probe points, ascending. Decade-spaced so a boundary anywhere in the window is
// bracketed by two neighbours, with the sub-unit decades filled in because most
// audio parameters that ARE bounded are bounded at 0, 1 or 2.
const std::vector<double>& bound_probe_points() {
  static const std::vector<double> points = [] {
    static constexpr double kMagnitudes[] = {
        1.0e-6, 1.0e-3, 1.0e-2, 1.0e-1, 1.0, 10.0, 100.0, 1.0e3, 1.0e4, 1.0e5, kBoundProbeLimit};
    std::vector<double> values;
    values.reserve(2 * std::size(kMagnitudes) + 1);
    for (size_t index = std::size(kMagnitudes); index > 0; --index) {
      values.push_back(-kMagnitudes[index - 1]);
    }
    values.push_back(0.0);
    for (const double magnitude : kMagnitudes) values.push_back(magnitude);
    return values;
  }();
  return points;
}

ParamMap probe_map(const std::string& name, const std::string& key, double value,
                   const std::vector<Param>& released = {}) {
  std::vector<Param> list = insert_probe_params(name, key, value);
  list.insert(list.end(), released.begin(), released.end());
  ParamMap probe = detail::make_map(list);
  probe.stop_recording_declarations();
  return probe;
}

// Whether a value is accepted, as measured through one construction path.
using AcceptFn = std::function<bool(double)>;

// Probe verdicts shared, while one insert is catalogued, by keys that differ only in their
// indices: `band3.q` builds with a value exactly when `band0.q` does, so each index-free shape
// is built once per value. A slow test holds every insert to the key-by-key measurement.
using SharedProbeVerdicts = std::unordered_map<std::string, bool>;
thread_local SharedProbeVerdicts* t_shared_probe_verdicts = nullptr;

class ScopedSharedProbeVerdicts {
 public:
  ScopedSharedProbeVerdicts() : previous_(t_shared_probe_verdicts) {
    t_shared_probe_verdicts = &verdicts_;
  }
  ~ScopedSharedProbeVerdicts() { t_shared_probe_verdicts = previous_; }
  ScopedSharedProbeVerdicts(const ScopedSharedProbeVerdicts&) = delete;
  ScopedSharedProbeVerdicts& operator=(const ScopedSharedProbeVerdicts&) = delete;

 private:
  SharedProbeVerdicts verdicts_;
  SharedProbeVerdicts* previous_;
};

// @p probe's verdict, or the one an earlier key of the same shape got for the same value and rate.
template <typename Probe>
bool shared_probe_verdict(const std::string& name, const std::string& key, double value,
                          double sample_rate, const Probe& probe) {
  if (t_shared_probe_verdicts == nullptr) return probe();
  std::string shape = name;
  shape += '\n';
  for (const char c : key) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      shape += c;
    } else if (shape.back() != '#') {
      shape += '#';
    }
  }
  shape += '\n';
  shape.append(reinterpret_cast<const char*>(&value), sizeof(value));
  shape.append(reinterpret_cast<const char*>(&sample_rate), sizeof(sample_rate));
  const auto found = t_shared_probe_verdicts->find(shape);
  if (found != t_shared_probe_verdicts->end()) return found->second;
  const bool verdict = probe();
  t_shared_probe_verdicts->emplace(std::move(shape), verdict);
  return verdict;
}

// Whether construction accepts @p value for @p key with every other parameter
// at its default. An unknown name yields no processor and so accepts nothing,
// which keeps a caller from measuring bounds against a processor that does not
// exist in this build configuration.
bool insert_accepts(const std::string& name, const std::string& key, double value,
                    const std::vector<Param>& released = {}) {
  const auto probe = [&] {
    try {
      return constructs(name, probe_map(name, key, value, released));
    } catch (...) {
      return false;
    }
  };
  // Released siblings are named by index, so such a probe is the key's own.
  return released.empty() ? shared_probe_verdict(name, key, value, 0.0, probe) : probe();
}

// insert_accepts, then prepared: a processor may refuse a selection only once
// it knows its rate (a linear-phase EQ has no all-pass response to realize).
bool insert_accepts_at_rate(const std::string& name, const std::string& key, double value,
                            double sample_rate) {
  return shared_probe_verdict(name, key, value, sample_rate, [&] {
    try {
      const ParamMap probe = probe_map(name, key, value);
      if (name == kChainOnlyProbe) return read_chain_only_config(probe);
      if (measuring_offline_repair) return read_repair_config(name, probe);
      auto processor = build_insert(name, probe, nullptr, sample_rate);
      if (processor == nullptr) return read_repair_config(name, probe);
      processor->prepare(sample_rate, kInsertProbeBlockSize);
      return true;
    } catch (...) {
      return false;
    }
  });
}

bool insert_accepts_prepared(const std::string& name, const std::string& key, double value) {
  return insert_accepts_at_rate(name, key, value, kInsertProbeSampleRate);
}

// Narrows a bracket whose ends disagree down to the extreme value construction
// still accepts. Integer-valued parameters bisect over integers: the flat
// surface rounds the value before it reaches the field, so a real-valued
// midpoint would report a bound halfway between two accepted settings.
double bisect_accepted_boundary(const AcceptFn& accepts, double rejected, double accepted,
                                bool integer_valued) {
  if (integer_valued) {
    long long accepted_step = std::llround(accepted);
    long long rejected_step = std::llround(rejected);
    while (std::llabs(rejected_step - accepted_step) > 1) {
      const long long middle = accepted_step + (rejected_step - accepted_step) / 2;
      if (accepts(static_cast<double>(middle))) {
        accepted_step = middle;
      } else {
        rejected_step = middle;
      }
    }
    return static_cast<double>(accepted_step);
  }
  for (int step = 0; step < kBoundMaxBisections; ++step) {
    const double tolerance =
        std::max(kBoundAbsoluteTolerance, kBoundRelativeTolerance * std::fabs(accepted));
    if (std::fabs(accepted - rejected) <= tolerance) break;
    const double middle = 0.5 * (rejected + accepted);
    if (middle == rejected || middle == accepted) break;
    if (accepts(middle)) {
      accepted = middle;
    } else {
      rejected = middle;
    }
  }
  return accepted;
}

// Rounds the accepted end of a bisected bracket toward the accepted side, so a
// published bound is never beyond what construction accepts. The bisection
// residue and the config fields' float resolution are added back first: an
// accepted value a hair inside a limit that is itself a 6-digit number must
// publish that number, not the step below it.
double settle_measured_bound(double accepted, bool is_max) {
  if (std::fabs(accepted) < kBoundZeroSnap || !std::isfinite(accepted)) return 0.0;
  const double slack =
      std::max(kBoundAbsoluteTolerance,
               std::max(kBoundRelativeTolerance,
                        static_cast<double>(std::numeric_limits<float>::epsilon())) *
                   std::fabs(accepted));
  const double exponent = std::ceil(std::log10(std::fabs(accepted)));
  const double scale = std::pow(10.0, static_cast<double>(kBoundSignificantDigits) - exponent);
  if (!std::isfinite(scale) || scale == 0.0) return accepted;
  const double settled = is_max ? std::floor((accepted + slack) * scale) / scale
                                : std::ceil((accepted - slack) * scale) / scale;
  return std::fabs(settled) < kBoundZeroSnap ? 0.0 : settled;
}

// Bisects from @p rejected to @p accepted, settles the bound toward the accepted side and
// reports whether construction refuses the settled value itself.
double settle_side(const AcceptFn& accepts, double rejected, double accepted, bool integer_valued,
                   bool is_max, bool* exclusive) {
  const double bound = settle_measured_bound(
      bisect_accepted_boundary(accepts, rejected, accepted, integer_valued), is_max);
  *exclusive = !accepts(bound);
  return bound;
}

struct MeasuredBounds {
  bool has_min = false;
  double min = 0.0;
  bool min_exclusive = false;
  bool has_max = false;
  double max = 0.0;
  bool max_exclusive = false;
  bool max_follows_nyquist = false;
};

MeasuredBounds measure_bounds(const std::string& name, const std::string& key,
                              bool integer_valued) {
  const AcceptFn accepts = [&](double value) { return insert_accepts(name, key, value); };
  // The default is accepted by construction, so an accepted interval that sits between two
  // decades (a gate threshold in [-90, -12]) still has a probe point inside it.
  std::vector<double> points = bound_probe_points();
  size_t default_index = points.size();
  const auto declared_default = empty_build(name).probed_defaults().find(key);
  if (declared_default != empty_build(name).probed_defaults().end() &&
      !declared_default->second.ambiguous && std::isfinite(declared_default->second.value) &&
      std::fabs(declared_default->second.value) <= kBoundProbeLimit &&
      std::find(points.begin(), points.end(), declared_default->second.value) == points.end()) {
    const auto at = points.insert(
        std::upper_bound(points.begin(), points.end(), declared_default->second.value),
        declared_default->second.value);
    default_index = static_cast<size_t>(at - points.begin());
  }
  // Each side stops at the first probe point it accepts, so an unconstrained
  // parameter — the majority — costs exactly two builds: the window's two ends
  // both build and there is nothing to narrow. Walking inward is also what makes
  // the answer independent of any assumption about the accepted set's shape:
  // the extreme probe alone decides whether a bound is reported at all.
  size_t lowest_accepted = points.size();
  for (size_t index = 0; index < points.size(); ++index) {
    if (accepts(points[index])) {
      lowest_accepted = index;
      break;
    }
  }
  // Nothing in the window builds. That is not a bound, it is a processor this
  // key cannot configure at all, so the catalog states no limit rather than an
  // empty interval no host could satisfy.
  if (lowest_accepted == points.size()) return {};
  size_t highest_accepted = lowest_accepted;
  for (size_t index = points.size(); index > lowest_accepted; --index) {
    if (accepts(points[index - 1])) {
      highest_accepted = index - 1;
      break;
    }
  }

  MeasuredBounds bounds;
  if (lowest_accepted > 0) {
    bounds.has_min = true;
    bounds.min = settle_side(accepts, points[lowest_accepted - 1], points[lowest_accepted],
                             integer_valued, false, &bounds.min_exclusive);
  }
  if (highest_accepted + 1 < points.size()) {
    bounds.has_max = true;
    bounds.max = settle_side(accepts, points[highest_accepted + 1], points[highest_accepted],
                             integer_valued, true, &bounds.max_exclusive);
  }
  // Only the default builds, and bisecting from it finds no width: it is accepted because a
  // sibling sits at its own default, which bounds nothing, so the key is reported unbounded
  // as when no point builds at all.
  if (lowest_accepted == default_index && highest_accepted == default_index && bounds.has_min &&
      bounds.has_max && bounds.min >= bounds.max) {
    return {};
  }
  return bounds;
}

// Second rate the ceiling is measured at, to tell a ceiling that follows the
// processing rate's Nyquist from one that does not.
constexpr double kNyquistProbeSampleRate = 44100.0;
constexpr double kNyquistMatchTolerance = 1.0e-4;

// Whether the ceiling of @p key rises and falls with half the processing rate:
// prepared at a second rate, the largest accepted value lands on that rate's
// Nyquist. Only an exclusive ceiling can follow it, and the measured `max` is
// kept as the number at the probe rate.
bool max_follows_nyquist(const std::string& name, const std::string& key,
                         const MeasuredBounds& bounds) {
  if (!bounds.has_max || !bounds.max_exclusive) return false;
  const AcceptFn accepts = [&](double value) {
    return insert_accepts_at_rate(name, key, value, kNyquistProbeSampleRate);
  };
  const double inside = bounds.max * (1.0 - 1.0e-3);
  if (accepts(inside)) return false;
  const double accepted = bounds.has_min ? 0.5 * (bounds.min + bounds.max) : 0.5 * bounds.max;
  if (!accepts(accepted)) return false;
  const double nyquist = 0.5 * kNyquistProbeSampleRate;
  const double ceiling = bisect_accepted_boundary(accepts, inside, accepted, false);
  return std::fabs(ceiling - nyquist) <= kNyquistMatchTolerance * nyquist;
}

// Replaces the ceiling of a Nyquist-following key with the one accepted when the
// insert is built and prepared at @p sample_rate.
void measure_ceiling_at_rate(const std::string& name, const std::string& key, double sample_rate,
                             MeasuredBounds* bounds) {
  const AcceptFn accepts = [&](double value) {
    return insert_accepts_at_rate(name, key, value, sample_rate);
  };
  // The insert is built at @p sample_rate, so its ceiling is that rate's Nyquist, not the
  // rate-less probe cap in `bounds->max`.
  const double ceiling = 0.5 * sample_rate;
  const double rejected = std::max(bounds->max, ceiling) * (1.0 + 1.0e-3);
  const double accepted = bounds->has_min ? 0.5 * (bounds->min + ceiling) : 0.5 * ceiling;
  if (accepts(rejected) || !accepts(accepted)) return;
  bounds->max = settle_side(accepts, rejected, accepted, false, true, &bounds->max_exclusive);
}

// The declared values of an enum key that construction accepts.
std::vector<detail::EnumChoice> measure_enum_choices(
    const std::string& name, const std::string& key,
    const std::vector<detail::EnumChoice>& declared) {
  std::vector<detail::EnumChoice> accepted;
  for (const detail::EnumChoice& choice : declared) {
    if (insert_accepts_prepared(name, key, static_cast<double>(choice.value))) {
      accepted.push_back(choice);
    }
  }
  return accepted;
}

// Integers scanned above a whole-number key's measured minimum. Bisection stops
// at the first hole of a set such as {1, 2, 4, 8}, so the scan does not start
// from the measured maximum.
constexpr long long kIntegerChoiceScanWidth = 64;

// The accepted integers of @p key when they are a closed set with holes. An
// interval is left to the bounds; a set with holes still open at the top of the
// scan keeps its minimum only, since bisecting it for a maximum lands in a hole.
bool measure_integer_choices(const std::string& name, const std::string& key, double min,
                             std::vector<detail::EnumChoice>* out, bool* open_with_holes) {
  const long long first = std::llround(min);
  std::vector<long long> accepted;
  bool top_accepted = false;
  for (long long value = first; value <= first + kIntegerChoiceScanWidth; ++value) {
    top_accepted = insert_accepts(name, key, static_cast<double>(value));
    if (top_accepted) accepted.push_back(value);
  }
  if (accepted.empty()) return false;
  const bool contiguous =
      accepted.back() - accepted.front() + 1 == static_cast<long long>(accepted.size());
  if (contiguous) return false;
  if (top_accepted) {
    *open_with_holes = true;
    return false;
  }
  out->clear();
  for (const long long value : accepted) {
    out->push_back(detail::EnumChoice{std::to_string(value), static_cast<int>(value)});
  }
  return true;
}

// Largest power of two scanned for a size-like key. A set that still accepts it has no closed
// top to list, and is left to its bounds.
constexpr int kMaxPowerOfTwoExponent = 20;

// The accepted powers of two of a whole-number key whose default is one and whose
// neighbours are all refused. Every accepted power of two is checked against the
// integers beside it, so a key that merely accepts a range containing powers of two
// is left to its bounds. @p released puts the siblings that bound the key from below at 1, so
// the list holds every size the stage takes at some valid sibling value.
bool measure_power_of_two_choices(const std::string& name, const std::string& key, double fallback,
                                  const std::vector<Param>& released,
                                  std::vector<detail::EnumChoice>* out) {
  const auto accepts = [&](double value) { return insert_accepts(name, key, value, released); };
  const long long start = std::llround(fallback);
  if (start < 2 || (start & (start - 1)) != 0) return false;
  if (accepts(static_cast<double>(start + 1)) || accepts(static_cast<double>(start - 1))) {
    return false;
  }
  out->clear();
  for (int exponent = 0; exponent <= kMaxPowerOfTwoExponent; ++exponent) {
    const long long value = 1LL << exponent;
    if (!accepts(static_cast<double>(value))) continue;
    if (exponent == kMaxPowerOfTwoExponent) return false;
    if (value > 2 &&
        (accepts(static_cast<double>(value + 1)) || accepts(static_cast<double>(value - 1)) ||
         accepts(static_cast<double>(value + value / 2)))) {
      return false;
    }
    out->push_back(detail::EnumChoice{std::to_string(value), static_cast<int>(value)});
  }
  return out->size() > 1;
}

// The siblings that bound @p key from below (`key ge/gt factor * sibling`), each at 1.
std::vector<Param> lower_bounding_siblings_at_one(const ParamMap& params, const std::string& key) {
  std::vector<Param> released;
  const auto depends = params.probed_depends().find(key);
  if (depends == params.probed_depends().end()) return released;
  for (const auto& dependency : depends->second) {
    if (dependency.relation == detail::Relation::Ge ||
        dependency.relation == detail::Relation::Gt) {
      released.push_back(Param{dependency.key, 1.0});
    }
  }
  return released;
}

// Renders a catalog number as JSON text. The precision is the shortest that
// round-trips through `float` — the storage nearly every mastering config field
// uses — widened so a value with an integer part never comes out in exponent
// form. Formatting is locale-independent because a host that switched
// LC_NUMERIC would otherwise emit a decimal comma into a JSON document.
std::string format_catalog_number(double value) {
  if (!std::isfinite(value)) return "null";
  int integer_digits = 1;
  const double magnitude = std::fabs(value);
  if (magnitude >= 1.0) {
    integer_digits = static_cast<int>(std::floor(std::log10(magnitude))) + 1;
  }
  std::string widest;
  for (int precision = 1; precision <= std::numeric_limits<double>::max_digits10; ++precision) {
    widest = sonare::util::format_general(value, std::max(precision, integer_digits));
    double round_trip = 0.0;
    if (!sonare::util::parse_double(widest.data(), widest.data() + widest.size(), &round_trip)) {
      continue;
    }
    if (static_cast<float>(round_trip) == static_cast<float>(value)) return widest;
  }
  return widest;
}

const char* catalog_type(ParamKind kind, bool is_enum) {
  switch (kind) {
    case ParamKind::Boolean:
      return "boolean";
    case ParamKind::String:
      return "string";
    case ParamKind::Array:
      return "array";
    case ParamKind::Integer:
    case ParamKind::Number:
      break;
  }
  return is_enum ? "enum" : "number";
}

// A bound that is the sibling's default scaled by the declared factor is that dependency read at
// the sibling's default, not a limit of the key's own; dependsOn carries it live, so it is not
// published.
void drop_dependency_bounds(const ParamMap& params, const std::string& key,
                            MeasuredBounds* bounds) {
  const auto depends = params.probed_depends().find(key);
  if (depends == params.probed_depends().end()) return;
  constexpr double kRestatesTolerance = 1.0e-9;
  for (const auto& dependency : depends->second) {
    const auto sibling = params.probed_defaults().find(dependency.key);
    if (sibling == params.probed_defaults().end() || sibling->second.ambiguous) continue;
    const double at_default = dependency.factor * sibling->second.value;
    const bool upper =
        dependency.relation == detail::Relation::Le || dependency.relation == detail::Relation::Lt;
    if (upper && bounds->has_max && std::fabs(bounds->max - at_default) <= kRestatesTolerance) {
      bounds->has_max = false;
      bounds->max_exclusive = false;
    } else if (!upper && bounds->has_min &&
               std::fabs(bounds->min - at_default) <= kRestatesTolerance) {
      bounds->has_min = false;
      bounds->min_exclusive = false;
    }
  }
}

// A logarithmic axis needs a positive lower end and a finite top: the unit's floor and ceiling
// fill what the reader left undeclared, and a positive accepted minimum is already the floor.
void resolve_log_axis(detail::Unit unit, const MeasuredBounds& bounds, double* ui_min,
                      double* ui_max) {
  const bool minimum_is_floor = bounds.has_min && bounds.min > 0.0;
  if (std::isnan(*ui_min) && !minimum_is_floor) *ui_min = detail::log_axis_floor(unit);
  const double low = std::isnan(*ui_min) ? bounds.min : *ui_min;
  if (std::isnan(*ui_max) && !bounds.has_max) {
    *ui_max = std::max(detail::log_axis_ceiling(unit), low * 10.0);
  }
  const double top = std::isnan(*ui_max) ? bounds.max : *ui_max;
  if (!std::isnan(*ui_min) && *ui_min >= top) *ui_min = top / 1000.0;
}

// Appends one entry. @p id_json is the automation id, or "null" for a key only
// construction reads.
void append_param_entry(std::string& out, const std::string& name, const std::string& key,
                        const std::string& id_json, bool rt_safe, const ParamMap& params,
                        double sample_rate) {
  const auto& kinds = params.probed_kinds();
  const auto& defaults = params.probed_defaults();
  const auto* slot = slot_of(params.declared_slots(), key);
  const auto& probed = params.probed_keys();
  const auto& enum_choices = params.probed_choices();

  // A descriptor key the builder never probed has no declared field to read a
  // type from; those are numeric automation targets, which is also the
  // catalog's neutral value.
  const auto kind = kinds.find(key);
  const ParamKind param_kind = kind != kinds.end() ? kind->second : ParamKind::Number;
  const bool measurable = param_kind == ParamKind::Integer || param_kind == ParamKind::Number;
  // A declared band key is read once its band is active, which the probe arranges.
  const bool construction_reads_key = probed.find(key) != probed.end() || kind != kinds.end();
  // GS TYPE accepts byte pairs, so a scalar probe cannot bound either byte.
  const bool coupled_gs_type_selector =
      name == "effects.gsEfx" && (key == "typeMsb" || key == "typeLsb");
  const auto declared = enum_choices.find(key);
  const bool is_enum = declared != enum_choices.end();

  bool has_choices = false;
  std::vector<detail::EnumChoice> choices;
  MeasuredBounds bounds;
  // The declaration the reader made beside the type and default; a key no
  // reader declared carries none, which the catalog test refuses for a number.
  const auto declared_meta = params.probed_meta().find(key);
  const bool has_meta = declared_meta != params.probed_meta().end();
  const detail::ParamMeta meta = has_meta ? declared_meta->second : detail::ParamMeta{};
  const bool unit_is_hz = has_meta && meta.unit == detail::Unit::Hz;
  if (construction_reads_key && measurable && !coupled_gs_type_selector) {
    if (is_enum) {
      choices = measure_enum_choices(name, key, declared->second);
      has_choices = true;
    } else {
      bounds = measure_bounds(name, key, param_kind == ParamKind::Integer);
      if (param_kind == ParamKind::Integer && bounds.has_min) {
        bool open_with_holes = false;
        has_choices = measure_integer_choices(name, key, bounds.min, &choices, &open_with_holes);
        if (open_with_holes) {
          bounds.has_max = false;
          bounds.max_exclusive = false;
        }
      }
      const auto fallback = defaults.find(key);
      if (param_kind == ParamKind::Integer && !has_choices && !bounds.has_min && !bounds.has_max &&
          fallback != defaults.end() && !fallback->second.ambiguous) {
        has_choices =
            measure_power_of_two_choices(name, key, fallback->second.value,
                                         lower_bounding_siblings_at_one(params, key), &choices);
      }
      if (!has_choices) drop_dependency_bounds(params, key, &bounds);
      if (unit_is_hz) bounds.max_follows_nyquist = max_follows_nyquist(name, key, bounds);
      if (bounds.max_follows_nyquist && sample_rate > 0.0) {
        measure_ceiling_at_rate(name, key, sample_rate, &bounds);
      }
    }
  }
  if (has_choices) bounds = MeasuredBounds{};

  out += "{\"name\":\"";
  out += key;
  out += "\",\"id\":";
  out += id_json;
  out += ",\"rtSafe\":";
  out += rt_safe ? "true" : "false";
  out += ",\"type\":\"";
  out += catalog_type(param_kind, is_enum);
  out += "\",\"min\":";
  out += bounds.has_min ? format_catalog_number(bounds.min) : "null";
  out += ",\"max\":";
  out += bounds.has_max ? format_catalog_number(bounds.max) : "null";
  out += ",\"minExclusive\":";
  out += bounds.min_exclusive ? "true" : "false";
  out += ",\"maxExclusive\":";
  out += bounds.max_exclusive ? "true" : "false";
  out += ",\"maxRelativeTo\":";
  out += bounds.max_follows_nyquist ? "\"nyquist\"" : "null";

  out += ",\"default\":";
  const auto fallback = defaults.find(key);
  if (!measurable && param_kind != ParamKind::Boolean) {
    out += "null";
  } else if (fallback == defaults.end() || fallback->second.ambiguous) {
    out += "null";
  } else if (param_kind == ParamKind::Boolean) {
    out += fallback->second.value != 0.0 ? "true" : "false";
  } else {
    out += format_catalog_number(fallback->second.value);
  }

  // A unit is a numeric parameter's declaration; a toggle, a selector or an array has none.
  const bool numeric_param = measurable && !is_enum;
  out += ",\"unit\":";
  if (numeric_param && has_meta) {
    out += '"';
    out += detail::unit_name(meta.unit);
    out += '"';
  } else {
    out += "null";
  }
  double ui_min = meta.ui_min;
  double ui_max = meta.ui_max;
  if (numeric_param && meta.scale == detail::Scale::Log) {
    resolve_log_axis(meta.unit, bounds, &ui_min, &ui_max);
  }
  out += ",\"uiMin\":";
  out += std::isnan(ui_min) ? "null" : format_catalog_number(ui_min);
  out += ",\"uiMax\":";
  out += std::isnan(ui_max) ? "null" : format_catalog_number(ui_max);
  out += ",\"scale\":\"";
  out += meta.scale == detail::Scale::Log ? "log" : "linear";
  out += '"';

  out += ",\"choices\":";
  if (!has_choices) {
    out += "null";
  } else {
    out += '[';
    for (size_t index = 0; index < choices.size(); ++index) {
      if (index > 0) out += ',';
      out += "{\"name\":\"";
      out += choices[index].name;
      out += "\",\"value\":";
      out += std::to_string(choices[index].value);
      out += '}';
    }
    out += ']';
  }

  out += ",\"slot\":";
  if (slot == nullptr) {
    out += "null";
  } else {
    out += '"';
    out += slot->first;
    out += '"';
  }

  out += ",\"dependsOn\":[";
  const auto depends = params.probed_depends().find(key);
  if (depends != params.probed_depends().end()) {
    bool first_dependency = true;
    for (const auto& dependency : depends->second) {
      if (!first_dependency) out += ',';
      first_dependency = false;
      out += "{\"key\":\"";
      out += dependency.key;
      out += "\",\"relation\":\"";
      out += detail::relation_name(dependency.relation);
      out += "\",\"factor\":";
      out += format_catalog_number(dependency.factor);
      out += '}';
    }
  }
  out += "],\"coupledWith\":[";
  const auto coupled = params.probed_coupled().find(key);
  if (coupled != params.probed_coupled().end()) {
    for (size_t index = 0; index < coupled->second.size(); ++index) {
      if (index > 0) out += ',';
      out += '"';
      out += coupled->second[index];
      out += '"';
    }
  }
  out += "]}";
}

// Appends an entry for each key only construction reads, except the keys in @p skip.
void append_construction_entries(std::string& out, const std::string& name, const ParamMap& params,
                                 double sample_rate,
                                 const std::unordered_set<std::string>* skip = nullptr) {
  for (const std::string& key : construction_keys(params)) {
    if (skip != nullptr && skip->find(key) != skip->end()) continue;
    if (out.size() > 1) out += ',';
    append_param_entry(out, name, key, "null", false, params, sample_rate);
  }
}

std::string build_insert_param_info_json(const std::string& name, double sample_rate,
                                         bool share_indexed_verdicts = true) {
  // Build a throwaway processor (like insert_param_names). Its descriptor table
  // gives the automation targets; the same build recorded every key the config
  // builder read, the C++ type it read it as (ParamMap::note_kind, driven by the
  // accessors and the declared type of the SONARE_FIELDS_* destination field),
  // its fallback (ParamMap::note_default, which against this empty map is the
  // config struct's own field initializer) and, for an enum, its declared
  // values. Reporting from those instead of from the key's spelling is what
  // keeps `type` honest.
  ParamMap params;
  auto processor = build_insert(name, params);
  if (processor == nullptr) return "[]";
  std::optional<ScopedSharedProbeVerdicts> shared_verdicts;
  if (share_indexed_verdicts) shared_verdicts.emplace();
  std::string out = "[";
  const auto descriptors = processor->parameter_descriptors();
  std::unordered_set<std::string> descriptor_keys;
  for (const auto& descriptor : descriptors) {
    if (out.size() > 1) out += ',';
    append_param_entry(out, name, descriptor.key, std::to_string(descriptor.id),
                       processor->parameter_is_realtime_safe(descriptor.id), params, sample_rate);
    descriptor_keys.insert(descriptor.key);
  }
  // Then the keys only construction reads, which take effect when the insert is built.
  append_construction_entries(out, name, params, sample_rate, &descriptor_keys);
  out += ']';
  return out;
}

}  // namespace

std::string repair_param_info_json(const std::string& name) {
  static thread_local std::unordered_map<std::string, std::string> memo;
  const auto cached = memo.find(name);
  if (cached != memo.end()) return cached->second;
  const OfflineRepairMeasurement offline;
  ParamMap params;
  if (!read_repair_config(name, params)) return "[]";
  std::string out = "[";
  append_construction_entries(out, name, params, 0.0);
  out += ']';
  return memo.emplace(name, std::move(out)).first->second;
}

namespace {

// The processor and key a flat chain key is read as. The chain's enum keys are those of the
// stage's own processor. Every spelling is canonicalized first, so `crossover.slope`,
// `bands.<i>.detector` and the scalar spellings resolve alike.
bool chain_key_processor(const std::string& chain_key, std::string* processor, std::string* key) {
  std::string canonical_storage;
  const std::string& canonical = canonical_chain_param_key(chain_key, &canonical_storage);
  // A band before its stage: the multiband prefix names the crossover's processor.
  const std::string band_prefix = "dynamics.multibandComp.band";
  const std::size_t dot = canonical.find('.', band_prefix.size());
  // The chain's index spelling: decimal digits, no leading zero.
  if (canonical.compare(0, band_prefix.size(), band_prefix) == 0 && dot != std::string::npos &&
      dot != band_prefix.size() &&
      canonical.find_first_not_of("0123456789", band_prefix.size()) == dot &&
      (canonical[band_prefix.size()] != '0' || dot == band_prefix.size() + 1)) {
    *processor = kChainBandProcessor;
    *key = canonical.substr(dot + 1);
    return true;
  }
  for (const ChainStage& stage : kChainStages) {
    const std::string stage_prefix = std::string(stage.prefix) + '.';
    if (canonical.compare(0, stage_prefix.size(), stage_prefix) != 0) continue;
    if (stage.processor == nullptr) return false;
    *processor = stage.processor;
    *key = canonical.substr(stage_prefix.size());
    return true;
  }
  return false;
}

}  // namespace

std::optional<double> mastering_enum_value(const std::string& processor, const std::string& key,
                                           const std::string& choice) {
  std::string id = canonical_processor_id(processor);
  std::string leaf = key;
  if (processor.empty() && !chain_key_processor(key, &id, &leaf)) return std::nullopt;
  const auto declared = empty_build(id).probed_choices().find(leaf);
  if (declared == empty_build(id).probed_choices().end()) return std::nullopt;
  const std::string current_choice = canonical_choice_name(leaf, choice);
  std::string valid;
  for (const detail::EnumChoice& entry : declared->second) {
    if (entry.name == current_choice) return static_cast<double>(entry.value);
    if (!valid.empty()) valid += ", ";
    valid += entry.name;
  }
  throw SonareException(ErrorCode::InvalidParameter,
                        key + ": unknown name '" + choice + "' (valid names: " + valid + ")");
}

namespace {

void append_json_string(std::string& out, const std::string& text) {
  out += '"';
  out += text;
  out += '"';
}

// `{key, aliases}` entries: each other spelling canonical_chain_param_key folds onto a key, with
// the list spellings canonical and `<i>` standing for an index.
std::vector<std::pair<std::string, std::string>> chain_aliases() {
  const std::string multiband = "dynamics.multibandComp.";
  std::vector<std::pair<std::string, std::string>> aliases = {
      {"repair.denoise.enabled", "repair.denoise"},
      {"repair.denoise.nFft", "repair.nFft"},
      {"repair.denoise.hopLength", "repair.hopLength"},
      {"repair.denoise.ddAlpha", "repair.ddAlpha"},
      {"repair.denoise.reductionDb", "repair.reductionDb"},
      {"repair.denoise.gainFloor", "repair.gainFloor"},
      {"eq.tilt.tiltDb", "eq.tiltDb"},
      {"eq.tilt.pivotHz", "eq.pivotHz"},
      {multiband + "slope", multiband + "crossover.slope"},
      {multiband + "mode", multiband + "crossover.mode"},
      {multiband + "firKernelSize", multiband + "crossover.firKernelSize"},
      {std::string(kChainCutoffList) + ".<i>", multiband + "cutoff<i>Hz"},
      {std::string(kChainCutoffList) + ".0", multiband + "lowCutoffHz"},
      {std::string(kChainCutoffList) + ".1", multiband + "highCutoffHz"},
  };
#define X(jkey, member, meta) \
  aliases.emplace_back(std::string(kChainBandList) + ".<i>." jkey, multiband + "band<i>." jkey);
  SONARE_FIELDS_COMPRESSOR(X)
#undef X
  // The shorthand of the default split's three bands.
  const char* const band_words[] = {"low", "mid", "high"};
  for (std::size_t band = 0; band < std::size(band_words); ++band) {
    for (const char* field : {"thresholdDb", "ratio", "attackMs", "releaseMs"}) {
      std::string shorthand = multiband + band_words[band] + field;
      const std::size_t capital = shorthand.size() - std::strlen(field);
      shorthand[capital] = static_cast<char>(std::toupper(static_cast<unsigned char>(field[0])));
      aliases.emplace_back(std::string(kChainBandList) + "." + std::to_string(band) + "." + field,
                           std::move(shorthand));
    }
  }
  return aliases;
}

std::string chain_only_param_info_json() {
  ParamMap params;
  (void)read_chain_only_config(params);
  std::string out = "[";
  append_construction_entries(out, kChainOnlyProbe, params, 0.0);
  out += ']';
  return out;
}

}  // namespace

std::string chain_catalog_json() {
  std::string out = "{\"stages\":[";
  for (const ChainStage& stage : kChainStages) {
    if (out.back() != '[') out += ',';
    out += "{\"prefix\":";
    append_json_string(out, stage.prefix);
    out += ",\"processor\":";
    if (stage.processor == nullptr) {
      out += "null";
    } else {
      append_json_string(out, stage.processor);
    }
    out += ",\"enabledKey\":";
    append_json_string(out, chain_enabled_key(stage));
    out += '}';
  }
  out += "],\"lists\":[{\"key\":";
  append_json_string(out, kChainCutoffList);
  out += ",\"processor\":null,\"minItems\":" + std::to_string(kMinMultibandBands - 1u) +
         ",\"maxItems\":" + std::to_string(kMaxMultibandBands - 1u) + "},{\"key\":";
  append_json_string(out, kChainBandList);
  out += ",\"processor\":";
  append_json_string(out, kChainBandProcessor);
  out += ",\"minItems\":" + std::to_string(kMinMultibandBands) +
         ",\"maxItems\":" + std::to_string(kMaxMultibandBands) + "}],\"params\":";
  out += chain_only_param_info_json();
  out += ",\"aliases\":[";
  for (const auto& [key, alias] : chain_aliases()) {
    if (out.back() != '[') out += ',';
    out += "{\"key\":";
    append_json_string(out, key);
    out += ",\"aliases\":[";
    append_json_string(out, alias);
    out += "]}";
  }
  out += "]}";
  return out;
}

namespace {

// An id that builds no insert may still be a repair stage, which has descriptors of its own.
std::string info_or_repair(std::string info, const std::string& name) {
  return info == "[]" ? repair_param_info_json(name) : std::move(info);
}

}  // namespace

std::string insert_slot_info_json(const std::string& requested_name) {
  const std::string name = canonical_processor_id(requested_name);
  // Same declarations the param entries' `slot` field is read from.
  std::string out = "[";
  for (const auto& [slot_name, declaration] : empty_build(name).declared_slots()) {
    if (out.size() > 1) out += ',';
    out += "{\"name\":\"";
    out += slot_name;
    out += "\",\"parent\":";
    if (declaration.parent.empty()) {
      out += "null";
    } else {
      out += '"';
      out += declaration.parent;
      out += '"';
    }
    out += ",\"activation\":\"";
    out += declaration.any_key ? "anyKey" : "always";
    out += "\",\"minCrossoverCutoffs\":";
    out += std::to_string(declaration.min_crossover_cutoffs);
    out += '}';
  }
  out += ']';
  return out;
}

std::string insert_param_info_json(const std::string& requested_name) {
  const std::string name = canonical_processor_id(requested_name);
  // Measuring the bounds costs one processor construction per probe point, so a
  // repeat query — and the capability catalog, which asks for every insert —
  // reads a memo instead of probing again. Thread-local because the C ABI's own
  // catalog buffers already are, and because the memo is pure derived data that
  // is cheaper to recompute per thread than to guard.
  static thread_local std::unordered_map<std::string, std::string> memo;
  const auto cached = memo.find(name);
  if (cached != memo.end()) return cached->second;
  return memo.emplace(name, info_or_repair(build_insert_param_info_json(name, 0.0), name))
      .first->second;
}

std::string insert_param_info_json_at_rate(const std::string& requested_name, double sample_rate) {
  const std::string name = canonical_processor_id(requested_name);
  // Keyed apart from the rate-less memo, so the rate-less answer never depends on
  // an earlier call.
  static thread_local std::unordered_map<std::string, std::string> memo;
  const std::string memo_key = name + '@' + std::to_string(sample_rate);
  const auto cached = memo.find(memo_key);
  if (cached != memo.end()) return cached->second;
  return memo
      .emplace(memo_key, info_or_repair(build_insert_param_info_json(name, sample_rate), name))
      .first->second;
}

std::string insert_param_info_json_measured_per_key(const std::string& requested_name,
                                                    double sample_rate) {
  const std::string name = canonical_processor_id(requested_name);
  return info_or_repair(
      build_insert_param_info_json(name, sample_rate, /*share_indexed_verdicts=*/false), name);
}

const std::vector<std::string>& insert_param_info_schema_paths() {
  static const std::vector<std::string> paths = {
      "[].name",
      "[].id",
      "[].rtSafe",
      "[].type",
      "[].min",
      "[].max",
      "[].minExclusive",
      "[].maxExclusive",
      "[].maxRelativeTo",
      "[].default",
      "[].unit",
      "[].uiMin",
      "[].uiMax",
      "[].scale",
      "[].choices",
      "[].choices[].name",
      "[].choices[].value",
      "[].slot",
      "[].dependsOn",
      "[].dependsOn[].key",
      "[].dependsOn[].relation",
      "[].dependsOn[].factor",
      "[].coupledWith",
  };
  return paths;
}

const std::vector<std::string>& insert_slot_info_schema_paths() {
  static const std::vector<std::string> paths = {
      "[].name",
      "[].parent",
      "[].activation",
      "[].minCrossoverCutoffs",
  };
  return paths;
}

}  // namespace sonare::mastering::api
