/// @file repair_stereo_impulse_test.cpp
/// @brief Detection, validation and linked-stereo behaviour of declick/declip.
///
/// The fixtures reproduce two items of the restoration corpus
/// (tools/mastering-eval/corpus.py) in the tree, so the planted quantities the
/// manifest records -- twelve two-sample clicks, 1560 clipped samples per
/// channel in runs of at most four -- are what the detectors are measured
/// against. The corpus audio itself is not committed; the recipe is.
///
/// Where a number is asserted here it is either a count the corpus generator
/// produced (numpy, a different toolchain from this one), a value derived in the
/// test from the fixture, or a ratio between two strategies. No RMSE is pinned
/// to a recorded measurement: the shared mono transfer decides a per-sample gain
/// across two discontinuous branches, so its own RMSE is only good to about four
/// significant figures from one build to the next.
///
/// The golden digests are the exception and their job is narrow. They came from
/// the build that preceded the detector extraction and they say one thing: the
/// extraction changed no sample. A digest mismatch under a different optimizer
/// or a different Eigen path is not a defect in the repair.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "mastering/api/audio_utils.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::repair;

namespace {

constexpr int kSampleRate = 48000;
constexpr double kBedHz = 440.0;
/// The corpus writes 24-bit WAVs and measures every planted quantity after the
/// round trip, so a fixture that skips the grid is a different signal.
constexpr double kQuantizeFull = 8388607.0;
constexpr double kQuantizeScale = 8388608.0;
/// One step of that grid: the smallest difference two fixtures can carry.
constexpr double kQuantizeStep = 1.0 / kQuantizeScale;

float quantize24(double value) {
  const double clamped = std::min(1.0, std::max(-1.0, value));
  return static_cast<float>(std::nearbyint(clamped * kQuantizeFull) / kQuantizeScale);
}

std::vector<double> sine(size_t frames, double amp, double phase) {
  std::vector<double> out(frames);
  for (size_t i = 0; i < frames; ++i) {
    out[i] =
        amp * std::sin(constants::kTwoPiD * kBedHz * static_cast<double>(i) / kSampleRate + phase);
  }
  return out;
}

std::vector<float> quantize(const std::vector<double>& values) {
  std::vector<float> out(values.size());
  for (size_t i = 0; i < values.size(); ++i) out[i] = quantize24(values[i]);
  return out;
}

// ------------------------------------------------------------ corpus fixtures

/// The corpus "sine_click" item: one second of 440 Hz at 0.15, plus the twelve
/// two-sample clicks the manifest lists, identical in both channels.
constexpr size_t kClickPositions[] = {1036,  5053,  9669,  12799, 14866, 18103,
                                      21899, 24475, 26817, 30462, 39336, 45273};
constexpr float kClickAmplitudes[] = {-0.653685f, -0.625842f, 0.547648f,  0.516581f,
                                      0.663013f,  0.703012f,  -0.586058f, 0.541328f,
                                      0.50645f,   -0.557609f, -0.734495f, -0.650943f};
constexpr size_t kPlantedClicks = 12;
constexpr size_t kPlantedClickWidth = 2;

std::vector<float> click_bed(double phase) { return quantize(sine(48000, 0.15, phase)); }

std::vector<float> click_fixture(double phase) {
  std::vector<double> bed = sine(48000, 0.15, phase);
  for (size_t k = 0; k < kPlantedClicks; ++k) {
    for (size_t w = 0; w < kPlantedClickWidth; ++w) {
      bed[kClickPositions[k] + w] += static_cast<double>(kClickAmplitudes[k]);
    }
  }
  return quantize(bed);
}

/// The config the planted count is recovered at. The defaults reject ten of the
/// twelve on this bed, which is what ClickDetection::rejected is for.
const DeclickConfig kCorpusDeclick{0.35f, 2.0f, 8, 20, 8.0f};

/// The corpus "sine_clip" item: half a second peak-normalized to 0.95 and hard
/// clipped at 0.945.
constexpr size_t kClipFrames = 24000;
constexpr float kClipThreshold = 0.945f;
constexpr size_t kPlantedClippedSamples = 1560;
constexpr size_t kPlantedClippedRuns = 440;
constexpr size_t kPlantedLongestClipRun = 4;

std::vector<double> clip_bed_unclipped(double phase, double peak) {
  std::vector<double> bed = sine(kClipFrames, 1.0, phase);
  double measured = 0.0;
  for (double value : bed) measured = std::max(measured, std::abs(value));
  for (double& value : bed) value *= peak / measured;
  return bed;
}

std::vector<float> clip_fixture(double phase, double peak = 0.95) {
  std::vector<double> bed = clip_bed_unclipped(phase, peak);
  for (double& value : bed) {
    value = std::min(static_cast<double>(kClipThreshold),
                     std::max(-static_cast<double>(kClipThreshold), value));
  }
  return quantize(bed);
}

/// The plateau the file actually carries. The WAV grid puts it just under the
/// requested threshold, so this -- not 0.945 -- is what a detector is handed.
float threshold_in_file(const std::vector<float>& a, const std::vector<float>& b) {
  float peak = 0.0f;
  for (float value : a) peak = std::max(peak, std::abs(value));
  for (float value : b) peak = std::max(peak, std::abs(value));
  return peak;
}

// -------------------------------------------------------------------- metrics

Audio view(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
}

std::vector<float> to_vector(const Audio& audio) {
  return std::vector<float>(audio.data(), audio.data() + audio.size());
}

/// FNV-1a over the raw sample bits: an exact identity, not a tolerance.
uint32_t digest(const Audio& audio) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < audio.size(); ++i) {
    uint32_t bits = 0;
    const float value = audio[i];
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (bits >> (8 * byte)) & 0xffu;
      hash *= 16777619u;
    }
  }
  return hash;
}

double rmse(const std::vector<float>& got, const std::vector<float>& want) {
  double sum = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    const double error = static_cast<double>(got[i]) - static_cast<double>(want[i]);
    sum += error * error;
  }
  return std::sqrt(sum / static_cast<double>(got.size()));
}

/// RMS of (error_left - error_right): what a defect present in both channels
/// leaves behind once only one side has been repaired. This is the quantity the
/// linked selection exists to remove; per-channel RMSE cannot see it.
double image_error(const std::vector<float>& left, const std::vector<float>& right,
                   const std::vector<float>& clean_left, const std::vector<float>& clean_right) {
  double sum = 0.0;
  for (size_t i = 0; i < left.size(); ++i) {
    const double difference = (static_cast<double>(left[i]) - clean_left[i]) -
                              (static_cast<double>(right[i]) - clean_right[i]);
    sum += difference * difference;
  }
  return std::sqrt(sum / static_cast<double>(left.size()));
}

template <typename Repair>
std::pair<std::vector<float>, std::vector<float>> shared_mono_transfer(
    const std::vector<float>& left, const std::vector<float>& right, Repair&& repair) {
  std::vector<float> a = left;
  std::vector<float> b = right;
  mastering::api::detail::apply_shared_mono_transfer_repair(a, b, kSampleRate, repair);
  return {a, b};
}

template <typename Repair>
std::pair<std::vector<float>, std::vector<float>> independent(const std::vector<float>& left,
                                                              const std::vector<float>& right,
                                                              Repair&& repair) {
  std::vector<float> a = left;
  std::vector<float> b = right;
  mastering::api::detail::apply_independent_repair(a, b, kSampleRate, repair);
  return {a, b};
}

}  // namespace

// ---------------------------------------------------------------- validation

TEST_CASE("Declick validation rejects the non-finite values the old form let past",
          "[repair][stereo][impulse]") {
  const std::vector<float> samples = click_fixture(0.0);
  const Audio audio = view(samples);

  // An infinite threshold compares greater than zero, so the old single-clause
  // check accepted it and the pass became a silent no-op: measured against the
  // preceding build, the output digest equalled the input's.
  DeclickConfig infinite_threshold = kCorpusDeclick;
  infinite_threshold.threshold = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS(declick(audio, infinite_threshold));
  REQUIRE_THROWS(detect_clicks(samples.data(), samples.size(), kSampleRate, infinite_threshold));

  DeclickConfig nan_threshold = kCorpusDeclick;
  nan_threshold.threshold = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(declick(audio, nan_threshold));

  DeclickConfig infinite_neighbor = kCorpusDeclick;
  infinite_neighbor.neighbor_ratio = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS(declick(audio, infinite_neighbor));

  DeclickConfig nan_residual = kCorpusDeclick;
  nan_residual.residual_ratio = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(declick(audio, nan_residual));

  DeclickConfig no_clicks = kCorpusDeclick;
  no_clicks.max_click_samples = 0;
  REQUIRE_THROWS(declick(audio, no_clicks));

  DeclickConfig negative_order = kCorpusDeclick;
  negative_order.lpc_order = -1;
  REQUIRE_THROWS(declick(audio, negative_order));

  REQUIRE_NOTHROW(declick(audio, kCorpusDeclick));
  REQUIRE_THROWS(detect_clicks(samples.data(), samples.size(), 0, kCorpusDeclick));
}

TEST_CASE("Declip validation rejects the knobs the clamp used to swallow",
          "[repair][stereo][impulse]") {
  const std::vector<float> samples = clip_fixture(0.0);
  const Audio audio = view(samples);
  DeclipConfig base;
  base.clip_threshold = threshold_in_file(samples, samples);

  // std::clamp propagates NaN, so before this validation a NaN blend weight
  // reached the output: the preceding build produced 46 NaN samples on a
  // 2048-sample clipped sine rather than raising.
  DeclipConfig nan_blend = base;
  nan_blend.lpc_blend = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(declip(audio, nan_blend));

  // Out of range used to be clamped: lpc_blend 2.0 produced the same output as
  // 1.0, and iterations 0 the same output as 1.
  DeclipConfig over_blend = base;
  over_blend.lpc_blend = 2.0f;
  REQUIRE_THROWS(declip(audio, over_blend));

  DeclipConfig negative_blend = base;
  negative_blend.lpc_blend = -0.1f;
  REQUIRE_THROWS(declip(audio, negative_blend));

  DeclipConfig no_iterations = base;
  no_iterations.iterations = 0;
  REQUIRE_THROWS(declip(audio, no_iterations));

  DeclipConfig nan_threshold = base;
  nan_threshold.clip_threshold = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(declip(audio, nan_threshold));
  REQUIRE_THROWS(detect_clipping(samples.data(), samples.size(), kSampleRate, nan_threshold));

  DeclipConfig over_unity = base;
  over_unity.clip_threshold = 1.5f;
  REQUIRE_THROWS(declip(audio, over_unity));

  REQUIRE_NOTHROW(declip(audio, base));
  REQUIRE_THROWS(detect_clipping(samples.data(), samples.size(), 0, base));
}

// ----------------------------------------------------------------- detection

TEST_CASE("Declick detection recovers the corpus click count", "[repair][stereo][impulse]") {
  const std::vector<float> samples = click_fixture(0.0);

  const ClickDetection detected =
      detect_clicks(samples.data(), samples.size(), kSampleRate, kCorpusDeclick);
  REQUIRE(detected.count == kPlantedClicks);
  REQUIRE(detected.rejected == 0);
  REQUIRE(detected.longest_run_samples == kPlantedClickWidth);
  // One second of material, so the rate is the count. The tolerance is one
  // float32 ulp at 12, below which the division cannot land.
  REQUIRE_THAT(detected.per_second, WithinAbs(12.0f, 1.0e-6f));

  // The defaults are too tight for a 0.15 bed: they act on two of the twelve and
  // say so rather than reporting clean material. A detector that had stalled
  // would report zero on both halves; the sum is the planted quantity at every
  // setting, while the split moves with the knobs.
  const ClickDetection defaults = detect_clicks(samples.data(), samples.size(), kSampleRate);
  REQUIRE(defaults.count == 2);
  REQUIRE(defaults.rejected == 10);

  struct Row {
    float threshold;
    float neighbor_ratio;
    size_t count;
  };
  for (const Row& row : {Row{0.8f, 4.0f, 2}, Row{0.5f, 4.0f, 8}, Row{0.4f, 3.0f, 10},
                         Row{0.4f, 2.0f, 11}, Row{0.35f, 2.0f, 12}}) {
    DeclickConfig config = kCorpusDeclick;
    config.threshold = row.threshold;
    config.neighbor_ratio = row.neighbor_ratio;
    const ClickDetection swept = detect_clicks(samples.data(), samples.size(), kSampleRate, config);
    CAPTURE(row.threshold, row.neighbor_ratio);
    REQUIRE(swept.count == row.count);
    REQUIRE(swept.count + swept.rejected == kPlantedClicks);
  }
}

TEST_CASE("Declip detection recovers the corpus clipped-sample count",
          "[repair][stereo][impulse]") {
  const std::vector<float> left = clip_fixture(0.0);
  const std::vector<float> right = clip_fixture(0.35);
  DeclipConfig config;
  config.clip_threshold = threshold_in_file(left, right);
  // The plateau is the requested threshold snapped to the 24-bit grid, so it is
  // computed here rather than written down.
  REQUIRE(config.clip_threshold == quantize24(kClipThreshold));

  for (const std::vector<float>& channel : {left, right}) {
    const ClipDetection detected =
        detect_clipping(channel.data(), channel.size(), kSampleRate, config);
    REQUIRE(detected.sample_count == kPlantedClippedSamples);
    REQUIRE(detected.run_count == kPlantedClippedRuns);
    REQUIRE(detected.longest_run_samples == kPlantedLongestClipRun);
    REQUIRE(detected.sample_fraction ==
            static_cast<float>(kPlantedClippedSamples) / static_cast<float>(kClipFrames));
    // Every run is far short of the LPC gap cap, so none takes the fallback.
    REQUIRE(detected.longest_run_samples < kDeclipMaxLpcGapSamples);
  }

  // The requested threshold is not the one in the file: the grid puts the
  // plateau seven hundredths of a millionth below 0.945, and an inclusive test
  // against the requested number finds nothing at all.
  DeclipConfig requested = config;
  requested.clip_threshold = kClipThreshold;
  const ClipDetection missed = detect_clipping(left.data(), left.size(), kSampleRate, requested);
  REQUIRE(missed.sample_count == 0);
  REQUIRE(missed.run_count == 0);
}

// --------------------------------------------------- mono output is preserved

TEST_CASE("Declick mono output survives the detector extraction unchanged",
          "[repair][stereo][impulse]") {
  const std::vector<float> left = click_fixture(0.0);
  const std::vector<float> right = click_fixture(0.35);
  // The fixtures are quantized to a 24-bit grid, so their digests are the one
  // thing here a different architecture reproduces.
  REQUIRE(digest(view(left)) == 0xb7cec44bu);
  REQUIRE(digest(view(right)) == 0xe96f434fu);
  // The repair moved the signal: its digest is not the input's.
  REQUIRE(digest(declick(view(left), kCorpusDeclick)) != digest(view(left)));
  REQUIRE(digest(declick(view(right), kCorpusDeclick)) != digest(view(right)));

  DeclickReport report;
  const Audio repaired = declick(view(left), kCorpusDeclick, &report);
  // Asking for a report does not move a sample. Read against a call the same
  // binary made rather than against a recorded value, so the claim is the
  // extraction's and not the host's arithmetic.
  REQUIRE(digest(repaired) == digest(declick(view(left), kCorpusDeclick)));
  REQUIRE(report.detected.count == kPlantedClicks);
  REQUIRE(report.repaired_runs == kPlantedClicks);
  REQUIRE(report.repaired_samples == kPlantedClicks * kPlantedClickWidth);
  REQUIRE(report.linked_runs == 0);
  REQUIRE(report.lpc_model_used);

  // An input too short for the configured order gets no AR model, so the fills
  // are linear. The report is the only place that shows it.
  const std::vector<float> tiny = {0.1f, 1.0f, 0.1f};
  DeclickReport tiny_report;
  declick(Audio::from_buffer(tiny.data(), tiny.size(), kSampleRate), {0.8f, 4.0f, 8, 20, 8.0f},
          &tiny_report);
  REQUIRE_FALSE(tiny_report.lpc_model_used);
  REQUIRE(tiny_report.repaired_runs == 1);
}

TEST_CASE("Declip mono output survives the detector extraction unchanged",
          "[repair][stereo][impulse]") {
  const std::vector<float> left = clip_fixture(0.0);
  const std::vector<float> right = clip_fixture(0.35);
  DeclipConfig config;
  config.clip_threshold = threshold_in_file(left, right);

  REQUIRE(digest(view(left)) == 0x2c547f25u);
  REQUIRE(digest(view(right)) == 0x87fe27e5u);
  REQUIRE(digest(declip(view(left), config)) != digest(view(left)));
  REQUIRE(digest(declip(view(right), config)) != digest(view(right)));

  DeclipReport report;
  const Audio repaired = declip(view(left), config, &report);
  REQUIRE(digest(repaired) == digest(declip(view(left), config)));
  REQUIRE(report.detected.sample_count == kPlantedClippedSamples);
  REQUIRE(report.lpc_reconstructed_runs == kPlantedClippedRuns);
  REQUIRE(report.interpolated_runs == 0);
  REQUIRE(report.repaired_samples == kPlantedClippedSamples);
  REQUIRE(report.linked_runs == 0);
}

// The recorded halves of the two cases above, `[.]`-hidden like every other hash
// freeze in this tree: the digest folds raw float samples, which is finer than
// the reproducibility of an LPC solve across architectures and libm
// implementations, so a value recorded on one host cannot match another. It
// stays a same-environment refactor tripwire, run through `make test-golden`.
// The declip pair predates the detector extraction and has survived it; the
// declick pair is re-recorded whenever the fill's arithmetic changes, which a
// bit-identity claim about declick can then be read against.
//
// The two halves are separate cases because they hold under different conditions,
// which one case cannot express: a skip covers everything in it.
TEST_CASE("Declip mono digests stay stable", "[.][repair][stereo][impulse][golden]") {
  const std::vector<float> clip_left = clip_fixture(0.0);
  const std::vector<float> clip_right = clip_fixture(0.35);
  DeclipConfig config;
  config.clip_threshold = threshold_in_file(clip_left, clip_right);
  CHECK(digest(declip(view(clip_left), config)) == 0x6fd0ee17u);
  CHECK(digest(declip(view(clip_right), config)) == 0xd76bca10u);
}

// These two are recorded twice because the fill's last bits move with the
// optimization level, and the sensitivity belongs to `src/util/lpc.cpp` rather
// than to declick: compiling that one unit at -O0 moves both digests while
// declick's own level changes nothing, and the declip pair above holds at every
// level while solving the same way 440 times. The difference is 1 ULP on 11 of
// 48000 samples, no sample moves by more than 1e-6, and detected / rejected /
// repaired_runs / repaired_samples are identical -- so the digest is a finer
// instrument than the behaviour it guards. Both values are recorded rather than
// one guarded and the other skipped: the sanctioned target builds Release, while
// the default ctest tree is Debug, so skipping either branch would leave one of
// the two ways this case gets invoked checking nothing.
TEST_CASE("Declick mono digests stay stable", "[.][repair][stereo][impulse][golden]") {
  const std::vector<float> click_left = click_fixture(0.0);
  const std::vector<float> click_right = click_fixture(0.35);
#ifdef NDEBUG
  CHECK(digest(declick(view(click_left), kCorpusDeclick)) == 0x6861830cu);
  CHECK(digest(declick(view(click_right), kCorpusDeclick)) == 0x4a9a870fu);
#else
  CHECK(digest(declick(view(click_left), kCorpusDeclick)) == 0x508faa52u);
  CHECK(digest(declick(view(click_right), kCorpusDeclick)) == 0x65fb694eu);
#endif
}

TEST_CASE("Declip reports the runs that fall past the LPC gap cap", "[repair][stereo][impulse]") {
  // A plateau longer than the cap is filled by interpolation, and lpc_order,
  // iterations and lpc_blend do not reach it. Before the report there was no way
  // for a caller to learn that the branch had been taken.
  const size_t frames = kDeclipMaxLpcGapSamples * 3;
  std::vector<float> samples(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    samples[i] = quantize24(0.5 * std::sin(constants::kTwoPiD * 4.0 * static_cast<double>(i) /
                                           static_cast<double>(frames)));
  }
  const size_t long_run_start = kDeclipMaxLpcGapSamples / 2;
  const size_t long_run_length = kDeclipMaxLpcGapSamples + 64;
  for (size_t i = 0; i < long_run_length; ++i) samples[long_run_start + i] = 0.99f;
  const size_t short_run_start = frames - 200;
  for (size_t i = 0; i < 16; ++i) samples[short_run_start + i] = -0.99f;

  DeclipReport report;
  DeclipConfig config;
  config.clip_threshold = 0.98f;
  declip(view(samples), config, &report);

  REQUIRE(report.detected.run_count == 2);
  REQUIRE(report.detected.longest_run_samples == long_run_length);
  REQUIRE(report.interpolated_runs == 1);
  REQUIRE(report.lpc_reconstructed_runs == 1);
  REQUIRE(report.repaired_samples == long_run_length + 16);
}

// ---------------------------------------------------------------- stereo link

TEST_CASE("Declick stereo with identical channels equals the mono path bit for bit",
          "[repair][stereo][impulse]") {
  const std::vector<float> samples = click_fixture(0.0);
  const Audio audio = view(samples);
  const uint32_t mono = digest(declick(audio, kCorpusDeclick));

  const DeclickStereoResult linked = declick_stereo(audio, audio, kCorpusDeclick);
  REQUIRE(digest(linked.left) == mono);
  REQUIRE(digest(linked.right) == mono);
  REQUIRE(linked.left_report.linked_runs == 0);
  REQUIRE(linked.right_report.linked_runs == 0);
  REQUIRE(linked.left_report.repaired_runs == kPlantedClicks);

  const std::vector<float> shorter(samples.begin(), samples.end() - 1);
  REQUIRE_THROWS(declick_stereo(audio, view(shorter), kCorpusDeclick));
}

TEST_CASE("Declip stereo with identical channels equals the mono path bit for bit",
          "[repair][stereo][impulse]") {
  const std::vector<float> samples = clip_fixture(0.0);
  const Audio audio = view(samples);
  DeclipConfig config;
  config.clip_threshold = threshold_in_file(samples, samples);
  const uint32_t mono = digest(declip(audio, config));

  const DeclipStereoResult linked = declip_stereo(audio, audio, config);
  REQUIRE(digest(linked.left) == mono);
  REQUIRE(digest(linked.right) == mono);
  REQUIRE(linked.left_report.linked_runs == 0);
  REQUIRE(linked.right_report.linked_runs == 0);
  REQUIRE(linked.left_report.detected.sample_count == kPlantedClippedSamples);
}

TEST_CASE("Linked declick detection beats the shared mono transfer on a common-mode click",
          "[repair][stereo][impulse]") {
  const std::vector<float> clean_left = click_bed(0.0);
  const std::vector<float> clean_right = click_bed(0.35);
  const std::vector<float> dirty_left = click_fixture(0.0);
  const std::vector<float> dirty_right = click_fixture(0.35);
  // The default neighbour ratio: the two channels disagree about which runs
  // qualify, which is the case the link is for. At the corpus setting both
  // channels select all twelve and there is nothing to link.
  const DeclickConfig config{0.35f, 4.0f, 8, 20, 8.0f};
  const auto repair = [&](const Audio& in) { return declick(in, config); };

  // The planted click is the same number in both channels, so before repair the
  // two error signals differ only where the 24-bit grid separates them: at most
  // one step over the 24 planted samples, which bounds the image error at
  // step * sqrt(planted / frames). A one-sided repair is what introduces a shift.
  const double grid_image_bound =
      kQuantizeStep * std::sqrt(static_cast<double>(kPlantedClicks * kPlantedClickWidth) /
                                static_cast<double>(dirty_left.size()));
  REQUIRE(image_error(dirty_left, dirty_right, clean_left, clean_right) < grid_image_bound);

  const auto transfer = shared_mono_transfer(dirty_left, dirty_right, repair);
  const auto split = independent(dirty_left, dirty_right, repair);
  const DeclickStereoResult linked = declick_stereo(view(dirty_left), view(dirty_right), config);
  const std::vector<float> linked_left = to_vector(linked.left);
  const std::vector<float> linked_right = to_vector(linked.right);

  // The link acted: the right channel selected a run the left did not, and the
  // left repaired it anyway. These are counts, so they are exact.
  REQUIRE(linked.left_report.detected.count == 8);
  REQUIRE(linked.right_report.detected.count == 9);
  REQUIRE(linked.left_report.repaired_runs == 9);
  REQUIRE(linked.right_report.repaired_runs == 9);
  REQUIRE(linked.left_report.linked_runs == 1);
  REQUIRE(linked.right_report.linked_runs == 0);

  // The three strategies are compared by ratio rather than by recorded digits.
  // The transfer path decides a per-sample gain across two discontinuous
  // branches -- a 1e-6 zero guard and a +/-4 clamp -- so one sample crossing
  // either moves its RMSE in the fourth significant figure from one build to the
  // next. The separations asserted here are orders clear of that: measured, the
  // linked image error is ~7e5x below the transfer's and ~4e6x below the
  // independent one.
  const double transfer_image =
      image_error(transfer.first, transfer.second, clean_left, clean_right);
  const double split_image = image_error(split.first, split.second, clean_left, clean_right);
  const double linked_image = image_error(linked_left, linked_right, clean_left, clean_right);
  CAPTURE(transfer_image, split_image, linked_image);
  // The two degenerate passes this separation could otherwise be read off, stated
  // as themselves: a link that copied one channel onto the other, and a pass that
  // left both alone. A magnitude floor cannot carry that any more -- the two-sided
  // fill puts the linked image error below the 24-bit grid bound a no-op also sits
  // under, so the bound would now reject the good result and the degenerate one
  // together.
  REQUIRE(linked_image > 0.0);
  REQUIRE(linked_left != linked_right);
  REQUIRE(linked_left != dirty_left);
  REQUIRE(linked_right != dirty_right);
  REQUIRE(linked_image * 100.0 < transfer_image);
  REQUIRE(linked_image * 100.0 < split_image);
  // Repairing only one side is worse for the image than not repairing at all,
  // which is the whole reason the selection is shared.
  REQUIRE(split_image > transfer_image);

  // Per-channel fidelity improves as well, so nothing was traded for the image.
  REQUIRE(rmse(linked_left, clean_left) < rmse(transfer.first, clean_left));
  REQUIRE(rmse(linked_right, clean_right) < rmse(transfer.second, clean_right));
  REQUIRE(rmse(linked_left, clean_left) < rmse(split.first, clean_left));
  REQUIRE(rmse(linked_left, clean_left) < rmse(dirty_left, clean_left));
  // Both channels now carry the same residual because they repaired the same
  // nine runs; the independent pass leaves the left one an unrepaired click
  // worse, which is a separation of tens of percent rather than of parts.
  REQUIRE(std::abs(rmse(linked_left, clean_left) - rmse(linked_right, clean_right)) <
          0.001 * rmse(linked_left, clean_left));
  REQUIRE(rmse(split.first, clean_left) > 1.1 * rmse(split.second, clean_right));
}

TEST_CASE("Linked declip detection beats the shared mono transfer on the corpus pair",
          "[repair][stereo][impulse]") {
  const std::vector<float> clean_left = quantize(clip_bed_unclipped(0.0, 0.95));
  const std::vector<float> clean_right = quantize(clip_bed_unclipped(0.35, 0.95));
  const std::vector<float> dirty_left = clip_fixture(0.0);
  const std::vector<float> dirty_right = clip_fixture(0.35);
  DeclipConfig config;
  config.clip_threshold = threshold_in_file(dirty_left, dirty_right);
  const auto repair = [&](const Audio& in) { return declip(in, config); };

  const auto transfer = shared_mono_transfer(dirty_left, dirty_right, repair);
  const DeclipStereoResult linked = declip_stereo(view(dirty_left), view(dirty_right), config);
  const std::vector<float> linked_left = to_vector(linked.left);
  const std::vector<float> linked_right = to_vector(linked.right);

  // The two channels clip at different times, so their downmix stays below the
  // threshold everywhere and the shared mono transfer has nothing to repair.
  // Every transfer gain is then exactly 1.0 and the pair comes back bit for bit
  // unchanged -- an identity, so it is asserted as one rather than as an RMSE.
  const std::vector<float> downmix = mastering::api::detail::mono_mix(dirty_left, dirty_right);
  REQUIRE(detect_clipping(downmix.data(), downmix.size(), kSampleRate, config).run_count == 0);
  REQUIRE(transfer.first == dirty_left);
  REQUIRE(transfer.second == dirty_right);

  // Against that baseline the link is a repair rather than a better repair. The
  // separations below are between a treated and an untreated signal, so they are
  // tens of percent and no recorded digit is needed to see them.
  REQUIRE(rmse(linked_left, clean_left) * 1.5 < rmse(dirty_left, clean_left));
  REQUIRE(rmse(linked_right, clean_right) * 1.5 < rmse(dirty_right, clean_right));
  REQUIRE(rmse(linked_left, clean_left) > 0.0);
  REQUIRE(linked.left_report.repaired_samples == kPlantedClippedSamples);
  REQUIRE(linked.right_report.repaired_samples == kPlantedClippedSamples);

  const double transfer_image =
      image_error(transfer.first, transfer.second, clean_left, clean_right);
  const double linked_image = image_error(linked_left, linked_right, clean_left, clean_right);
  CAPTURE(transfer_image, linked_image);
  REQUIRE(transfer_image == image_error(dirty_left, dirty_right, clean_left, clean_right));
  REQUIRE(linked_image * 1.5 < transfer_image);

  // The union is disjoint here, so no run is widened. The link costs nothing
  // where it has nothing to do.
  REQUIRE(linked.left_report.linked_runs == 0);
  REQUIRE(linked.right_report.linked_runs == 0);
}

TEST_CASE("Linked declip reconstructs one region in both channels at unequal levels",
          "[repair][stereo][impulse]") {
  // The same waveform at two levels: the plateaus land on the same peaks and the
  // quieter side's runs are shorter, so the union widens every one of them.
  const std::vector<float> clean_left = quantize(clip_bed_unclipped(0.0, 0.95));
  const std::vector<float> clean_right = quantize(clip_bed_unclipped(0.0, 0.9465));
  const std::vector<float> dirty_left = clip_fixture(0.0, 0.95);
  const std::vector<float> dirty_right = clip_fixture(0.0, 0.9465);
  DeclipConfig config;
  config.clip_threshold = threshold_in_file(dirty_left, dirty_right);
  const auto repair = [&](const Audio& in) { return declip(in, config); };

  const auto transfer = shared_mono_transfer(dirty_left, dirty_right, repair);
  const auto split = independent(dirty_left, dirty_right, repair);
  const DeclipStereoResult linked = declip_stereo(view(dirty_left), view(dirty_right), config);
  const std::vector<float> linked_left = to_vector(linked.left);
  const std::vector<float> linked_right = to_vector(linked.right);

  // Every one of the quieter channel's runs is widened to the louder one's.
  REQUIRE(linked.left_report.linked_runs == 0);
  REQUIRE(linked.right_report.linked_runs == kPlantedClippedRuns);

  // Ratios again, for the reason given in the declick case: the transfer path's
  // per-sample gain crosses a zero guard and a clamp, so its own RMSE is only
  // good to about four significant figures across builds. Measured, the linked
  // image error is 356x below the transfer's and 205x below the independent one.
  const double transfer_image =
      image_error(transfer.first, transfer.second, clean_left, clean_right);
  const double split_image = image_error(split.first, split.second, clean_left, clean_right);
  const double linked_image = image_error(linked_left, linked_right, clean_left, clean_right);
  CAPTURE(transfer_image, split_image, linked_image);
  REQUIRE(linked_image * 100.0 < transfer_image);
  REQUIRE(linked_image * 100.0 < split_image);

  // What the shared selection bought: the two channels now carry the same
  // reconstruction error. They differ in level by 0.37%, so their errors should
  // track to within a few times that; repaired independently they stand a factor
  // of three apart, because the quieter one keeps most of its plateau.
  REQUIRE(std::abs(rmse(linked_left, clean_left) - rmse(linked_right, clean_right)) <
          0.02 * rmse(linked_left, clean_left));
  REQUIRE(rmse(split.first, clean_left) > 3.0 * rmse(split.second, clean_right));

  // And what it cost, asserted in the direction it went rather than left
  // implicit: the quieter channel is reconstructed over samples it had not
  // clipped, so its own fidelity falls below what the transfer leaves. The left
  // channel, which set the extent, improves on the transfer as usual.
  REQUIRE(rmse(linked_left, clean_left) * 1.3 < rmse(transfer.first, clean_left));
  REQUIRE(rmse(linked_right, clean_right) > 2.0 * rmse(transfer.second, clean_right));
}
