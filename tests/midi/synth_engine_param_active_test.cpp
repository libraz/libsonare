/// @file synth_engine_param_active_test.cpp
/// @brief The `active_when` gates of the engine-section descriptors, measured
///        against renders: a field must change the audio exactly when its gate
///        holds, must change it once the gate is satisfied, and must stop when
///        any one of its conditions is broken. Fields that act only under a
///        performance, saturate, or are reached through several layers at once
///        sit on a reasoned exception list, and an entry that excuses nothing
///        fails.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "c_api/synth_patch_common.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/synth_presets.h"
#include "support/midi_render.h"

namespace {

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
    {SynthEngineMode::kPercussion, "exclusiveClass",
     "performance: chokes a sounding piece on an overlapping note of the same class"},
    {SynthEngineMode::kPercussion, "modeAlpha*",
     "reached through the strike point or the air-spring split, either one"},
    {SynthEngineMode::kPercussion, "baseFreqHz",
     "reached through the modes or a key-tracked shell mode, either one"},
    {SynthEngineMode::kPercussion, "noiseAirHz", "bounds the noise, wire and shimmer layers alike"},
    {SynthEngineMode::kPercussion, "contactMs",
     "times the direct contact and the plate's contact alike"},
    {SynthEngineMode::kPercussion, "shell*", "rings whichever layer sounds"},
    {SynthEngineMode::kPercussion, "plate*",
     "rings whichever layer sounds, its own contact included"},
};

bool excuse_matches(const Excuse& e, SynthEngineMode mode, const std::string& key) {
  if (e.mode != mode) return false;
  const size_t n = std::strlen(e.key);
  if (n > 0 && e.key[n - 1] == '*') return key.compare(0, n - 1, e.key, n - 1) == 0;
  return key == e.key;
}

/// The wrapper body vocabulary, in BodyType order.
const char* const kBodyNames[] = {"none", "guitar", "violin", "wood-tube", "brass-bell", "vocal"};

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

/// Whether moving @p key off its value in @p cfg changes the render;
/// @p reference is @p cfg's own render when the caller already has it.
bool acts(const NativeSynthConfig& cfg, const std::string& key,
          const std::vector<float>* reference = nullptr) {
  const auto ds = engine_param_descriptors(cfg.patch);
  const EngineParamDescriptor* d = find(ds, key);
  REQUIRE(d != nullptr);
  std::vector<float> own;
  if (reference == nullptr) {
    own = render(cfg);
    reference = &own;
  }
  for (double c : candidates(*d)) {
    NativeSynthConfig moved = cfg;
    if (!set_engine(moved, key, c)) continue;
    if (!same(render(moved), *reference)) return true;
  }
  return false;
}

double engine_value(const NativeSynthConfig& cfg, const std::string& key) {
  const auto ds = engine_param_descriptors(cfg.patch);
  const EngineParamDescriptor* d = find(ds, key);
  REQUIRE(d != nullptr);
  return d->value;
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

bool holds(const ActiveCondition& c, const NativeSynthConfig& cfg) {
  double v = 0.0;
  if (c.scope == ActiveCondition::Scope::kPatch && c.kind == ActiveCondition::Kind::kString) {
    REQUIRE(c.key == "body");
    REQUIRE(c.relation == ActiveCondition::Relation::kEq);
    return c.text == kBodyNames[static_cast<int>(cfg.patch.body)];
  }
  if (c.scope == ActiveCondition::Scope::kPatch) {
    v = cfg.patch.*wrapper_number(c.key).member;
  } else {
    v = engine_value(cfg, c.key);
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

bool all_hold(const std::vector<ActiveCondition>& gate, const NativeSynthConfig& cfg) {
  for (const auto& c : gate) {
    if (!holds(c, cfg)) return false;
  }
  return true;
}

/// Write a value into @p cfg that makes @p c hold (or, with @p broken, fail).
void force(const ActiveCondition& c, NativeSynthConfig& cfg, bool broken) {
  if (c.scope == ActiveCondition::Scope::kPatch && c.kind == ActiveCondition::Kind::kString) {
    const bool want_named = !broken;
    for (int i = 0; i < static_cast<int>(std::size(kBodyNames)); ++i) {
      if ((c.text == kBodyNames[i]) == want_named) {
        cfg.patch.body = static_cast<BodyType>(i);
        return;
      }
    }
    FAIL("no body to force " << c.key);
  }
  const bool wrapper = c.scope == ActiveCondition::Scope::kPatch;
  const auto ds = engine_param_descriptors(cfg.patch);
  const EngineParamDescriptor* d = wrapper ? nullptr : find(ds, c.key);
  REQUIRE((wrapper || d != nullptr));
  const bool whole = !wrapper && (d->integer || d->boolean);
  const bool boolean = !wrapper && d->boolean;
  const double v = c.number;
  const double lo = wrapper ? wrapper_number(c.key).lo : d->bounded ? d->lo : v - 1.0;
  const double hi = wrapper ? wrapper_number(c.key).hi : d->bounded ? d->hi : v + 1.0;
  const auto above = [&] { return whole ? hi : 0.5 * (v + hi); };
  const auto below = [&] { return whole ? lo : 0.5 * (v + lo); };
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
    cfg.patch.*wrapper_number(c.key).member = static_cast<float>(target);
    return;
  }
  REQUIRE(set_engine(cfg, c.key, target));
}

struct Mismatch {
  std::string key;
  std::string what;
};

/// Every gate of @p mode measured three ways; mismatches the exception list
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

  const NativeSynthConfig base = mode_only(mode);
  const auto ds = engine_param_descriptors(base.patch);

  // The opened context: every switch a gate names, set the way the gate asks.
  NativeSynthConfig opened = base;
  for (const auto& d : ds) {
    for (const auto& c : d.active_when) {
      if (c.kind == ActiveCondition::Kind::kBoolean && find(ds, c.key)->active_when.empty()) {
        force(c, opened, false);
      }
    }
  }

  for (const NativeSynthConfig* context : {&base, static_cast<const NativeSynthConfig*>(&opened)}) {
    const char* name = context == &base ? "base" : "opened";
    const std::vector<float> reference = render(*context);
    for (const auto& d : ds) {
      const bool expected = all_hold(d.active_when, *context);
      if (acts(*context, d.key, &reference) != expected) {
        report(d.key, std::string(name) +
                          (expected ? ": gate holds, field inert" : ": gate fails, field acts"));
      }
    }
  }

  for (const auto& d : ds) {
    if (d.active_when.empty()) continue;
    NativeSynthConfig satisfied = base;
    for (const auto& c : d.active_when) force(c, satisfied, false);
    if (!acts(satisfied, d.key)) report(d.key, "gate satisfied, field inert");
    for (size_t broken = 0; broken < d.active_when.size(); ++broken) {
      NativeSynthConfig cfg = base;
      for (size_t i = 0; i < d.active_when.size(); ++i) {
        if (i != broken) force(d.active_when[i], cfg, false);
      }
      if (all_hold({d.active_when[broken]}, cfg)) force(d.active_when[broken], cfg, true);
      if (acts(cfg, d.key)) report(d.key, "acts with " + d.active_when[broken].key + " broken");
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

}  // namespace

// Percussion's sweep alone runs ~20 s in a Debug build, so the untagged subset
// takes a numeric-gated engine and the one gated on wrapper fields.
TEST_CASE("engine param gates match the render on brass and bowed string", "[midi][synth]") {
  require_gates_measured({SynthEngineMode::kBrass, SynthEngineMode::kBowedString});
}

TEST_CASE("engine param gates match the render on every engine", "[.][slow][midi][synth]") {
  std::vector<SynthEngineMode> modes;
  for (int m = 0; m <= sonare::midi::synth::kSynthEngineModeMax; ++m) {
    const auto mode = static_cast<SynthEngineMode>(m);
    if (sonare::midi::synth::base_preset_name(mode) != nullptr) modes.push_back(mode);
  }
  REQUIRE(modes.size() > 2);
  require_gates_measured(modes);
}

TEST_CASE("an engine param gate never names its own field or a key outside its engine",
          "[midi][synth]") {
  size_t gated = 0;
  for (int m = 0; m <= sonare::midi::synth::kSynthEngineModeMax; ++m) {
    const auto mode = static_cast<SynthEngineMode>(m);
    if (sonare::midi::synth::base_preset_name(mode) == nullptr) continue;
    const auto ds = engine_param_descriptors(mode_only(mode).patch);
    for (const auto& d : ds) {
      for (const auto& c : d.active_when) {
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
  REQUIRE(gated > 0);
}
