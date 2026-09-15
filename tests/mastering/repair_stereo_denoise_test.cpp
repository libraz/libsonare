// Channel-linked denoise and dereverb: configuration rejection, the detectors,
// the mask report, and what the linked mask buys over a time-domain transfer
// ratio. The mono entry points are frozen by hash here because every case below
// is written against them as the reference.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/audio_io.h"
#include "core/spectrum.h"
#include "mastering/api/audio_utils.h"
#include "mastering/common/noise_profile.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "repair_metrics.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

namespace repair = sonare::mastering::repair;
namespace common = sonare::mastering::common;
namespace metrics = sonare::test::repair_metrics;
using Catch::Approx;
using sonare::Audio;
using sonare::ErrorCode;
using sonare::SonareException;

constexpr int kSampleRate = 48000;
constexpr double kTwoPi = 2.0 * sonare::constants::kPiD;

/// @brief Reproducible uniform noise in [-1, 1), so a planted defect is a known quantity.
class Lcg {
 public:
  explicit Lcg(uint32_t seed) : state_(seed) {}
  float next() {
    state_ = state_ * 1664525u + 1013904223u;
    return static_cast<float>(state_ >> 8) / 8388608.0f - 1.0f;
  }
  /// @brief Approximately Gaussian by the central limit theorem over 12 draws.
  float next_gaussian() {
    float sum = 0.0f;
    for (int i = 0; i < 12; ++i) sum += next();
    return sum * 0.5f;
  }

 private:
  uint32_t state_;
};

// The bed every case starts from: two steady tones plus a slow envelope, so the
// quantile noise estimator has both tonal bins and a varying frame energy.
std::vector<float> tonal_bed(size_t size, double phase) {
  std::vector<float> out(size);
  for (size_t i = 0; i < size; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const double envelope = 0.7 + 0.3 * std::sin(kTwoPi * 0.5 * t);
    out[i] = static_cast<float>(envelope * (0.35 * std::sin(kTwoPi * 440.0 * t + phase) +
                                            0.18 * std::sin(kTwoPi * 1320.0 * t + phase)));
  }
  return out;
}

// The bed the mono freeze below was captured on: no envelope, uniform floor.
std::vector<float> make_bed(size_t size, uint32_t seed) {
  std::vector<float> out(size);
  Lcg rng(seed);
  for (size_t i = 0; i < size; ++i) {
    const float noise = rng.next();
    const double t = static_cast<double>(i) / kSampleRate;
    out[i] = static_cast<float>(0.35 * std::sin(kTwoPi * 440.0 * t) +
                                0.18 * std::sin(kTwoPi * 1320.0 * t)) +
             0.05f * noise;
  }
  return out;
}

double mean_square(const std::vector<float>& x) {
  double sum = 0.0;
  for (const float v : x) sum += static_cast<double>(v) * static_cast<double>(v);
  return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

/// @brief Adds noise at @p snr_db against the bed's own power, as corpus.py does.
/// @return The planted floor in dBFS: 10*log10 of the noise's mean square.
double plant_noise(std::vector<float>* bed, Lcg* rng, double snr_db) {
  std::vector<float> noise(bed->size());
  for (size_t i = 0; i < noise.size(); ++i) noise[i] = rng->next_gaussian();
  const double unit = mean_square(noise);
  const double target = mean_square(*bed) / std::pow(10.0, snr_db / 10.0);
  const double gain = std::sqrt(target / unit);
  for (size_t i = 0; i < noise.size(); ++i) {
    noise[i] = static_cast<float>(noise[i] * gain);
    (*bed)[i] += noise[i];
  }
  return 10.0 * std::log10(mean_square(noise));
}

uint64_t hash_samples(const Audio& audio) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < audio.size(); ++i) {
    uint32_t bits = 0;
    const float value = audio[i];
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (bits >> (byte * 8)) & 0xffu;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

bool identical(const Audio& a, const Audio& b) {
  if (a.size() != b.size()) return false;
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::vector<float> to_vector(const Audio& audio) {
  return std::vector<float>(audio.data(), audio.data() + audio.size());
}

Audio as_audio(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
}

/// @brief How far the quietest tenth of the STFT frames sits below the mean, in dB.
/// @details The quantile noise estimator averages exactly those frames, so this
///   is the offset its reading carries on a signal with no programme on it.
double quietest_decile_offset_db(const std::vector<float>& samples) {
  sonare::StftConfig stft;
  stft.n_fft = 1024;
  stft.hop_length = 256;
  const sonare::Spectrogram spec = sonare::Spectrogram::compute(as_audio(samples), stft);
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const std::vector<float>& power = spec.power();
  std::vector<double> energy(static_cast<size_t>(frames), 0.0);
  for (int b = 0; b < bins; ++b) {
    const double weight = (b == 0 || b == bins - 1) ? 1.0 : 2.0;
    for (int t = 0; t < frames; ++t) {
      energy[static_cast<size_t>(t)] +=
          weight * static_cast<double>(power[static_cast<size_t>(b * frames + t)]);
    }
  }
  double total = 0.0;
  for (const double e : energy) total += e;
  std::sort(energy.begin(), energy.end());
  const size_t selected = std::max<size_t>(1, static_cast<size_t>(std::lround(0.1 * frames)));
  double quietest = 0.0;
  for (size_t i = 0; i < selected; ++i) quietest += energy[i];
  return 10.0 * std::log10((quietest / static_cast<double>(selected)) /
                           (total / static_cast<double>(frames)));
}

ErrorCode rejection_code(const std::function<void()>& call) {
  try {
    call();
  } catch (const SonareException& e) {
    return e.code();
  }
  return ErrorCode::Ok;
}

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

}  // namespace

// ---------------------------------------------------------------- mono freeze

TEST_CASE("mono denoise and dereverb are unchanged", "[repair][stereo][denoise]") {
  const auto bed = make_bed(16384, 12345u);
  const Audio audio = as_audio(bed);

  repair::DenoiseClassicalConfig stsa;
  stsa.mode = repair::DenoiseMode::MmseStsa;
  repair::DenoiseClassicalConfig berouti;
  berouti.mode = repair::DenoiseMode::SpectralSubtraction;
  repair::DenoiseClassicalConfig mcra;
  mcra.noise_estimator = repair::DenoiseNoiseEstimator::Mcra;
  repair::DereverbClassicalConfig wpe;
  wpe.wpe_enabled = true;

  // Two things move these: a refactor that shifts a multiply between float and
  // double, and any change to a default the gain mask derives from. The
  // suppression depth is one such default, so it moves the modes that read it.
  CHECK(hash_samples(repair::denoise_classical(audio)) == 0x15db5a462dd378ccull);
  CHECK(hash_samples(repair::denoise_classical(audio, stsa)) == 0x2b708e00347f0b26ull);
  CHECK(hash_samples(repair::denoise_classical(audio, berouti)) == 0x1b736c89a790d94bull);
  CHECK(hash_samples(repair::denoise_classical(audio, mcra)) == 0xdcbbe46d44350ed7ull);
  CHECK(hash_samples(repair::dereverb_classical(audio)) == 0xaa8454d236697b37ull);
  CHECK(hash_samples(repair::dereverb_classical(audio, wpe)) == 0x3fa27970fd263f44ull);
}

// ------------------------------------------------------------------ rejection

TEST_CASE("denoise rejects every out-of-domain field by name", "[repair][stereo][denoise]") {
  const auto bed = make_bed(4096, 77u);
  const Audio audio = as_audio(bed);
  const auto reject = [&](auto mutate) {
    repair::DenoiseClassicalConfig config;
    mutate(config);
    return rejection_code([&] { repair::denoise_classical(audio, config); });
  };

  CHECK(rejection_code([&] { repair::denoise_classical(audio); }) == ErrorCode::Ok);

  // NaN fails every comparison, so a `x < 0` guard would pass it through.
  CHECK(reject([](auto& c) { c.dd_alpha = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.reduction_db = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.over_subtraction = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.spectral_floor = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.noise_estimation_quantile = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.dd_alpha = kInf; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.reduction_db = kInf; }) == ErrorCode::InvalidParameter);

  CHECK(reject([](auto& c) { c.n_fft = 1000; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.hop_length = 0; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.hop_length = 2048; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.dd_alpha = 1.0f; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.reduction_db = -1.0f; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.over_subtraction = 17.0f; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.noise_estimation_quantile = 0.0f; }) == ErrorCode::InvalidParameter);

  // Both enums fell through to a default branch before: an unknown estimator ran
  // Imcra silently, and an unknown mode threw only after the STFT had been built.
  CHECK(reject([](auto& c) { c.mode = static_cast<repair::DenoiseMode>(99); }) ==
        ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) {
          c.noise_estimator = static_cast<repair::DenoiseNoiseEstimator>(99);
        }) == ErrorCode::InvalidParameter);

  // The detector and the linked entry share the same oracle.
  CHECK(rejection_code([&] {
          repair::DenoiseClassicalConfig config;
          config.reduction_db = kNaN;
          repair::detect_noise_floor(bed.data(), bed.size(), kSampleRate, config);
        }) == ErrorCode::InvalidParameter);
  CHECK(rejection_code([&] { repair::detect_noise_floor(bed.data(), bed.size(), 0); }) ==
        ErrorCode::InvalidParameter);
}

TEST_CASE("dereverb rejects every out-of-domain field by name", "[repair][stereo][denoise]") {
  const auto bed = make_bed(4096, 77u);
  const Audio audio = as_audio(bed);
  const auto reject = [&](auto mutate) {
    repair::DereverbClassicalConfig config;
    mutate(config);
    return rejection_code([&] { repair::dereverb_classical(audio, config); });
  };

  CHECK(rejection_code([&] { repair::dereverb_classical(audio); }) == ErrorCode::Ok);

  // These four were written as `x < 0`, which NaN passes. Observed: before this
  // change every one of them ran to completion and returned samples.
  CHECK(reject([](auto& c) { c.late_delay_ms = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.over_subtraction = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.spectral_floor = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.wpe_strength = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.late_delay_ms = kInf; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.threshold = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.attenuation = kNaN; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.t60_sec = kNaN; }) == ErrorCode::InvalidParameter);

  // The WPE solve is cubic in the tap count and independent of the input length,
  // so an unbounded tap count diverged on a one-second buffer.
  CHECK(reject([](auto& c) { c.wpe_taps = repair::kDereverbMaxWpeTaps + 1; }) ==
        ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.wpe_iterations = repair::kDereverbMaxWpeIterations + 1; }) ==
        ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.wpe_taps = 0; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.threshold = 1.5f; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.attenuation = 1.5f; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.n_fft = 1000; }) == ErrorCode::InvalidParameter);
  CHECK(reject([](auto& c) { c.hop_length = 2048; }) == ErrorCode::InvalidParameter);

  CHECK(rejection_code([&] {
          repair::DereverbClassicalConfig config;
          config.wpe_strength = kNaN;
          repair::detect_reverb(bed.data(), bed.size(), kSampleRate, config);
        }) == ErrorCode::InvalidParameter);
}

// -------------------------------------------------------- dead-field rewiring

TEST_CASE("dereverb threshold and attenuation reach the samples", "[repair][stereo][denoise]") {
  const auto bed = make_bed(16384, 12345u);
  const Audio audio = as_audio(bed);

  // The declared defaults now say what this module has always done, so the
  // default output is the same hash the mono freeze above pins.
  repair::DereverbClassicalConfig defaults;
  CHECK(defaults.threshold == 0.0f);
  CHECK(defaults.attenuation == 1.0f);

  repair::DereverbClassicalConfig half;
  half.attenuation = 0.5f;
  repair::DereverbClassicalConfig gated;
  gated.threshold = 0.9f;

  const Audio full_out = repair::dereverb_classical(audio, defaults);
  const Audio half_out = repair::dereverb_classical(audio, half);
  const Audio gated_out = repair::dereverb_classical(audio, gated);

  // Both knobs were inert before this change: all three of these were one hash.
  CHECK_FALSE(identical(full_out, half_out));
  CHECK_FALSE(identical(full_out, gated_out));
  CHECK_FALSE(identical(half_out, gated_out));

  // attenuation scales how far the output moves from the input, so half the
  // attenuation moves it roughly half as far. The value is what distinguishes a
  // wired knob from one that merely perturbs the output.
  const auto distance = [&](const Audio& processed) {
    double sum = 0.0;
    for (size_t i = 0; i < processed.size(); ++i) {
      const double d = static_cast<double>(processed[i]) - static_cast<double>(bed[i]);
      sum += d * d;
    }
    return std::sqrt(sum);
  };
  const double full_distance = distance(full_out);
  const double half_distance = distance(half_out);
  REQUIRE(full_distance > 0.0);
  CHECK(half_distance / full_distance > 0.4);
  CHECK(half_distance / full_distance < 0.6);

  // attenuation = 0 is the identity the wiring promises: nothing is subtracted.
  repair::DereverbClassicalConfig off;
  off.attenuation = 0.0f;
  CHECK(distance(repair::dereverb_classical(audio, off)) / full_distance < 1.0e-3);

  // threshold, swept alone. Raising it admits fewer bins as late reverberation,
  // so the output moves less; an implementation that does not read the field
  // returns one distance for all four and fails at the first step. Each step is
  // required to be a real one rather than a last-bit one -- measured against the
  // ungated distance the sweep reads 0.163, 0.145 and 0.119.
  double previous = full_distance;
  for (const float step : {0.2f, 0.5f, 0.9f}) {
    repair::DereverbClassicalConfig swept;
    swept.threshold = step;
    const double moved = distance(repair::dereverb_classical(audio, swept));
    INFO("threshold " << step << " distance ratio " << moved / full_distance);
    CHECK(moved < previous * 0.95);
    previous = moved;
  }
  CHECK(previous / full_distance > 0.05);
}

// ------------------------------------------------- single channel equals mono

TEST_CASE("one linked channel reproduces the mono path bit for bit", "[repair][stereo][denoise]") {
  const auto bed = make_bed(16384, 12345u);
  const Audio audio = as_audio(bed);
  const Audio* channels[1] = {&audio};

  std::vector<repair::DenoiseClassicalConfig> denoise_configs(4);
  denoise_configs[1].mode = repair::DenoiseMode::MmseStsa;
  denoise_configs[2].mode = repair::DenoiseMode::SpectralSubtraction;
  denoise_configs[3].noise_estimator = repair::DenoiseNoiseEstimator::Mcra;
  for (const auto& config : denoise_configs) {
    std::vector<Audio> out;
    repair::denoise_classical_linked(channels, 1, &out, config);
    REQUIRE(out.size() == 1);
    CHECK(identical(out[0], repair::denoise_classical(audio, config)));
  }

  std::vector<repair::DereverbClassicalConfig> dereverb_configs(3);
  dereverb_configs[1].wpe_enabled = true;
  dereverb_configs[2].attenuation = 0.4f;
  dereverb_configs[2].threshold = 0.2f;
  for (const auto& config : dereverb_configs) {
    std::vector<Audio> out;
    repair::dereverb_classical_linked(channels, 1, &out, config);
    REQUIRE(out.size() == 1);
    CHECK(identical(out[0], repair::dereverb_classical(audio, config)));
  }
}

// -------------------------------------------------- the report overload

TEST_CASE("asking for a report does not move a sample", "[repair][stereo][denoise]") {
  const auto bed = make_bed(16384, 12345u);
  const Audio audio = as_audio(bed);

  // The report is filled off the sample path, so the three-argument overload has
  // to return the two-argument overload's samples bit for bit -- including with
  // a null report, which is the contract the other five repair modules share.
  std::vector<repair::DenoiseClassicalConfig> denoise_configs(3);
  denoise_configs[1].mode = repair::DenoiseMode::SpectralSubtraction;
  denoise_configs[2].noise_estimator = repair::DenoiseNoiseEstimator::Mcra;
  for (const auto& config : denoise_configs) {
    const Audio plain = repair::denoise_classical(audio, config);
    repair::DenoiseReport report;
    CHECK(identical(repair::denoise_classical(audio, config, &report), plain));
    CHECK(identical(repair::denoise_classical(audio, config, nullptr), plain));
    // Without this the identity above is satisfied by an overload that ignores
    // its argument, which is the one wrong implementation it has to exclude.
    CHECK(report.detected.floor_dbfs < 0.0f);
    CHECK(report.mean_reduction_db > 0.0f);
    CHECK(report.max_reduction_db > report.mean_reduction_db);
  }

  std::vector<repair::DereverbClassicalConfig> dereverb_configs(2);
  dereverb_configs[1].wpe_enabled = true;
  for (const auto& config : dereverb_configs) {
    const Audio plain = repair::dereverb_classical(audio, config);
    repair::DereverbReport report;
    CHECK(identical(repair::dereverb_classical(audio, config, &report), plain));
    CHECK(identical(repair::dereverb_classical(audio, config, nullptr), plain));
    CHECK(report.suppressed_fraction > 0.0f);
    CHECK(report.mean_reduction_db > 0.0f);
    CHECK(report.detected.late_decay_ratio_db < 0.0f);
    // The WPE fields are the pair that shows the 0.98 clamp acting, and they are
    // zero exactly when the stage did not run.
    if (config.wpe_enabled) {
      CHECK(report.wpe_predictor_norm > 0.0f);
      CHECK(report.wpe_predictor_norm <= report.detected.late_predictability + 1.0e-6f);
    } else {
      CHECK(report.detected.late_predictability == 0.0f);
      CHECK(report.wpe_predictor_norm == 0.0f);
    }
  }
}

// ------------------------------------------------------------ interchannel

TEST_CASE("the linked repair is one linear operator on every channel",
          "[repair][stereo][denoise]") {
  // A shared real mask makes the repair a single linear map applied to each
  // channel, so it commutes with taking a difference between channels. That is
  // what "the processing cannot move an interchannel difference" means, stated
  // as an identity that survives resynthesis -- masking makes a spectrogram
  // inconsistent, so re-analyzing the output does not return the masked
  // spectrum and a cell-by-cell ratio there measures the inconsistency instead.
  const size_t size = 16384;
  std::vector<float> left = tonal_bed(size, 0.0);
  std::vector<float> right = tonal_bed(size, 0.6);
  Lcg rng(4242u);
  for (size_t i = 0; i < size; ++i) {
    left[i] += 0.04f * rng.next();
    right[i] = right[i] * 0.6f + 0.04f * rng.next();
  }
  std::vector<float> difference(size);
  for (size_t i = 0; i < size; ++i) difference[i] = left[i] - right[i];

  const Audio left_audio = as_audio(left);
  const Audio right_audio = as_audio(right);
  const Audio difference_audio = as_audio(difference);
  const Audio* channels[3] = {&left_audio, &right_audio, &difference_audio};
  std::vector<Audio> linked;
  repair::denoise_classical_linked(channels, 3, &linked);
  REQUIRE(linked.size() == 3);

  // The scale the rounding bound is taken against: the analysis rounds against
  // what goes in, and the mask only ever attenuates, so the input peak bounds
  // both halves of the round trip.
  float peak = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    peak = std::max({peak, std::abs(left[i]), std::abs(right[i]), std::abs(difference[i])});
  }
  REQUIRE(peak > 0.3f);

  double worst_linked = 0.0;
  double worst_independent = 0.0;
  // The ablation: the same gain function and the same config, one mask per
  // channel instead of one shared. Without it the bound below would pass for a
  // per-channel implementation too and prove nothing.
  const Audio independent_left = repair::denoise_classical(left_audio);
  const Audio independent_right = repair::denoise_classical(right_audio);
  const Audio independent_difference = repair::denoise_classical(difference_audio);
  for (size_t i = 0; i < size; ++i) {
    worst_linked = std::max(
        worst_linked, std::abs(static_cast<double>(linked[2][i]) - linked[0][i] + linked[1][i]));
    worst_independent =
        std::max(worst_independent, std::abs(static_cast<double>(independent_difference[i]) -
                                             independent_left[i] + independent_right[i]));
  }

  // The identity is exact in real arithmetic and rounded in this one: an STFT
  // frame sums n_fft products in float, so the residual is bounded by about
  // sqrt(n_fft) * float32 eps of the peak, twice over for analysis and
  // synthesis. 1024 and 1.19e-07 give 7.6e-06; the margin below is that.
  const double float_bound =
      2.0 * std::sqrt(1024.0) * std::numeric_limits<float>::epsilon() * static_cast<double>(peak);
  INFO("linked residual " << worst_linked << " per-channel residual " << worst_independent
                          << " float bound " << float_bound);
  CHECK(worst_linked < float_bound);
  // A per-channel repair is not one operator, and misses by orders of magnitude
  // rather than by rounding.
  CHECK(worst_independent > 100.0 * float_bound);
}

// -------------------------------------------------------------- the detectors

TEST_CASE("the noise floor detector recovers a planted floor", "[repair][stereo][denoise]") {
  const size_t size = 24000;

  // How far the reading wanders when only the noise realization is redrawn.
  // Everything below is judged against this rather than against a number copied
  // out of a passing run: a gain the draw could have produced is not a gain.
  std::vector<double> draws;
  for (uint32_t seed = 40u; seed < 46u; ++seed) {
    std::vector<float> bed = tonal_bed(size, 0.0);
    Lcg rng(seed);
    const double planted = plant_noise(&bed, &rng, 12.0);
    draws.push_back(repair::detect_noise_floor(bed.data(), bed.size(), kSampleRate).floor_dbfs -
                    planted);
  }
  double draw_mean = 0.0;
  for (const double d : draws) draw_mean += d;
  draw_mean /= static_cast<double>(draws.size());
  double draw_variance = 0.0;
  for (const double d : draws) draw_variance += (d - draw_mean) * (d - draw_mean);
  const double draw_spread = std::sqrt(draw_variance / static_cast<double>(draws.size() - 1));
  INFO("redraw spread " << draw_spread << " dB about " << draw_mean);
  REQUIRE(draw_spread < 1.0);

  const double snrs[] = {18.0, 12.0, 6.0};
  std::vector<double> reads;
  std::vector<double> planted_levels;
  std::vector<double> programme_levels;
  for (size_t i = 0; i < 3; ++i) {
    std::vector<float> bed = tonal_bed(size, 0.0);
    Lcg rng(static_cast<uint32_t>(11 + i));
    const double planted = plant_noise(&bed, &rng, snrs[i]);
    const double programme = 10.0 * std::log10(mean_square(bed));
    const repair::NoiseDetection detection =
        repair::detect_noise_floor(bed.data(), bed.size(), kSampleRate);
    reads.push_back(detection.floor_dbfs);
    planted_levels.push_back(planted);
    programme_levels.push_back(programme);

    INFO("snr " << snrs[i] << " programme " << programme << " planted " << planted << " read "
                << detection.floor_dbfs);
    // The two values a wrong implementation lands on are both named here: an
    // implementation that summed the spectrum without the noise estimator
    // returns the programme level, and one that returned its field's default
    // returns 0. The reading has to sit in the bottom quarter of the span
    // between them instead.
    const double span = programme - planted;
    REQUIRE(span > 5.0);
    CHECK(std::abs(detection.floor_dbfs - planted) < 0.25 * span);
    CHECK(detection.floor_dbfs != 0.0f);
    CHECK(detection.floor_dbfs < programme - 0.5 * span);

    // Every band above the analysis floor carries part of the reading, and the
    // bands have to sum back to it -- the shape is a decomposition of the
    // broadband level, not a second measurement of it.
    double band_power = 0.0;
    int populated = 0;
    for (size_t k = 0; k < repair::kRepairNoiseBandCount; ++k) {
      if (detection.band_floor_dbfs[k] <= sonare::constants::kFloorDb) continue;
      ++populated;
      band_power += std::pow(10.0, 0.1 * detection.band_floor_dbfs[k]);
    }
    CHECK(populated > 20);
    CHECK(10.0 * std::log10(band_power) == Approx(detection.floor_dbfs).margin(1.0e-3));
  }

  // With no programme on top there is no tonal residue to bias the estimate, so
  // the reading is the planted level. Stated before the run rather than read off
  // it: the bound is the redraw spread measured above, which is the only source
  // of movement left once the programme is gone.
  for (uint32_t seed = 60u; seed < 63u; ++seed) {
    std::vector<float> only_noise(size);
    Lcg rng(seed);
    for (size_t i = 0; i < size; ++i) only_noise[i] = 0.02f * rng.next_gaussian();
    const double planted = 10.0 * std::log10(mean_square(only_noise));
    const float read =
        repair::detect_noise_floor(only_noise.data(), only_noise.size(), kSampleRate).floor_dbfs;
    // Not the planted level: the estimator averages the quietest tenth of the
    // frames, and the quietest tenth of a random floor sits below its own mean.
    // How far below is a property of this signal, so the test computes it from
    // the signal rather than carrying a number.
    const double expected = planted + quietest_decile_offset_db(only_noise);
    INFO("noise alone: planted " << planted << " quietest-decile " << expected << " read " << read);
    CHECK(read == Approx(expected).margin(3.0 * draw_spread));
  }

  // Tracking, with the separation the per-item bounds already guarantee rather
  // than a margin chosen here: two readings whose errors are each under a
  // quarter of their own span cannot be closer than the planted gap less those
  // two quarters. An implementation returning any constant collapses this to 0.
  const double planted_gap = planted_levels[2] - planted_levels[0];
  const double guaranteed = planted_gap - 0.25 * (programme_levels[0] - planted_levels[0]) -
                            0.25 * (programme_levels[2] - planted_levels[2]);
  INFO("planted gap " << planted_gap << " read gap " << reads[2] - reads[0] << " guaranteed "
                      << guaranteed);
  REQUIRE(guaranteed > 0.0);
  CHECK(reads[2] - reads[0] > guaranteed);
}

TEST_CASE("the noise floor detector hits the corpus's planted quantity",
          "[repair][stereo][denoise]") {
  const std::string manifest_path = "tools/mastering-eval/audio/manifest.json";
  std::ifstream manifest_file(manifest_path);
  if (!manifest_file) SKIP("evaluation corpus not generated (tools/mastering-eval/corpus.py)");
  std::stringstream buffer;
  buffer << manifest_file.rdbuf();
  const sonare::util::json::Value manifest = sonare::util::json::parse(buffer.str());

  int checked = 0;
  for (const auto& item : manifest["items"].as_array()) {
    if (!item.contains("defects") || !item["defects"].contains("noise")) continue;
    // A row carrying more than noise has a floor made of every defect it holds,
    // which the noise sub-record does not describe.
    if (item["defects"].as_object().size() != 1) continue;
    const double planted = item["defects"]["noise"]["noise_floor_dbfs"].as_number();

    auto [samples, rate, channels] =
        sonare::load_audio_interleaved("tools/mastering-eval/audio/" + item["audio"].as_string());
    REQUIRE(channels >= 1);
    const size_t frames = samples.size() / static_cast<size_t>(channels);
    for (int c = 0; c < channels; ++c) {
      std::vector<float> channel(frames);
      for (size_t i = 0; i < frames; ++i) {
        channel[i] = samples[i * static_cast<size_t>(channels) + static_cast<size_t>(c)];
      }
      const float read =
          repair::detect_noise_floor(channel.data(), channel.size(), rate).floor_dbfs;
      const double programme = 10.0 * std::log10(mean_square(channel));
      INFO(item["id"].as_string() << " ch" << c << " programme " << programme << " planted "
                                  << planted << " read " << read);
      // Derived rather than toleranced: the reading has to be nearer the floor
      // the generator planted than the level of the programme sitting on top of
      // it. An implementation that summed the spectrum and skipped the noise
      // estimator returns the programme level and fails this on every row.
      CHECK(std::abs(read - planted) < std::abs(read - programme));
      // The residual is the module's estimator, not the report: selecting the
      // quietest frames of a 1/f floor picks its low-energy realizations, which
      // reads about 4 dB low on the two pink rows and under 0.5 dB on the white
      // ones. Bound set above the worst of those rather than fitted to it.
      CHECK(std::abs(read - planted) < 6.0);
      ++checked;
    }
  }
  CHECK(checked >= 8);
}

TEST_CASE("the reverb detector separates a tail from a dry offset", "[repair][stereo][denoise]") {
  // A decaying note pair, dry and through an exponential tail. The tail is the
  // same decay law dereverb_classical assumes when it turns t60_sec into a
  // late-power estimate.
  const size_t size = 48000;
  std::vector<float> dry(size, 0.0f);
  for (size_t note = 0; note < 6; ++note) {
    const size_t onset = note * 8000;
    for (size_t i = 0; i < 4000 && onset + i < size; ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      dry[onset + i] += static_cast<float>(0.5 * std::exp(-40.0 * t) *
                                           std::sin(kTwoPi * (220.0 + 40.0 * note) * t));
    }
  }

  // Four mutually prime comb delays, each fed back at the gain that puts it 60 dB
  // down after t60. A recursion rather than a convolution: the same exponential
  // tail for four multiply-adds a sample instead of a full impulse response.
  const auto reverberate = [&](double t60) {
    const size_t delays[4] = {1237, 1601, 2053, 2411};
    std::vector<float> wet(dry.begin(), dry.end());
    for (const size_t delay : delays) {
      const float feedback = static_cast<float>(
          std::pow(10.0, -3.0 * static_cast<double>(delay) / (kSampleRate * t60)));
      for (size_t i = delay; i < wet.size(); ++i) wet[i] += feedback * wet[i - delay];
    }
    float peak = 0.0f;
    for (const float v : wet) peak = std::max(peak, std::abs(v));
    for (float& v : wet) v = v / peak * 0.8f;
    return wet;
  };

  const std::vector<float> short_tail = reverberate(0.35);
  const std::vector<float> long_tail = reverberate(1.2);

  const float dry_decay =
      repair::detect_reverb(dry.data(), dry.size(), kSampleRate).late_decay_ratio_db;
  const float short_decay =
      repair::detect_reverb(short_tail.data(), short_tail.size(), kSampleRate).late_decay_ratio_db;
  const float long_decay =
      repair::detect_reverb(long_tail.data(), long_tail.size(), kSampleRate).late_decay_ratio_db;

  INFO("dry " << dry_decay << " short " << short_decay << " long " << long_decay);
  // A tail sustains across the module's lag and a dry offset does not, so the
  // ratio rises with reverberation. Three values, each pair separated: an
  // implementation returning one constant, or the same value twice, fails here.
  CHECK(dry_decay < short_decay - 2.0f);
  CHECK(short_decay < long_decay - 1.0f);
  CHECK(dry_decay < 0.0f);

  // Predictability is zero unless the WPE stage ran, and rises with the tail.
  repair::DereverbClassicalConfig wpe;
  wpe.wpe_enabled = true;
  CHECK(
      repair::detect_reverb(long_tail.data(), long_tail.size(), kSampleRate).late_predictability ==
      0.0f);
  const float dry_predictability =
      repair::detect_reverb(dry.data(), dry.size(), kSampleRate, wpe).late_predictability;
  const float long_predictability =
      repair::detect_reverb(long_tail.data(), long_tail.size(), kSampleRate, wpe)
          .late_predictability;
  INFO("predictability dry " << dry_predictability << " long " << long_predictability);
  CHECK(long_predictability > dry_predictability);
  // Recorded before the cap, so it is free to sit above it.
  CHECK(long_predictability > 0.0f);
}

// ---------------------------------------------------------------- the reports

TEST_CASE("the denoise report describes the mask that ran", "[repair][stereo][denoise]") {
  std::vector<float> left = tonal_bed(48000, 0.0);
  std::vector<float> right = tonal_bed(48000, 0.6);
  Lcg rng_left(31u);
  Lcg rng_right(32u);
  plant_noise(&left, &rng_left, 12.0);
  plant_noise(&right, &rng_right, 12.0);

  repair::DenoiseClassicalConfig config;
  const repair::DenoiseReport report =
      repair::denoise_classical_stereo(as_audio(left), as_audio(right), config).report;

  const float floor_limit_db = config.reduction_db;
  INFO("mean " << report.mean_reduction_db << " max " << report.max_reduction_db << " at-floor "
               << report.floor_limited_fraction);
  CHECK(report.mean_reduction_db > 1.0f);
  // The mask cannot cut past its own floor, so the deepest cut is that floor
  // exactly rather than merely "some large number".
  CHECK(report.max_reduction_db == Approx(floor_limit_db).margin(1.0e-3));
  CHECK(report.floor_limited_fraction > 0.0f);
  CHECK(report.floor_limited_fraction < 1.0f);
  CHECK(report.detected.floor_dbfs < 0.0f);

  // A deeper floor deepens the deepest cut by exactly the floor's own change.
  repair::DenoiseClassicalConfig deeper = config;
  deeper.reduction_db = 40.0f;
  const repair::DenoiseReport deeper_report =
      repair::denoise_classical_stereo(as_audio(left), as_audio(right), deeper).report;
  CHECK(deeper_report.max_reduction_db == Approx(deeper.reduction_db).margin(1.0e-3));
  CHECK(deeper_report.mean_reduction_db > report.mean_reduction_db);

  // Berouti has no gain floor, so the fraction is defined to be zero there
  // rather than left at whatever the other branch would have counted.
  repair::DenoiseClassicalConfig berouti = config;
  berouti.mode = repair::DenoiseMode::SpectralSubtraction;
  const repair::DenoiseReport berouti_report =
      repair::denoise_classical_stereo(as_audio(left), as_audio(right), berouti).report;
  CHECK(berouti_report.floor_limited_fraction == 0.0f);
}

TEST_CASE("the dereverb report describes the mask that ran", "[repair][stereo][denoise]") {
  const auto bed = make_bed(48000, 555u);
  const Audio audio = as_audio(bed);

  repair::DereverbClassicalConfig config;
  const repair::DereverbReport all = repair::dereverb_classical_stereo(audio, audio, config).report;
  // threshold 0 admits every cell that carries any late energy, which is every
  // cell past the lag. Counting them is what makes the knob observable.
  INFO("suppressed " << all.suppressed_fraction << " mean " << all.mean_reduction_db);
  CHECK(all.suppressed_fraction > 0.9f);
  CHECK(all.mean_reduction_db > 0.0f);

  repair::DereverbClassicalConfig gated = config;
  gated.threshold = 0.5f;
  const repair::DereverbReport some = repair::dereverb_classical_stereo(audio, audio, gated).report;
  CHECK(some.suppressed_fraction < all.suppressed_fraction);
  CHECK(some.mean_reduction_db < all.mean_reduction_db);

  // threshold 1 admits only a cell whose lagged power, after the t60 decay, still
  // exceeds its own -- a drop of more than 14 dB across the lag at the default
  // geometry. That is a few percent of a steady signal, not none of it: the knob
  // is relative to each bin's own power, so no finite setting closes it outright.
  repair::DereverbClassicalConfig closed = config;
  closed.threshold = 1.0f;
  const repair::DereverbReport none =
      repair::dereverb_classical_stereo(audio, audio, closed).report;
  INFO("closed " << none.suppressed_fraction << " gated " << some.suppressed_fraction);
  CHECK(none.suppressed_fraction < some.suppressed_fraction);
  CHECK(none.suppressed_fraction < 0.1f);
  CHECK(none.mean_reduction_db < some.mean_reduction_db);
  CHECK(none.mean_reduction_db > 0.0f);

  repair::DereverbClassicalConfig wpe = config;
  wpe.wpe_enabled = true;
  const repair::DereverbReport with_wpe =
      repair::dereverb_classical_stereo(audio, audio, wpe).report;
  CHECK(with_wpe.wpe_predictor_norm > 0.0f);
  // The cap is a silent branch; the pair is the only way to see it act.
  CHECK(with_wpe.wpe_predictor_norm <= with_wpe.detected.late_predictability + 1.0e-6f);
  CHECK(all.wpe_predictor_norm == 0.0f);
}

// ------------------------------------- linked mask against the transfer ratio

TEST_CASE("the linked mask beats the time-domain transfer ratio", "[repair][stereo][denoise]") {
  // Bands with different SNR per channel: a single per-sample ratio scales every
  // band by the same amount, so it cannot follow a floor that varies with
  // frequency the way a per-bin mask does. Bursts with silence between them, so the quietest frames
  // the noise estimator selects are noise and not the programme: on a bed that never stops it
  // subtracts the bed, and both strategies then damage the signal rather than
  // repair it, which is not a comparison.
  const size_t size = 48000;
  std::vector<float> clean_left(size, 0.0f);
  std::vector<float> clean_right(size, 0.0f);
  for (size_t note = 0; note < 6; ++note) {
    const size_t onset = note * 8000;
    for (size_t i = 0; i < 5000 && onset + i < size; ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      const double envelope = std::min(1.0, static_cast<double>(i) / 400.0) * std::exp(-3.0 * t);
      clean_left[onset + i] += static_cast<float>(
          envelope * (0.42 * std::sin(kTwoPi * 300.0 * t) + 0.14 * std::sin(kTwoPi * 3000.0 * t)));
      clean_right[onset + i] +=
          static_cast<float>(envelope * (0.25 * std::sin(kTwoPi * 300.0 * t + 0.7) +
                                         0.31 * std::sin(kTwoPi * 3000.0 * t + 0.4)));
    }
  }

  Lcg rng(8080u);
  std::vector<float> dirty_left = clean_left;
  std::vector<float> dirty_right = clean_right;
  // A tilted floor: most of the noise power sits under 1 kHz.
  float low_state_left = 0.0f;
  float low_state_right = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    low_state_left = 0.96f * low_state_left + 0.04f * rng.next_gaussian();
    low_state_right = 0.96f * low_state_right + 0.04f * rng.next_gaussian();
    dirty_left[i] += 0.35f * low_state_left + 0.006f * rng.next_gaussian();
    dirty_right[i] += 0.35f * low_state_right + 0.006f * rng.next_gaussian();
  }

  // The strategy in the chain today: repair the mono sum, then apply the
  // per-sample transfer ratio to both channels.
  std::vector<float> transfer_left = dirty_left;
  std::vector<float> transfer_right = dirty_right;
  sonare::mastering::api::detail::apply_shared_mono_transfer_repair(
      transfer_left, transfer_right, kSampleRate,
      [](const Audio& mono) { return repair::denoise_classical(mono); });

  const repair::DenoiseStereoResult linked =
      repair::denoise_classical_stereo(as_audio(dirty_left), as_audio(dirty_right));
  const Audio& linked_left = linked.left;
  const Audio& linked_right = linked.right;

  const auto score = [&](const std::vector<float>& left, const std::vector<float>& right) {
    return std::pair<double, double>{
        0.5 * (metrics::segmental_snr(clean_left, left, kSampleRate) +
               metrics::segmental_snr(clean_right, right, kSampleRate)),
        0.5 * (metrics::log_spectral_distance(clean_left, left, kSampleRate) +
               metrics::log_spectral_distance(clean_right, right, kSampleRate))};
  };

  const auto dirty_score = score(dirty_left, dirty_right);
  const auto transfer_score = score(transfer_left, transfer_right);
  const auto linked_score = score(to_vector(linked_left), to_vector(linked_right));

  // What "the stereo image moved" means: the part of the error that is not
  // common to the two channels. A repair that treats the channels differently
  // leaves a difference the input did not have.
  const auto image_error = [&](const std::vector<float>& left, const std::vector<float>& right) {
    double sum = 0.0;
    for (size_t i = 0; i < size; ++i) {
      const double difference = (static_cast<double>(left[i]) - clean_left[i]) -
                                (static_cast<double>(right[i]) - clean_right[i]);
      sum += difference * difference;
    }
    return std::sqrt(sum / static_cast<double>(size));
  };

  // The third strategy, for the ablation: the same denoiser run per channel.
  // Removing the link is what the comparison is about, so the unlinked variant
  // has to be present for the judgement to be capable of failing.
  Audio independent_left = repair::denoise_classical(as_audio(dirty_left));
  Audio independent_right = repair::denoise_classical(as_audio(dirty_right));
  const auto independent_score = score(to_vector(independent_left), to_vector(independent_right));

  INFO("segSNR dirty " << dirty_score.first << " transfer " << transfer_score.first
                       << " independent " << independent_score.first << " linked "
                       << linked_score.first);
  INFO("LSD dirty " << dirty_score.second << " transfer " << transfer_score.second
                    << " independent " << independent_score.second << " linked "
                    << linked_score.second);
  INFO("image transfer " << image_error(transfer_left, transfer_right) << " independent "
                         << image_error(to_vector(independent_left), to_vector(independent_right))
                         << " linked "
                         << image_error(to_vector(linked_left), to_vector(linked_right)));

  // The linked mask is a repair: it beats doing nothing on both metrics. Stated
  // first because the comparison below is only worth reading once this holds.
  CHECK(linked_score.first > dirty_score.first);
  CHECK(linked_score.second < dirty_score.second);

  // The comparison the strategy was chosen on, against what the chain does
  // today rather than against per-channel processing.
  CHECK(linked_score.first > transfer_score.first);
  CHECK(linked_score.second < transfer_score.second);

  // The image error the transfer ratio leaves is 6x the linked mask's, because
  // one gain per sample is a compromise across every band at once. Per-channel
  // processing is recorded above rather than asserted on: on material whose two
  // channels carry the same noise process it lands within 0.05% of the linked
  // mask here, which is not a difference a check can stand on. The ablation
  // that does separate them is the linear-operator identity, which per-channel
  // processing misses by five orders of magnitude.
  CHECK(image_error(to_vector(linked_left), to_vector(linked_right)) <
        0.25 * image_error(transfer_left, transfer_right));

  // Recorded rather than assumed: on this material the transfer ratio is worse
  // than not processing at all on both metrics, because one gain per sample
  // scales every band by whatever the loudest band needed.
  CHECK(transfer_score.first < dirty_score.first);
  CHECK(transfer_score.second > dirty_score.second);
}

TEST_CASE("noise band bins cover the spectrum without overlapping", "[repair][stereo][denoise]") {
  int edges[common::kRepairNoiseBandCount + 1] = {};
  common::repair_noise_band_bins(1024, kSampleRate, edges);
  CHECK(edges[common::kRepairNoiseBandCount] == 513);
  for (size_t k = 0; k < common::kRepairNoiseBandCount; ++k) {
    CHECK(edges[k] <= edges[k + 1]);
    CHECK(edges[k] >= 0);
  }
  // 20 Hz is under one bin at this geometry, so the first edge is bin 0.
  CHECK(edges[0] == 0);
}
