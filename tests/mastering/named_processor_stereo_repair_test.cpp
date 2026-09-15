// The stereo branch of apply_named_processor_stereo(), one case per repair id.
//
// Every case is written against the id's OWN stereo contract rather than one
// shared property: three of the five link a decision across the channels, one
// links only a tracked frequency, and one is independent by contract. Each
// carries its own ablation -- the same processor run per channel through the
// mono entry point -- because a witness that a per-channel implementation also
// satisfies says nothing about the branch under test.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "mastering/api/named_processor.h"
#include "util/constants.h"

namespace {

namespace api = sonare::mastering::api;

constexpr int kSampleRate = 48000;
constexpr double kTwoPi = 2.0 * sonare::constants::kPiD;

/// @brief Reproducible uniform noise in [-1, 1).
class Lcg {
 public:
  explicit Lcg(uint32_t seed) : state_(seed) {}
  float next() {
    state_ = state_ * 1664525u + 1013904223u;
    return static_cast<float>(state_ >> 8) / 8388608.0f - 1.0f;
  }

 private:
  uint32_t state_;
};

std::vector<float> tone(size_t size, double hz, double amplitude, double phase) {
  std::vector<float> out(size);
  for (size_t i = 0; i < size; ++i) {
    out[i] = static_cast<float>(
        amplitude * std::sin(kTwoPi * hz * static_cast<double>(i) / kSampleRate + phase));
  }
  return out;
}

api::StereoResult run_stereo(const std::string& name, const std::vector<float>& left,
                             const std::vector<float>& right,
                             const std::vector<api::Param>& params = {}) {
  return api::apply_named_processor_stereo(name, left.data(), right.data(), left.size(),
                                           kSampleRate, params);
}

/// @brief The ablation: the same id run on one channel at a time.
std::vector<float> run_mono(const std::string& name, const std::vector<float>& channel,
                            const std::vector<api::Param>& params = {}) {
  return api::apply_named_processor(name, channel.data(), channel.size(), kSampleRate, params)
      .samples;
}

/// @brief Indices where @p processed differs from @p input, compared as bits.
/// @details A repair that edits a region leaves every other sample untouched, so
///   this set IS the decision the pass made, read off the output.
std::vector<size_t> edited_indices(const std::vector<float>& input,
                                   const std::vector<float>& processed) {
  std::vector<size_t> out;
  const size_t count = std::min(input.size(), processed.size());
  for (size_t i = 0; i < count; ++i) {
    if (std::memcmp(&input[i], &processed[i], sizeof(float)) != 0) out.push_back(i);
  }
  return out;
}

bool identical(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

// ------------------------------------------------- union of selected regions

TEST_CASE("repair.declick repairs the union of both channels' runs",
          "[mastering][repair][stereo][named]") {
  // A click in the left channel only. The right channel is a different
  // programme rather than a scaled copy: a scaled copy would be repaired
  // identically by a per-channel pass too, since every threshold in this module
  // scales with it, and the case would then pass for the implementation it
  // exists to exclude.
  constexpr size_t kLength = 8192;
  constexpr size_t kClickAt = 4000;
  std::vector<float> left = tone(kLength, 220.0, 0.30, 0.0);
  std::vector<float> right = tone(kLength, 331.0, 0.24, 0.9);
  Lcg rng(4u);
  for (size_t i = 0; i < kLength; ++i) {
    left[i] += 0.01f * rng.next();
    right[i] += 0.01f * rng.next();
  }
  left[kClickAt] += 0.9f;
  left[kClickAt + 1] -= 0.9f;

  const std::vector<api::Param> params = {{"threshold", 0.35},
                                          {"neighborRatio", 2.0},
                                          {"maxClickSamples", 8.0},
                                          {"lpcOrder", 20.0},
                                          {"residualRatio", 8.0}};
  const api::StereoResult stereo = run_stereo("repair.declick", left, right, params);
  REQUIRE(stereo.left.size() == kLength);
  REQUIRE(stereo.right.size() == kLength);

  const std::vector<size_t> left_edits = edited_indices(left, stereo.left);
  const std::vector<size_t> right_edits = edited_indices(right, stereo.right);
  INFO("left edits " << left_edits.size() << " right edits " << right_edits.size());
  // The contract is a shared region set, so the two channels are edited at the
  // same indices -- and the right channel is edited at all, which is what only
  // the union can produce.
  REQUIRE_FALSE(left_edits.empty());
  CHECK(right_edits == left_edits);

  // The ablation. Per channel, the right one has no click of its own to find,
  // so it comes back untouched and the sets cannot agree.
  const std::vector<float> mono_right = run_mono("repair.declick", right, params);
  CHECK(edited_indices(right, mono_right).empty());
  CHECK_FALSE(identical(stereo.right, mono_right));
}

TEST_CASE("repair.declip reconstructs the union of both channels' runs",
          "[mastering][repair][stereo][named]") {
  // The same plateau in both channels, one sample longer on the left. The
  // contract is that the union is reconstructed, so the right channel's shorter
  // run is widened to the left's.
  constexpr size_t kLength = 4096;
  std::vector<float> left = tone(kLength, 120.0, 0.98, 0.0);
  std::vector<float> right = tone(kLength, 120.0, 0.96, 0.0);
  const float ceiling = 0.90f;
  for (size_t i = 0; i < kLength; ++i) {
    left[i] = std::clamp(left[i], -ceiling, ceiling);
    right[i] = std::clamp(right[i], -ceiling, ceiling);
  }

  const std::vector<api::Param> params = {
      {"clipThreshold", 0.89}, {"lpcOrder", 16.0}, {"iterations", 2.0}, {"lpcBlend", 1.0}};
  const api::StereoResult stereo = run_stereo("repair.declip", left, right, params);
  REQUIRE(stereo.left.size() == kLength);

  const std::vector<size_t> left_edits = edited_indices(left, stereo.left);
  const std::vector<size_t> right_edits = edited_indices(right, stereo.right);
  INFO("left edits " << left_edits.size() << " right edits " << right_edits.size());
  REQUIRE_FALSE(left_edits.empty());
  CHECK(right_edits == left_edits);

  // The ablation: the narrower channel keeps its own narrower region set.
  const std::vector<float> mono_right = run_mono("repair.declip", right, params);
  const std::vector<size_t> mono_edits = edited_indices(right, mono_right);
  INFO("per-channel right edits " << mono_edits.size());
  CHECK(mono_edits != right_edits);
}

// ------------------------------------------------------- one shared range

TEST_CASE("repair.trimSilence cuts both channels to one range",
          "[mastering][repair][stereo][named]") {
  // Signal in disjoint halves: the left channel speaks early, the right late.
  // Cutting each to its own range would leave two different lengths.
  // The two bursts are deliberately different lengths as well as disjoint: with
  // equal-length bursts the per-channel ablation below comes out to one length
  // by symmetry and stops excluding anything.
  constexpr size_t kLength = 24000;
  std::vector<float> left(kLength, 0.0f);
  std::vector<float> right(kLength, 0.0f);
  for (size_t i = 4000; i < 8000; ++i) left[i] = 0.4f;
  for (size_t i = 14000; i < 22000; ++i) right[i] = 0.4f;

  const std::vector<api::Param> params = {{"threshold", 0.05}};
  const api::StereoResult stereo = run_stereo("repair.trimSilence", left, right, params);

  // One range, so one length -- and the range spans from the left channel's
  // onset to the right channel's offset because either channel counts as signal.
  CHECK(stereo.left.size() == stereo.right.size());
  INFO("kept " << stereo.left.size() << " of " << kLength);
  CHECK(stereo.left.size() == 18000);
  CHECK(stereo.left.size() < kLength);

  // The ablation: per channel the two lengths differ, which is the outcome the
  // shared range exists to prevent.
  const std::vector<float> mono_left = run_mono("repair.trimSilence", left, params);
  const std::vector<float> mono_right = run_mono("repair.trimSilence", right, params);
  INFO("per-channel lengths " << mono_left.size() << " and " << mono_right.size());
  CHECK(mono_left.size() != mono_right.size());
  CHECK(mono_left.size() < stereo.left.size());
}

// ------------------------------------------------ one shared tracked frequency

TEST_CASE("repair.dehum tracks one fundamental for the pair",
          "[mastering][repair][stereo][named]") {
  // The same 50 Hz hum under two different programmes. Tracked per channel, each
  // search is pulled by its own material and the two notches land apart; the
  // contract is that the tracker reads the channel mean instead.
  constexpr size_t kLength = 48000;
  const std::vector<float> hum = tone(kLength, 50.0, 0.20, 0.0);
  std::vector<float> left = tone(kLength, 62.0, 0.45, 0.0);
  std::vector<float> right = tone(kLength, 41.0, 0.45, 1.3);
  for (size_t i = 0; i < kLength; ++i) {
    left[i] += hum[i];
    right[i] += hum[i];
  }

  const std::vector<api::Param> params = {{"fundamentalHz", 50.0},
                                          {"harmonics", 3.0},
                                          {"q", 30.0},
                                          {"adaptive", 1.0},
                                          {"searchRangeHz", 8.0}};
  const api::StereoResult stereo = run_stereo("repair.dehum", left, right, params);
  REQUIRE(stereo.left.size() == kLength);

  // The ablation. Only the frequency is shared, so the outputs are not expected
  // to equal the per-channel ones on both sides -- but at least one side must
  // differ, or nothing was shared at all.
  const std::vector<float> mono_left = run_mono("repair.dehum", left, params);
  const std::vector<float> mono_right = run_mono("repair.dehum", right, params);
  const bool left_moved = !identical(stereo.left, mono_left);
  const bool right_moved = !identical(stereo.right, mono_right);
  INFO("left moved " << left_moved << " right moved " << right_moved);
  CHECK((left_moved || right_moved));

  // And the pass did something, stated as selectivity rather than as a bare
  // level: a stage that simply turned everything down would satisfy "the hum
  // dropped" without having notched anything.
  const auto line_level = [&](const std::vector<float>& x, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
      const double angle = kTwoPi * hz * static_cast<double>(i) / kSampleRate;
      re += x[i] * std::cos(angle);
      im += x[i] * std::sin(angle);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(x.size());
  };
  const double hum_ratio = line_level(stereo.left, 50.0) / line_level(left, 50.0);
  const double programme_ratio = line_level(stereo.left, 62.0) / line_level(left, 62.0);
  INFO("hum ratio " << hum_ratio << " programme ratio " << programme_ratio);
  CHECK(hum_ratio < 0.5 * programme_ratio);
  CHECK(programme_ratio > 0.8);
}

// ------------------------------------------------- independent by contract

TEST_CASE("repair.decrackle leaves the channels independent",
          "[mastering][repair][stereo][named]") {
  // Different scratches at different instants, which is what the contract says
  // this defect is. Nothing is shared, so the stereo entry has to agree with the
  // mono one bit for bit -- a later change that links this goes red here.
  constexpr size_t kLength = 8192;
  std::vector<float> left = tone(kLength, 180.0, 0.30, 0.0);
  std::vector<float> right = tone(kLength, 240.0, 0.28, 0.4);
  Lcg rng(77u);
  for (size_t i = 0; i < kLength; ++i) {
    left[i] += 0.01f * rng.next();
    right[i] += 0.01f * rng.next();
  }
  for (size_t at : {1100u, 2600u, 5200u}) left[at] += 0.6f;
  for (size_t at : {900u, 3900u, 6100u}) right[at] -= 0.6f;

  const std::vector<api::Param> params = {{"threshold", 0.25}, {"mode", 0.0}, {"levels", 4.0}};
  const api::StereoResult stereo = run_stereo("repair.decrackle", left, right, params);
  const std::vector<float> mono_left = run_mono("repair.decrackle", left, params);
  const std::vector<float> mono_right = run_mono("repair.decrackle", right, params);

  CHECK(identical(stereo.left, mono_left));
  CHECK(identical(stereo.right, mono_right));

  // Non-vacuity: the equality above is also satisfied by a pass that does
  // nothing at all, so the pass has to have edited each channel.
  CHECK_FALSE(edited_indices(left, stereo.left).empty());
  CHECK_FALSE(edited_indices(right, stereo.right).empty());
  // And the two channels' decisions are genuinely different, which is what makes
  // "independent" an observation rather than a coincidence of equal inputs.
  CHECK(edited_indices(left, stereo.left) != edited_indices(right, stereo.right));
}
