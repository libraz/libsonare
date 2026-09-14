// Cross-language agreement cases for the restoration quality metrics
// (segmental SNR, log kurtosis ratio, log-spectral distance). The C++ side of
// each metric and its Python counterpart must agree on the same fixed vectors.
#include "repair_metrics.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "util/constants.h"

namespace {

namespace metrics = sonare::test::repair_metrics;

// The same 64 samples appear in metrics_repair._agreement_case(). Nine significant digits
// round-trip through float32, so both languages read identical samples.
const std::vector<float> kAgreementClean = {
    0.0973545834f, 0.448020369f,  0.56344223f,   0.47747317f,   0.364068449f,  0.375386655f,
    0.504074693f,  0.589327276f,  0.461526036f,  0.0999566093f, -0.333829224f, -0.617625177f,
    -0.642323256f, -0.490006953f, -0.350897968f, -0.352267146f, -0.447759628f, -0.462734759f,
    -0.255865842f, 0.14372994f,   0.543952942f,  0.737424135f,  0.664114833f,  0.450502753f,
    0.292962402f,  0.286892831f,  0.349193305f,  0.300413966f,  0.0344605222f, -0.372338444f,
    -0.708984494f, -0.795319438f, -0.62443471f,  -0.360641122f, -0.195695758f, -0.187579736f,
    -0.220440716f, -0.119085349f, 0.182488576f,  0.565995574f,  0.814145982f,  0.784704745f,
    0.524722576f,  0.227377817f,  0.0689707547f, 0.0662896782f, 0.0763010159f, -0.0632447079f,
    -0.375648975f, -0.708166659f, -0.849993348f, -0.705163658f, -0.372234195f, -0.0623646826f,
    0.0738869458f, 0.0626645535f, 0.0672626123f, 0.229060575f,  0.528344214f,  0.78714025f,
    0.813293815f,  0.562591016f,  0.179469377f,  -0.119114019f};

const std::vector<float> kAgreementProcessed = {
    0.0981206074f, 0.4555628f,    0.566838264f,  0.457478046f,  0.356876701f,  0.352674395f,
    0.462118f,     0.567563117f,  0.439797133f,  0.104728535f,  -0.325172186f, -0.588777661f,
    -0.612557292f, -0.463409185f, -0.334250271f, -0.349844068f, -0.423265785f, -0.419098258f,
    -0.23194252f,  0.149016172f,  0.529353619f,  0.721015871f,  0.630402327f,  0.438146919f,
    0.281121731f,  0.310348839f,  0.382469416f,  0.31563431f,   0.0406418927f, -0.349331141f,
    -0.678894639f, -0.749402463f, -0.607337296f, -0.34372589f,  -0.177198187f, -0.171282947f,
    -0.205833301f, -0.101602025f, 0.201786071f,  0.57286793f,   0.797753453f,  0.763524115f,
    0.503403604f,  0.221326634f,  0.0705853254f, 0.0294230841f, 0.0722776428f, -0.0742331371f,
    -0.378703684f, -0.700963557f, -0.84074825f,  -0.691183209f, -0.37364161f,  -0.0581244342f,
    0.0889330357f, 0.0252746921f, 0.0420315862f, 0.198966727f,  0.496846795f,  0.749709249f,
    0.77744627f,   0.563504279f,  0.17052041f,   -0.115976125f};

constexpr double kAgreementSampleRate = 8000.0;
constexpr int kAgreementFrameLength = 16;
constexpr int kAgreementHopLength = 8;

// Both halves accumulate in double and differ only in transform and summation order: on these
// vectors the gap measures 0 for segmental SNR and at most 7e-15 for the spectral pair. 1e-9 keeps
// five orders of headroom and is still far tighter than a difference of definition could be.
constexpr double kAgreementTolerance = 1e-9;

// Values produced by tools/mastering-eval/metrics_repair.py on the vectors above.
constexpr double kPythonSegmentalSnr = 26.831584635410575;
constexpr double kPythonLogKurtosisRatio = 1.0030271740299053;
constexpr double kPythonLogSpectralDistance = 10.275617136869082;

// A gain that is a power of two scales every float sample exactly, so any movement in a
// gain-invariant metric is the metric's own, not the probe's rounding.
constexpr float kExactGain = 0.5f;
constexpr float kInexactGain = 0.7f;

// A float32 product carries 2^-24 relative error into every sample, and that reaches the metrics as
// under 1e-6 here; a gain read as quality would instead move the distance by 20*log10(0.7), so this
// threshold sits five orders below what it is meant to catch. The exact gain moves nothing at all.
constexpr double kInexactGainTolerance = 1e-5;
constexpr double kExactGainTolerance = 1e-12;

/// Deterministic test material: two partials plus a slow amplitude drift.
std::vector<float> make_clean(int samples, double sample_rate) {
  std::vector<float> out(static_cast<std::size_t>(samples));
  for (std::size_t n = 0; n < out.size(); ++n) {
    const double t = static_cast<double>(n) / sample_rate;
    const double envelope = 0.6 + 0.4 * std::sin(sonare::constants::kTwoPiD * 1.5 * t);
    out[n] =
        static_cast<float>(envelope * (0.5 * std::sin(sonare::constants::kTwoPiD * 220.0 * t) +
                                       0.2 * std::sin(sonare::constants::kTwoPiD * 930.0 * t)));
  }
  return out;
}

/// Reproducible white noise from a fixed 32-bit LCG, so a failure is the metric's and not a seed's.
std::vector<float> add_noise(const std::vector<float>& signal, float amplitude, uint32_t seed) {
  std::vector<float> out = signal;
  uint32_t state = seed;
  for (float& sample : out) {
    state = state * 1664525u + 1013904223u;
    const float uniform = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
    sample += amplitude * uniform;
  }
  return out;
}

std::vector<float> scaled(const std::vector<float>& signal, float gain) {
  std::vector<float> out = signal;
  for (float& sample : out) sample *= gain;
  return out;
}

}  // namespace

TEST_CASE("Restoration metrics match the Python half on the fixed vectors", "[repair][metrics]") {
  const double seg_snr =
      metrics::segmental_snr(kAgreementClean, kAgreementProcessed, kAgreementSampleRate,
                             kAgreementFrameLength, kAgreementHopLength);
  const double kurtosis_ratio =
      metrics::log_kurtosis_ratio(kAgreementClean, kAgreementProcessed, kAgreementSampleRate,
                                  kAgreementFrameLength, kAgreementHopLength);
  const double distance =
      metrics::log_spectral_distance(kAgreementClean, kAgreementProcessed, kAgreementSampleRate,
                                     kAgreementFrameLength, kAgreementHopLength);

  INFO("segmental SNR " << seg_snr << " vs Python " << kPythonSegmentalSnr);
  REQUIRE(std::abs(seg_snr - kPythonSegmentalSnr) < kAgreementTolerance);
  INFO("log kurtosis ratio " << kurtosis_ratio << " vs Python " << kPythonLogKurtosisRatio);
  REQUIRE(std::abs(kurtosis_ratio - kPythonLogKurtosisRatio) < kAgreementTolerance);
  INFO("log-spectral distance " << distance << " vs Python " << kPythonLogSpectralDistance);
  REQUIRE(std::abs(distance - kPythonLogSpectralDistance) < kAgreementTolerance);

  // The saturation counts are integers, so the two languages agree exactly or not at all.
  const auto report =
      metrics::segmental_snr_report(kAgreementClean, kAgreementProcessed, kAgreementSampleRate,
                                    kAgreementFrameLength, kAgreementHopLength);
  REQUIRE(report.active_frames == 7);
  REQUIRE(report.ceiling_frames == 0);
  REQUIRE(report.floor_frames == 0);
  REQUIRE(report.value == seg_snr);
}

TEST_CASE("An identity output reads as no damage on all three metrics", "[repair][metrics]") {
  const double sample_rate = 16000.0;
  const std::vector<float> clean = make_clean(8192, sample_rate);

  REQUIRE(metrics::segmental_snr(clean, clean, sample_rate) == metrics::kSegSnrCeilingDb);
  REQUIRE(metrics::log_kurtosis_ratio(clean, clean, sample_rate) == 1.0);
  REQUIRE(metrics::log_spectral_distance(clean, clean, sample_rate) == 0.0);
}

TEST_CASE("Segmental SNR falls as noise is added to the output", "[repair][metrics]") {
  const double sample_rate = 16000.0;
  const std::vector<float> clean = make_clean(8192, sample_rate);
  const std::vector<float> quiet_noise = add_noise(clean, 0.01f, 12345u);
  const std::vector<float> loud_noise = add_noise(clean, 0.10f, 12345u);

  const double clean_snr = metrics::segmental_snr(clean, clean, sample_rate);
  const double quiet_snr = metrics::segmental_snr(clean, quiet_noise, sample_rate);
  const double loud_snr = metrics::segmental_snr(clean, loud_noise, sample_rate);

  INFO("segmental SNR: identity " << clean_snr << ", +0.01 " << quiet_snr << ", +0.10 "
                                  << loud_snr);
  REQUIRE(quiet_snr < clean_snr);
  REQUIRE(loud_snr < quiet_snr);
}

TEST_CASE("Segmental SNR declares when the clip is carrying its value", "[repair][metrics]") {
  const double sample_rate = 16000.0;
  const std::vector<float> clean = make_clean(8192, sample_rate);

  SECTION("an identity output puts every frame on the ceiling") {
    const auto report = metrics::segmental_snr_report(clean, clean, sample_rate);
    REQUIRE(report.value == metrics::kSegSnrCeilingDb);
    REQUIRE(report.saturated());
    REQUIRE(report.ceiling_frames == report.active_frames);
    REQUIRE(report.ceiling_fraction() == 1.0);
    REQUIRE(report.floor_frames == 0);
  }

  SECTION("noise that clears the ceiling is not declared saturated") {
    const auto report =
        metrics::segmental_snr_report(clean, add_noise(clean, 0.10f, 12345u), sample_rate);
    REQUIRE_FALSE(report.saturated());
    REQUIRE(report.ceiling_frames == 0);
    REQUIRE(report.active_frames > 0);
  }

  // Between the two: a row the ceiling is partly hiding reads as a fraction, not as a flag.
  SECTION("a partly clipped row is not saturated but reports its share") {
    const auto report =
        metrics::segmental_snr_report(clean, add_noise(clean, 0.01f, 12345u), sample_rate);
    REQUIRE_FALSE(report.saturated());
    REQUIRE(report.ceiling_fraction() > 0.0);
    REQUIRE(report.ceiling_fraction() < 1.0);
  }

  SECTION("the bare metric reports the same value as the report") {
    const std::vector<float> processed = add_noise(clean, 0.02f, 987654321u);
    REQUIRE(metrics::segmental_snr(clean, processed, sample_rate) ==
            metrics::segmental_snr_report(clean, processed, sample_rate).value);
  }
}

TEST_CASE("A pure output gain moves neither the kurtosis ratio nor the distance",
          "[repair][metrics]") {
  const double sample_rate = 16000.0;
  const std::vector<float> clean = make_clean(8192, sample_rate);
  const std::vector<float> processed = add_noise(clean, 0.02f, 987654321u);

  const double kurtosis_ratio = metrics::log_kurtosis_ratio(clean, processed, sample_rate);
  const double distance = metrics::log_spectral_distance(clean, processed, sample_rate);

  SECTION("a gain the float format represents exactly") {
    const std::vector<float> gained = scaled(processed, kExactGain);
    REQUIRE(std::abs(metrics::log_kurtosis_ratio(clean, gained, sample_rate) - kurtosis_ratio) <
            kExactGainTolerance);
    REQUIRE(std::abs(metrics::log_spectral_distance(clean, gained, sample_rate) - distance) <
            kExactGainTolerance);

    // The probe is not a no-op: the same gain moves the metric that is meant to see level.
    const double gained_snr = metrics::segmental_snr(clean, gained, sample_rate);
    const double ungained_snr = metrics::segmental_snr(clean, processed, sample_rate);
    INFO("segmental SNR " << ungained_snr << " -> " << gained_snr << " under a " << kExactGain
                          << " gain");
    REQUIRE(gained_snr < ungained_snr - 1.0);
  }

  SECTION("a gain that rounds every sample") {
    const std::vector<float> gained = scaled(processed, kInexactGain);
    REQUIRE(std::abs(metrics::log_kurtosis_ratio(clean, gained, sample_rate) - kurtosis_ratio) <
            kInexactGainTolerance);
    REQUIRE(std::abs(metrics::log_spectral_distance(clean, gained, sample_rate) - distance) <
            kInexactGainTolerance);
  }
}
