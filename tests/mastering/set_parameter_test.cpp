// Data-driven coverage for ProcessorBase::set_parameter across every insert the
// factory can build. The list is sourced from insert_factory_names() so this
// test automatically covers current and future inserts. It guards against three
// regressions: (1) a missing set_parameter definition (caught at link time and
// by the N >= 1 contract), (2) a set_parameter that returns false for all ids,
// and (3) a set_parameter that is accepted but has no effect on the signal.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/utility/gain.h"
#include "rt/processor_base.h"

namespace {

using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::rt::ProcessorBase;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kNumChannels = 2;
constexpr const char* kGsEfxName = "effects.gsEfx";
// A meaningful output change is anything above this max-abs sample delta.
constexpr float kDiffTolerance = 1.0e-5f;
// Hard cap on probed ids so a buggy set_parameter that always returns true is
// caught instead of looping forever.
constexpr unsigned int kMaxProbedParams = 4096u;

// Inserts that currently expose zero automatable parameters. The first entry is
// configured by data (an impulse response) rather than scalar automation; the
// remainder simply have not had a set_parameter() override implemented yet, so
// they fall back to the ProcessorBase no-op. They are pinned here so the test
// still enforces the >= 1 contract for every covered insert and flags the day a
// covered processor silently loses its override. Removing a name from this list
// once its set_parameter() lands will tighten coverage automatically.
bool IsZeroParamAllowed(const std::string& name) {
  // Every factory insert now exposes at least one automatable parameter
  // (effects.reverb.convolution gained a dry/wet mix), so no name is exempt.
  static const std::array<const char*, 0> kZeroParam = {};
  return std::any_of(kZeroParam.begin(), kZeroParam.end(),
                     [&name](const char* entry) { return name == entry; });
}

// Inserts whose parameter changes are not observable when fed a single block of
// broadband noise (e.g. modulation/lookahead processors that need many blocks,
// or stages whose audible effect requires signal energy in a specific band that
// our generic noise does not isolate). Each entry must be justified by manual
// investigation; the contract checks (N >= 1, range rejection) still apply.
bool IsNoiseEffectExempt(const std::string& name) {
  // multiband.dynamicEq's factory exposes only crossover config, with no
  // per-band activation keys, so make_insert cannot enable a sub-band and the
  // band-parameter automation stays a no-op on our stimulus. Its set_parameter
  // forwards verbatim to the per-band eq.dynamic code path, which IS exercised
  // with an enabled band below, so the forwarding contract is still covered.
  //
  // effects.reverb.convolution constructs as a pure passthrough until an impulse
  // response is supplied (the IR cannot be expressed through scalar JSON params),
  // and process() early-returns while ir_ is empty — so its dryWet automation has
  // no observable effect on a single noise block via plain make_insert(). The
  // set_parameter contract (N == 1, range rejection) is still checked above; the
  // dryWet blend itself is exercised by the effects-layer convolution tests.
  return name == "multiband.dynamicEq" || name == "effects.reverb.convolution";
}

// Fills a deterministic broadband-noise block with a 32-bit LCG so the same
// stimulus drives every comparison.
std::vector<std::vector<float>> MakeNoiseBlock() {
  std::vector<std::vector<float>> channels(
      static_cast<size_t>(kNumChannels), std::vector<float>(static_cast<size_t>(kBlockSize), 0.0f));
  std::uint32_t state = 0x1234567u;
  for (int ch = 0; ch < kNumChannels; ++ch) {
    for (int n = 0; n < kBlockSize; ++n) {
      state = state * 1664525u + 1013904223u;
      const float unit = static_cast<float>(state >> 8) / 16777216.0f;                   // [0, 1)
      channels[static_cast<size_t>(ch)][static_cast<size_t>(n)] = (unit - 0.5f) * 0.6f;  // ~0.3 amp
    }
  }
  return channels;
}

// Drives one block through a processor, mutating the supplied buffers in place.
void ProcessBlock(ProcessorBase& processor, std::vector<std::vector<float>>& channels) {
  std::array<float*, kNumChannels> ptrs{};
  for (int ch = 0; ch < kNumChannels; ++ch) {
    ptrs[static_cast<size_t>(ch)] = channels[static_cast<size_t>(ch)].data();
  }
  processor.process(ptrs.data(), kNumChannels, kBlockSize);
}

float MaxAbsDiff(const std::vector<std::vector<float>>& a,
                 const std::vector<std::vector<float>>& b) {
  float worst = 0.0f;
  for (size_t ch = 0; ch < a.size(); ++ch) {
    for (size_t n = 0; n < a[ch].size(); ++n) {
      worst = std::max(worst, std::abs(a[ch][n] - b[ch][n]));
    }
  }
  return worst;
}

// Builds a prepared insert from JSON params, returning nullptr for unknown
// names. Defaults to the empty object ("{}").
std::unique_ptr<ProcessorBase> MakePrepared(const std::string& name,
                                            const std::string& json_params = "{}") {
  auto processor = make_insert(name, json_params);
  if (processor) {
    processor->prepare(kSampleRate, kBlockSize);
  }
  return processor;
}

// JSON used only by the audible-effect stage to put an insert into a state where
// its parameter automation is observable on a single noise block. Most inserts
// are audible from defaults and use "{}". The EQs, however, construct with every
// band/filter disabled, so changing a band's frequency/gain/Q is a legitimate
// no-op until a band is enabled. The factory enables a band when band params are
// present (eq_band defaults enabled=true), so we activate band 0 with a flat
// (0 dB) gain; the parameter perturbation then sweeps that live band. The
// activation is applied identically to the baseline and perturbed instances, so
// the only difference between them is the perturbed parameter itself.
std::string ActivationJson(const std::string& name) {
  if (name == kGsEfxName) {
    // Thru is useful for catalog probing, but it has no audible graph. Select
    // the measured stereo-EQ type so byte automation reaches a live stage.
    return R"({"typeMsb":1,"typeLsb":0,"realization":0,"byte1":127})";
  }
  if (name == "eq.parametric" || name == "eq.equalizer" || name == "eq.minimumPhase" ||
      name == "eq.linearPhase") {
    return R"({"band0.frequencyHz":1000,"band0.gainDb":0})";
  }
  if (name == "eq.midSide") {
    return R"({"midBand0.frequencyHz":1000,"midBand0.gainDb":0})";
  }
  if (name == "eq.cutFilter") {
    // Enable the high-pass so cutoff/Q automation reshapes the filtered noise.
    return R"({"highPassEnabled":true,"highPassFrequencyHz":2000})";
  }
  if (name == "eq.dynamic") {
    // The factory enables a band only when band params are present; activate
    // band 0 (flat) so its static-gain/freq automation is observable.
    return R"({"band0.frequencyHz":1000,"band0.staticGainDb":0})";
  }
  return "{}";
}

// Probes param ids 0,1,2,... until set_parameter rejects one, returning the
// count N of accepted ids.
unsigned int CountParameters(const std::string& name) {
  auto processor = MakePrepared(name);
  REQUIRE(processor != nullptr);
  unsigned int count = 0;
  while (count < kMaxProbedParams && processor->set_parameter(count, 1.0f)) {
    ++count;
  }
  REQUIRE(count < kMaxProbedParams);
  return count;
}

// The generic count above relies on parameter ids being a contiguous prefix.
// GS EFX deliberately publishes all twenty wire-byte descriptors, while only
// the bytes mapped by the selected type are realtime-safe. Discover those ids
// from the typed activation instead of treating the default Thru state as an
// automation graph.
std::vector<unsigned int> DiscoverParameterIds(const std::string& name) {
  if (name != kGsEfxName) {
    const unsigned int count = CountParameters(name);
    std::vector<unsigned int> ids;
    ids.reserve(count);
    for (unsigned int id = 0; id < count; ++id) ids.push_back(id);
    return ids;
  }

  const std::string activation = ActivationJson(name);
  auto processor = MakePrepared(name, activation);
  REQUIRE(processor != nullptr);
  std::vector<unsigned int> ids;
  for (const auto& descriptor : processor->parameter_descriptors()) {
    if (!processor->parameter_is_realtime_safe(descriptor.id)) continue;
    float constructed = 0.0f;
    REQUIRE(processor->constructed_parameter_value(descriptor.id, &constructed));
    auto probe = MakePrepared(name, activation);
    REQUIRE(probe != nullptr);
    REQUIRE(probe->set_parameter(descriptor.id, constructed));
    ids.push_back(descriptor.id);
  }
  REQUIRE_FALSE(ids.empty());
  return ids;
}

// GS EFX bytes are unsigned 7-bit values, and some typed slots have a shorter
// printed state list. Keep perturbations in the wire domain and retain only
// values accepted by the selected type's realtime setter.
std::vector<float> ParameterPerturbations(const std::string& name, unsigned int id) {
  if (name != kGsEfxName) return {1.0f, 12.0f, -1.0f, -12.0f};

  constexpr std::array<float, 4> kGsValues = {0.0f, 1.0f, 64.0f, 127.0f};
  const std::string activation = ActivationJson(name);
  std::vector<float> accepted;
  for (const float value : kGsValues) {
    auto probe = MakePrepared(name, activation);
    REQUIRE(probe != nullptr);
    if (probe->set_parameter(id, value)) accepted.push_back(value);
  }
  REQUIRE_FALSE(accepted.empty());
  return accepted;
}

}  // namespace

TEST_CASE("All factory inserts expose set_parameter contract",
          "[.][slow][mastering][set_parameter]") {
  const std::vector<std::string> names = insert_factory_names();
  REQUIRE_FALSE(names.empty());

  for (const std::string& name : names) {
    DYNAMIC_SECTION(name) {
      // The factory and the name list live in the same translation unit, so a
      // listed name should always build; skip cleanly if somehow disabled.
      auto probe = make_insert(name, "{}");
      if (!probe) {
        WARN("make_insert returned nullptr for " << name << "; skipping");
        continue;
      }

      // 1. Automatable-param discovery + contract.
      const std::vector<unsigned int> parameter_ids = DiscoverParameterIds(name);
      const unsigned int n = static_cast<unsigned int>(parameter_ids.size());
      if (IsZeroParamAllowed(name)) {
        REQUIRE(n == 0u);
      } else {
        REQUIRE(n >= 1u);
      }

      // 2. Out-of-range rejection: ids past the valid range return false and do
      // not crash.
      {
        auto processor = MakePrepared(name);
        REQUIRE(processor != nullptr);
        REQUIRE_FALSE(processor->set_parameter(1000000u, 0.0f));
        const unsigned int invalid_id = name == kGsEfxName ? 20u : n;
        REQUIRE_FALSE(processor->set_parameter(invalid_id, 0.0f));
      }

      // 3. Audible effect: a parameter change must alter the output of at least
      // one id for processors that have parameters.
      if (n == 0u) {
        continue;
      }

      const auto stimulus = MakeNoiseBlock();
      const std::string activation = ActivationJson(name);

      auto baseline = MakePrepared(name, activation);
      REQUIRE(baseline != nullptr);
      auto reference = stimulus;
      ProcessBlock(*baseline, reference);

      bool observed_change = false;
      // Bidirectional dB-scale magnitudes. Negative values are essential for
      // threshold/ceiling parameters: a positive ceiling clamps to 0 dB (the
      // default) and never engages, whereas a -12 dB ceiling limits the
      // ~-10 dBFS noise stimulus and produces an observable change.
      for (const unsigned int id : parameter_ids) {
        if (observed_change) break;
        for (const float value : ParameterPerturbations(name, id)) {
          auto perturbed = MakePrepared(name, activation);
          REQUIRE(perturbed != nullptr);
          // A fresh instance per id keeps stateful processors (reverbs, filters,
          // envelope followers) from carrying detection state between probes.
          REQUIRE(perturbed->set_parameter(id, value));
          auto output = stimulus;
          ProcessBlock(*perturbed, output);
          if (MaxAbsDiff(reference, output) > kDiffTolerance) {
            observed_change = true;
            break;
          }
        }
      }

      if (!IsNoiseEffectExempt(name)) {
        INFO("No parameter change altered the output of insert: " << name);
        REQUIRE(observed_change);
      }
    }
  }
}

namespace {

// Accepts every id and counts how often a value actually reaches it.
class CountingProcessor : public ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int calls = 0;

 protected:
  bool set_parameter_impl(unsigned int, float) override {
    ++calls;
    return true;
  }
};

}  // namespace

TEST_CASE("set_parameter refuses a non-finite value before any processor sees it",
          "[mastering][set_parameter]") {
  CountingProcessor processor;
  for (float value :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()}) {
    CHECK_FALSE(processor.set_parameter(0, value));
  }
  CHECK(processor.calls == 0);
  CHECK(processor.set_parameter(0, std::numeric_limits<float>::max()));
  CHECK(processor.calls == 1);
}

TEST_CASE("Processor parameter metadata retains construction and last successful values",
          "[mastering][set_parameter][last_applied]") {
  auto processor = make_insert("utility.gain", R"({"levelDb":-3})");
  REQUIRE(processor != nullptr);
  auto* gain = dynamic_cast<sonare::mastering::utility::Gain*>(processor.get());
  REQUIRE(gain != nullptr);
  auto check = [](const sonare::rt::ProcessorBase& value, float expected) {
    float constructed = 0.0f;
    float last = 0.0f;
    REQUIRE(value.constructed_parameter_value(0, &constructed));
    REQUIRE(constructed == -3.0f);
    REQUIRE(value.last_applied_parameter_value(0, &last));
    REQUIRE(last == expected);
  };
  check(*gain, -3.0f);
  REQUIRE(gain->set_parameter(0, -6.0f));
  check(*gain, -6.0f);
  REQUIRE_FALSE(gain->set_parameter(999, 1.0f));
  REQUIRE_FALSE(gain->set_parameter(0, std::numeric_limits<float>::quiet_NaN()));
  REQUIRE_FALSE(gain->set_parameter(0, std::numeric_limits<float>::infinity()));
  check(*gain, -6.0f);
  float untouched = 42.0f;
  REQUIRE_FALSE(gain->last_applied_parameter_value(999, &untouched));
  REQUIRE(untouched == 42.0f);
  REQUIRE_FALSE(gain->last_applied_parameter_value(0, nullptr));
  auto copied = *gain;
  check(copied, -6.0f);
  auto moved = std::move(copied);
  check(moved, -6.0f);
  REQUIRE(moved.set_parameter(0, -9.0f));
  check(moved, -9.0f);
  check(*gain, -6.0f);
}
