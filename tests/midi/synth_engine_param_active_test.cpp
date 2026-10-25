/// @file synth_engine_param_active_test.cpp
/// @brief The `active_when` gates of the engine-section descriptors, measured
///        against renders. Each alternative, satisfied minimally from the
///        engine's mode-only patch, must let the field change the audio; with
///        no alternative holding the field must not; and in a mode-only patch
///        the fields whose gate holds are exactly the fields that alter the
///        render. Fields that act only under a performance, saturate, or hang
///        on a relation between two keys sit on a reasoned exception list, and
///        an entry that excuses nothing fails.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "c_api/synth_patch_common.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/synth_presets.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::synth::ActiveAlternative;
using sonare::midi::synth::ActiveCondition;
using sonare::midi::synth::apply_engine_params;
using sonare::midi::synth::BodyType;
using sonare::midi::synth::engine_param_descriptors;
using sonare::midi::synth::EngineParam;
using sonare::midi::synth::EngineParamDescriptor;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::VaWaveform;

using Gate = std::vector<ActiveAlternative>;

constexpr double kRate = 24000.0;
constexpr int kBlock = 256;
constexpr int kFrames = 26 * kBlock;

/// A field the gates cannot describe, and why.
struct Excuse {
  SynthEngineMode mode;
  /// The key, or a prefix ending in `*`.
  const char* key;
  const char* reason;
};

const Excuse kExcuses[] = {
    {SynthEngineMode::kKarplusStrong, "bodyCoupling",
     "saturated: the loop's stability cap absorbs it until decayStretch moves"},
    {SynthEngineMode::kAdditive, "morph",
     "acts only where the second registration differs from the first, a relation between keys"},
    {SynthEngineMode::kAdditive, "drawbarsB*",
     "performance: a live morph offset reaches the second registration at morph 0"},
    {SynthEngineMode::kPipeOrgan, "swell", "performance: the shutter follows CC11"},
    {SynthEngineMode::kHarpsichord, "velocityDroopDb",
     "performance: acts only on a velocity above peakVelocity"},
    {SynthEngineMode::kFlute, "jetCentringPerOctave",
     "acts only above the first register (f0 over 2 x C4), a relation to the key"},
};

bool excuse_matches(const Excuse& e, SynthEngineMode mode, const std::string& key) {
  if (e.mode != mode) return false;
  const size_t n = std::strlen(e.key);
  if (n > 0 && e.key[n - 1] == '*') return key.compare(0, n - 1, e.key, n - 1) == 0;
  return key == e.key;
}

bool excused(SynthEngineMode mode, const std::string& key) {
  for (const Excuse& e : kExcuses) {
    if (excuse_matches(e, mode, key)) return true;
  }
  return false;
}

/// The wrapper body vocabulary, in BodyType order.
const char* const kBodyNames[] = {"none", "guitar", "violin", "wood-tube", "brass-bell", "vocal"};
/// The wrapper waveform vocabulary, in VaWaveform order.
const char* const kWaveformNames[] = {"sine", "saw", "square", "triangle", "noise"};

/// A patch as a host selecting only @p mode gets it: the init wrapper and the
/// base preset's engine section.
NativeSynthConfig mode_only(SynthEngineMode mode) {
  const char* base_name = sonare::midi::synth::base_preset_name(mode);
  REQUIRE(base_name != nullptr);
  const auto* base = sonare::midi::synth::find_synth_preset(base_name);
  REQUIRE(base != nullptr);
  NativeSynthConfig cfg;
  cfg.patch.mode = mode;
  sonare_c_detail::copy_engine_section(base->config.patch, mode, &cfg.patch);
  return cfg;
}

/// Two overlapping notes at two velocities, each released inside the window;
/// the low one sits under a harpsichord's foreshortened bass scale.
std::vector<float> render(const NativeSynthConfig& cfg) {
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  std::vector<float> out;
  out.reserve(2 * kFrames);
  for (int b = 0; b < kFrames / kBlock; ++b) {
    if (b == 0)
      synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
    if (b == 6)
      synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 67, 40)));
    if (b == 11)
      synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
    if (b == 17)
      synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_off(0, 0, 67, 0)));
    const sonare::test::StereoRender block = sonare::test::render_stereo(synth, kBlock);
    out.insert(out.end(), block.left.begin(), block.left.end());
    out.insert(out.end(), block.right.begin(), block.right.end());
  }
  return out;
}

bool same(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

const EngineParamDescriptor* find(const std::vector<EngineParamDescriptor>& ds,
                                  const std::string& key) {
  for (const auto& d : ds) {
    if (d.key == key) return &d;
  }
  return nullptr;
}

bool set_engine(NativeSynthConfig& cfg, const std::string& key, double value) {
  const EngineParam p{key.c_str(), value};
  return apply_engine_params(cfg.patch, &p, 1, nullptr);
}

/// A patch under test and its engine fields, whose values follow every write
/// made through @ref set, so a gate is read without rebuilding the descriptors.
struct Patch {
  NativeSynthConfig cfg;
  std::vector<EngineParamDescriptor> fields;
};

Patch patch_of(const NativeSynthConfig& cfg) { return {cfg, engine_param_descriptors(cfg.patch)}; }

const EngineParamDescriptor& field(const Patch& p, const std::string& key) {
  const EngineParamDescriptor* d = find(p.fields, key);
  REQUIRE(d != nullptr);
  return *d;
}

bool set(Patch& p, const std::string& key, double value) {
  if (!set_engine(p.cfg, key, value)) return false;
  for (auto& d : p.fields) {
    if (d.key == key) d.value = static_cast<float>(value);
  }
  return true;
}

/// Values a field is moved to, all accepted and none equal to its own.
std::vector<double> candidates(const EngineParamDescriptor& d) {
  const double v = d.value;
  std::vector<double> raw;
  if (d.boolean) {
    raw = {v == 0.0 ? 1.0 : 0.0};
  } else if (d.bounded) {
    raw = {d.lo, d.hi, d.integer ? std::floor(0.5 * (d.lo + d.hi)) : 0.5 * (d.lo + d.hi)};
    // Near the bottom too: a radiation or loss term can saturate over the rest.
    if (!d.integer) raw.push_back(d.lo + 0.05 * (d.hi - d.lo));
  } else if (d.integer) {
    raw = {v + 1.0, v - 1.0};
  } else {
    raw = {v + 1.0, v != 0.0 ? 0.5 * v : 0.5, 2.0 * v + 3.0};
  }
  std::vector<double> out;
  for (double c : raw) {
    if (static_cast<float>(c) == d.value) continue;
    bool dup = false;
    for (double o : out) dup = dup || o == c;
    if (!dup) out.push_back(c);
  }
  return out;
}

/// Whether moving @p key off its value in @p p changes the render;
/// @p reference is @p p's own render when the caller already has it.
bool acts(const Patch& p, const std::string& key, const std::vector<float>* reference = nullptr) {
  std::vector<float> own;
  if (reference == nullptr) {
    own = render(p.cfg);
    reference = &own;
  }
  for (double c : candidates(field(p, key))) {
    NativeSynthConfig moved = p.cfg;
    if (!set_engine(moved, key, c)) continue;
    if (!same(render(moved), *reference)) return true;
  }
  return false;
}

/// The wrapper's numeric fields a gate names, with the range the patch clamp keeps.
struct WrapperNumber {
  const char* key;
  float NativeSynthPatch::*member;
  double lo, hi;
};
const WrapperNumber kWrapperNumbers[] = {{"bodyMix", &NativeSynthPatch::body_mix, 0.0, 1.0}};

const WrapperNumber& wrapper_number(const std::string& key) {
  for (const WrapperNumber& w : kWrapperNumbers) {
    if (key == w.key) return w;
  }
  FAIL("no wrapper field " << key);
  return kWrapperNumbers[0];
}

bool holds(const ActiveCondition& c, const Patch& p) {
  double v = 0.0;
  if (c.scope == ActiveCondition::Scope::kPatch && c.kind == ActiveCondition::Kind::kString) {
    REQUIRE(c.relation == ActiveCondition::Relation::kEq);
    if (c.key == "waveform")
      return c.text == kWaveformNames[static_cast<int>(p.cfg.patch.waveform)];
    REQUIRE(c.key == "body");
    return c.text == kBodyNames[static_cast<int>(p.cfg.patch.body)];
  }
  if (c.scope == ActiveCondition::Scope::kPatch) {
    v = p.cfg.patch.*wrapper_number(c.key).member;
  } else {
    v = field(p, c.key).value;
  }
  switch (c.relation) {
    case ActiveCondition::Relation::kEq:
      return v == c.number;
    case ActiveCondition::Relation::kNe:
      return v != c.number;
    case ActiveCondition::Relation::kGt:
      return v > c.number;
    case ActiveCondition::Relation::kGe:
      return v >= c.number;
    case ActiveCondition::Relation::kLt:
      return v < c.number;
    case ActiveCondition::Relation::kLe:
      return v <= c.number;
  }
  return false;
}

bool all_hold(const ActiveAlternative& alternative, const Patch& p) {
  for (const auto& c : alternative) {
    if (!holds(c, p)) return false;
  }
  return true;
}

/// What a UI reads off the descriptor: no gate, or some alternative holding.
bool gate_holds(const Gate& gate, const Patch& p) {
  if (gate.empty()) return true;
  for (const auto& alternative : gate) {
    if (all_hold(alternative, p)) return true;
  }
  return false;
}

/// Write the value nearest the condition's threshold that makes @p c hold (or,
/// with @p broken, fail) into @p p.
void force(const ActiveCondition& c, Patch& p, bool broken) {
  if (c.scope == ActiveCondition::Scope::kPatch && c.kind == ActiveCondition::Kind::kString) {
    const bool want_named = !broken;
    if (c.key == "waveform") {
      for (int i = 0; i < static_cast<int>(std::size(kWaveformNames)); ++i) {
        if ((c.text == kWaveformNames[i]) == want_named) {
          p.cfg.patch.waveform = static_cast<VaWaveform>(i);
          return;
        }
      }
      FAIL("no waveform to force " << c.text);
    }
    for (int i = 0; i < static_cast<int>(std::size(kBodyNames)); ++i) {
      if ((c.text == kBodyNames[i]) == want_named) {
        p.cfg.patch.body = static_cast<BodyType>(i);
        return;
      }
    }
    FAIL("no body to force " << c.key);
  }
  const bool wrapper = c.scope == ActiveCondition::Scope::kPatch;
  const EngineParamDescriptor* d = wrapper ? nullptr : &field(p, c.key);
  const bool whole = !wrapper && (d->integer || d->boolean);
  const bool boolean = !wrapper && d->boolean;
  const double v = c.number;
  const double lo = wrapper ? wrapper_number(c.key).lo : d->bounded ? d->lo : v - 1.0;
  const double hi = wrapper ? wrapper_number(c.key).hi : d->bounded ? d->hi : v + 1.0;
  const auto above = [&] { return whole ? v + 1.0 : 0.5 * (v + hi); };
  const auto below = [&] { return whole ? v - 1.0 : 0.5 * (v + lo); };
  using R = ActiveCondition::Relation;
  R relation = c.relation;
  if (broken) {
    switch (relation) {
      case R::kEq:
        relation = R::kNe;
        break;
      case R::kNe:
        relation = R::kEq;
        break;
      case R::kGt:
        relation = R::kLe;
        break;
      case R::kGe:
        relation = R::kLt;
        break;
      case R::kLt:
        relation = R::kGe;
        break;
      case R::kLe:
        relation = R::kGt;
        break;
    }
  }
  double target = v;
  switch (relation) {
    case R::kEq:
    case R::kGe:
    case R::kLe:
      target = v;
      break;
    case R::kNe:
      target = boolean ? 1.0 - v : (v != hi ? hi : lo);
      break;
    case R::kGt:
      target = above();
      break;
    case R::kLt:
      target = below();
      break;
  }
  INFO("forcing " << c.key << " to " << target);
  if (wrapper) {
    p.cfg.patch.*wrapper_number(c.key).member = static_cast<float>(target);
    return;
  }
  REQUIRE(set(p, c.key, target));
}

/// @p alternative made to hold from @p base, each condition the base fails
/// moved to its threshold.
Patch satisfied(const Patch& base, const ActiveAlternative& alternative) {
  Patch p = base;
  for (const auto& c : alternative) {
    if (!holds(c, p)) force(c, p, false);
  }
  REQUIRE(all_hold(alternative, p));
  return p;
}

struct Mismatch {
  std::string key;
  std::string what;
};

/// The keys whose gate holds in @p p against the keys whose movement changes
/// its render, excused keys left out.
std::vector<Mismatch> shown_against_altering(SynthEngineMode mode, const Patch& p,
                                             const char* context) {
  std::vector<Mismatch> out;
  const std::vector<float> reference = render(p.cfg);
  for (const auto& d : p.fields) {
    if (excused(mode, d.key)) continue;
    const bool shown = gate_holds(d.active_when, p);
    if (acts(p, d.key, &reference) != shown) {
      out.push_back({d.key, std::string(context) + (shown ? ": gate holds, field inert"
                                                          : ": gate fails, field acts")});
    }
  }
  return out;
}

/// @p base with every switch a gate names set the way a gate asks, where the
/// switch itself is ungated: the patch a host reaches by flipping one switch.
Patch opened(const Patch& base) {
  Patch p = base;
  for (const auto& d : base.fields) {
    for (const auto& alternative : d.active_when) {
      for (const auto& c : alternative) {
        if (c.kind == ActiveCondition::Kind::kBoolean && field(base, c.key).active_when.empty()) {
          force(c, p, false);
        }
      }
    }
  }
  return p;
}

/// Every gate of @p mode measured: the mode-only and opened patches compared
/// shown against altering, each alternative satisfied minimally, and each
/// minimal state where no alternative holds. Mismatches the exception list
/// does not cover are returned, and each covering entry is marked used.
std::vector<Mismatch> sweep(SynthEngineMode mode, std::vector<bool>& used) {
  std::vector<Mismatch> out;
  const auto report = [&](const std::string& key, const std::string& what) {
    for (size_t i = 0; i < std::size(kExcuses); ++i) {
      if (excuse_matches(kExcuses[i], mode, key)) {
        used[i] = true;
        return;
      }
    }
    out.push_back({key, what});
  };

  const Patch base = patch_of(mode_only(mode));
  for (const auto& [context, name] :
       {std::pair{base, "mode-only"}, std::pair{opened(base), "opened"}}) {
    const std::vector<float> reference = render(context.cfg);
    for (const auto& d : context.fields) {
      const bool shown = gate_holds(d.active_when, context);
      if (acts(context, d.key, &reference) != shown) {
        report(d.key, std::string(name) +
                          (shown ? ": gate holds, field inert" : ": gate fails, field acts"));
      }
    }
  }

  for (const auto& d : base.fields) {
    for (size_t a = 0; a < d.active_when.size(); ++a) {
      const ActiveAlternative& alternative = d.active_when[a];
      const std::string which = "alternative " + std::to_string(a);
      if (!acts(satisfied(base, alternative), d.key)) report(d.key, which + " holds, field inert");
      for (size_t broken = 0; broken < alternative.size(); ++broken) {
        Patch p = base;
        for (size_t i = 0; i < alternative.size(); ++i) {
          if (i != broken && !holds(alternative[i], p)) force(alternative[i], p, false);
        }
        if (holds(alternative[broken], p)) force(alternative[broken], p, true);
        if (gate_holds(d.active_when, p)) continue;
        if (acts(p, d.key)) {
          report(d.key, which + " with " + alternative[broken].key + " broken, field acts");
        }
      }
    }
  }
  return out;
}

void require_gates_measured(const std::vector<SynthEngineMode>& modes) {
  std::vector<bool> used(std::size(kExcuses), false);
  for (SynthEngineMode mode : modes) {
    INFO("engine mode " << static_cast<int>(mode));
    for (const Mismatch& m : sweep(mode, used)) {
      INFO(m.key << ": " << m.what);
      CHECK(false);
    }
  }
  for (size_t i = 0; i < std::size(kExcuses); ++i) {
    bool in_run = false;
    for (SynthEngineMode mode : modes) in_run = in_run || kExcuses[i].mode == mode;
    if (!in_run) continue;
    INFO("exception " << kExcuses[i].key << " (" << kExcuses[i].reason << ") excuses nothing");
    CHECK(used[i]);
  }
}

std::vector<SynthEngineMode> engine_modes() {
  std::vector<SynthEngineMode> modes;
  for (int m = 0; m <= sonare::midi::synth::kSynthEngineModeMax; ++m) {
    const auto mode = static_cast<SynthEngineMode>(m);
    if (sonare::midi::synth::base_preset_name(mode) != nullptr) modes.push_back(mode);
  }
  return modes;
}

}  // namespace

// Percussion's sweep alone runs well past the fast budget in a Debug build, so
// the untagged subset takes a numeric-gated engine and the one gated on
// wrapper fields.
TEST_CASE("engine param gates match the render on brass and bowed string", "[midi][synth]") {
  require_gates_measured({SynthEngineMode::kBrass, SynthEngineMode::kBowedString});
}

// The plucked string's mid anchor is the second pole of its quoted loss solve, so it acts only
// once hfDecayS opens that solve.
TEST_CASE("engine param gates match the render on the plucked string", "[midi][synth]") {
  require_gates_measured({SynthEngineMode::kPluckedString});
}

TEST_CASE("engine param gates match the render on every engine", "[.][slow][midi][synth]") {
  const std::vector<SynthEngineMode> modes = engine_modes();
  REQUIRE(modes.size() > 2);
  require_gates_measured(modes);
}

/// The percussion mode-only patch with only gmKit set, compared shown against altering.
void require_kit_patch_shows_what_alters(bool gm_kit) {
  Patch p = patch_of(mode_only(SynthEngineMode::kPercussion));
  REQUIRE(set(p, "gmKit", gm_kit ? 1 : 0));
  for (const Mismatch& m :
       shown_against_altering(SynthEngineMode::kPercussion, p, gm_kit ? "kit on" : "kit off")) {
    INFO(m.key << ": " << m.what);
    CHECK(false);
  }
}

// A host selecting the percussion engine and switching the kit off, nothing
// else: every field a UI shows as live must move the render, and no other.
TEST_CASE("percussion with the kit off shows exactly the fields that alter it", "[midi][synth]") {
  require_kit_patch_shows_what_alters(false);
}

TEST_CASE("percussion with the kit on shows exactly the fields that alter it",
          "[.][slow][midi][synth]") {
  require_kit_patch_shows_what_alters(true);
}

TEST_CASE("every engine's mode-only patch shows exactly the fields that alter it",
          "[.][slow][midi][synth]") {
  for (SynthEngineMode mode : engine_modes()) {
    INFO("engine mode " << static_cast<int>(mode));
    for (const Mismatch& m : shown_against_altering(mode, patch_of(mode_only(mode)), "mode-only")) {
      INFO(m.key << ": " << m.what);
      CHECK(false);
    }
  }
}

TEST_CASE("an engine param gate never names its own field or a key outside its engine",
          "[midi][synth]") {
  size_t gated = 0;
  for (SynthEngineMode mode : engine_modes()) {
    const auto ds = engine_param_descriptors(mode_only(mode).patch);
    for (const auto& d : ds) {
      for (const auto& alternative : d.active_when) {
        REQUIRE_FALSE(alternative.empty());
        for (const auto& c : alternative) {
          INFO(d.key << " gated on " << c.key);
          ++gated;
          REQUIRE(c.key != d.key);
          if (c.scope == ActiveCondition::Scope::kPatch) continue;
          const EngineParamDescriptor* target = find(ds, c.key);
          REQUIRE(target != nullptr);
          REQUIRE((c.kind == ActiveCondition::Kind::kBoolean) == target->boolean);
          if (target->bounded) {
            REQUIRE(c.number >= target->lo);
            REQUIRE(c.number <= target->hi);
          }
        }
      }
    }
  }
  REQUIRE(gated > 0);
}
