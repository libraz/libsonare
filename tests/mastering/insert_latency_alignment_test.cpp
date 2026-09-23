/// @file insert_latency_alignment_test.cpp
/// @brief Impulse-alignment guard for every processor the insert factory builds.
///
/// latency_samples() is what a host compensates an insert by and what the
/// offline runner trims from the head of a rendered buffer, so it has to be the
/// delay a caller actually observes — including delay contributed by a
/// sub-processor the class merely holds. The check is driven from
/// insert_factory_names() rather than a hand-written list so a new insert is
/// covered the day it is registered, and from each insert's published choices so
/// a configuration that changes the delay (an oversampled path, a lookahead) is
/// measured against what insert_timing() reports for it.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/tube.h"
#include "rt/processor_base.h"
#include "util/json.h"

namespace {

using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::insert_probe_params;
using sonare::mastering::api::insert_timing;
using sonare::mastering::api::make_insert;
using sonare::mastering::multiband::MultibandSaturation;
using sonare::mastering::multiband::MultibandSaturationConfig;
using sonare::mastering::multiband::SaturationType;
using sonare::rt::ProcessorBase;
namespace json = sonare::util::json;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kNumChannels = 2;
constexpr int kNumBlocks = 16;
// A little way into the first block, so a linear-phase response has room for its
// pre-ringing and a carrier/LFO-driven stage is past its zero start phase.
constexpr int kImpulseIndex = 128;
// Well under every default ceiling and threshold, so limiters and clippers act
// as pure delays instead of reshaping the impulse.
constexpr float kImpulseAmplitude = 0.5f;
// Arrival floor, relative to the response peak (-60 dB).
constexpr float kOnsetFraction = 1.0e-3f;
// Below this the processor emitted nothing and no arrival can be read at all.
constexpr float kMeasurableFloor = 1.0e-9f;
// Samples within this fraction of the peak belong to a flat-topped peak.
constexpr float kPlateauFraction = 1.0e-5f;

/// Impulse response summary, with both indices expressed as a lag against the
/// input impulse.
struct Response {
  float peak = 0.0f;
  int peak_lag = 0;
  int onset_lag = 0;
  // Last sample of the run starting at the peak that stays level with it.
  int plateau_end_lag = 0;
};

/// One exempted configuration: @p key set to @p value on insert @p name. A "*"
/// in either matches any run, so "*" alone also covers the default configuration.
struct Exemption {
  std::string name;
  std::string key;
  std::string value;
  std::string reason;
};

/// One measured configuration. The default one has an empty key and value.
struct Configuration {
  std::string name;
  std::string key;
  std::string value;
  std::string json;
};

/// Configurations whose impulse-response peak is not their I/O latency: a
/// resonant or steep filter shape moves the peak by its own group delay, which
/// is phase response rather than a delay a host should compensate. The
/// arrival/peak bracket below still applies to them.
const std::vector<Exemption>& PeakShiftedConfigurations() {
  static const std::vector<Exemption> kRows = {
      // A fully-wet 1 kHz band-pass: the resonator peaks a quarter period in.
      {"eq.bandPass", "*", "*", "resonator group delay"},
      // The 4x12 cab roll-off adds its own group delay after the tube stage.
      {"saturation.ampSim", "*", "*", "cab roll-off group delay"},
      // Both drive a magnetization state rather than passing the input through,
      // so an impulse produces a decaying step and not a matching impulse: the
      // response arrives at the right sample, then holds a long plateau whose
      // largest excursion is placed by the relaxation envelope rather than by
      // any delay. The arrival/peak bracket below is the part that still says
      // something about latency for these two.
      {"saturation.tape", "*", "*", "magnetization plateau"},
      {"saturation.transformer", "*", "*", "magnetization plateau"},
      // The crossover spreads the impulse across each band's filter response
      // rather than one sample, and the top band's envelope follower starts
      // cold, so it under-reads the arrival sample and expands it down harder
      // than the smaller sample that follows.
      {"multiband.expander", "*", "*", "crossover spread with a cold top-band follower"},
      // A low-pass or band-pass band delays its peak by the filter's own group
      // delay; the dynamic EQ's all-pass band rotates it by the same kind.
      {"eq.parametric", "band*.type", "3", "low-pass group delay"},
      {"eq.parametric", "band*.type", "5", "band-pass group delay"},
      {"eq.equalizer", "band*.type", "3", "low-pass group delay"},
      {"eq.equalizer", "band*.type", "5", "band-pass group delay"},
      {"eq.dynamic", "band*.type", "3", "low-pass group delay"},
      {"eq.dynamic", "band*.type", "5", "band-pass group delay"},
      {"eq.minimumPhase", "band*.type", "3", "low-pass group delay"},
      {"eq.midSide", "midBand*.type", "3", "low-pass group delay"},
      {"eq.midSide", "midBand*.type", "5", "band-pass group delay"},
      {"multiband.dynamicEq", "band*.dyn*.type", "3", "low-pass group delay"},
      {"multiband.dynamicEq", "band*.dyn*.type", "5", "band-pass group delay"},
      {"multiband.dynamicEq", "band*.dyn*.type", "9", "all-pass group delay"},
      // An LR8 crossover's steeper bands no longer sum to a single-sample peak.
      {"multiband.compressor", "slope", "2", "LR8 crossover group delay"},
      {"multiband.limiter", "slope", "2", "LR8 crossover group delay"},
      {"multiband.imager", "slope", "2", "LR8 crossover group delay"},
      {"multiband.saturation", "slope", "2", "LR8 crossover group delay"},
      {"multiband.dynamicEq", "slope", "2", "LR8 crossover group delay"},
      {"saturation.multibandExciter", "slope", "2", "LR8 crossover group delay"},
      // A tube top band's gain breaks the flat band sum, so the sum peaks where
      // that filtered band does. Band alignment is the multiband case below.
      {"multiband.saturation", "band2.type", "2", "tube band breaks the flat band sum"},
#ifdef SONARE_WITH_FX
      // Fully-wet swept resonant band-passes, same resonator shape as eq.bandPass.
      {"effects.modulation.wah", "*", "*", "resonator group delay"},
      {"effects.modulation.autoWah", "*", "*", "resonator group delay"},
#endif
  };
  return kRows;
}

/// Configurations with no fixed arrival to measure at all. They must still
/// declare zero latency: an insert whose delay is not stationary has no single
/// figure a host or the offline runner could compensate by, so any non-zero
/// declaration would misalign it.
const std::vector<Exemption>& UnmeasurableConfigurations() {
  static const std::vector<Exemption> kRows = {
#ifdef SONARE_WITH_FX
      // Both rotors read fully-wet doppler delay lines whose delay is swept by
      // the rotor LFOs, so the response has no stationary arrival. It only looks
      // stationary from one impulse: both LFOs start at the mean, so a single
      // measurement reads the mean delay (the 1.2 ms default depth, 57.6 samples
      // at 48 kHz) while the running delay sweeps the whole 0…2.4 ms span.
      // Declaring that mean would be wrong in three places at once: the offline
      // runner would trim a fixed 58 samples off every named-processor render,
      // the channel strip would add 58 samples of delay compensation, and this
      // stereo-pair-only insert's untouched rear planes would sit 58 samples
      // ahead of a front pair whose actual delay sweeps 0…115 — a time-varying
      // misalignment that does not exist while the declaration stays zero.
      {"effects.modulation.rotary", "*", "*", "LFO-swept doppler delay"},
#endif
  };
  return kRows;
}

/// Configurations whose response is flat-topped: several samples share the
/// peak, so the declaration has to sit inside that run rather than on its first
/// sample.
const std::vector<Exemption>& FlatPeakConfigurations() {
  static const std::vector<Exemption> kRows = {
      // Below the ceiling, ADAA2 is a three-tap box centred on its one-sample delay.
      {"saturation.hardClipper", "aliasing", "2", "ADAA2 box kernel"},
  };
  return kRows;
}

bool Contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

/// Whether @p text matches @p pattern, where each "*" stands for any run.
bool Glob(const char* pattern, const char* text) {
  if (*pattern == '\0') return *text == '\0';
  if (*pattern == '*') return Glob(pattern + 1, text) || (*text != '\0' && Glob(pattern, text + 1));
  return *pattern == *text && Glob(pattern + 1, text + 1);
}

bool Matches(const Exemption& row, const Configuration& config) {
  return row.name == config.name && Glob(row.key.c_str(), config.key.c_str()) &&
         Glob(row.value.c_str(), config.value.c_str());
}

bool Matches(const std::vector<Exemption>& rows, const Configuration& config) {
  return std::any_of(rows.begin(), rows.end(),
                     [&](const Exemption& row) { return Matches(row, config); });
}

/// A row naming an insert, or a configuration within @p configs, that no longer
/// exists fails rather than silently carrying its exemption forward. With no
/// @p configs only the insert names are checked.
void RequireExemptionsExist(const std::vector<Exemption>& rows,
                            const std::vector<Configuration>& configs = {}) {
  const std::vector<std::string> names = insert_factory_names();
  for (const Exemption& row : rows) {
    INFO(row.name << " " << row.key << "=" << row.value << " (" << row.reason << ")");
    REQUIRE_FALSE(row.reason.empty());
    REQUIRE(Contains(names, row.name));
    if (row.key == "*" || configs.empty()) continue;
    const bool present = std::any_of(configs.begin(), configs.end(),
                                     [&](const Configuration& c) { return Matches(row, c); });
    REQUIRE(present);
  }
}

Configuration DefaultConfiguration(const std::string& name) { return {name, "", "", "{}"}; }

/// @p key of @p name at @p value, with the band it belongs to switched on.
Configuration ProbeConfiguration(const std::string& name, const std::string& key, double value) {
  json::Object object;
  for (const auto& param : insert_probe_params(name, key, value)) {
    object[param.key] = json::Value(param.value);
  }
  return {name, key, std::to_string(std::llround(value)), json::dump(json::Value(object))};
}

/// Every insert's default configuration, then each key with published choices
/// set to each of them in turn, every other key at its default.
const std::vector<Configuration>& SweptConfigurations() {
  static const std::vector<Configuration> kConfigs = [] {
    std::vector<Configuration> configs;
    for (const std::string& name : insert_factory_names()) {
      configs.push_back(DefaultConfiguration(name));
      const json::Value info = json::parse_strict(insert_param_info_json(name));
      for (const json::Value& entry : info.as_array()) {
        if (entry["choices"].is_null()) continue;
        for (const json::Value& choice : entry["choices"].as_array()) {
          configs.push_back(
              ProbeConfiguration(name, entry["name"].as_string(), choice["value"].as_number()));
        }
      }
    }
    return configs;
  }();
  return kConfigs;
}

/// Two correlated impulse channels, silent apart from one sample each.
std::vector<std::vector<float>> MakeImpulse(int num_samples) {
  std::vector<std::vector<float>> channels(static_cast<size_t>(kNumChannels),
                                           std::vector<float>(num_samples, 0.0f));
  for (auto& channel : channels) {
    channel[kImpulseIndex] = kImpulseAmplitude;
  }
  return channels;
}

/// Drives an impulse through a prepared processor and reads back where the
/// response arrives and where it peaks. Both are the earliest qualifying index
/// over all channels, so a processor that delays one side only (a Haas-style
/// insert) reports the undelayed side rather than its widening delay.
Response Measure(ProcessorBase& processor, int num_blocks = kNumBlocks) {
  const int num_samples = kBlockSize * num_blocks;
  auto channels = MakeImpulse(num_samples);
  std::array<float*, kNumChannels> pointers{};
  for (int block = 0; block < num_blocks; ++block) {
    for (int ch = 0; ch < kNumChannels; ++ch) {
      pointers[static_cast<size_t>(ch)] =
          channels[static_cast<size_t>(ch)].data() + block * kBlockSize;
    }
    processor.process(pointers.data(), kNumChannels, kBlockSize);
  }

  Response response;
  for (const auto& channel : channels) {
    for (float sample : channel) {
      response.peak = std::max(response.peak, std::abs(sample));
    }
  }
  const float onset_floor = response.peak * kOnsetFraction;
  int peak_index = num_samples;
  int onset_index = num_samples;
  for (const auto& channel : channels) {
    for (int n = 0; n < num_samples; ++n) {
      const float magnitude = std::abs(channel[static_cast<size_t>(n)]);
      if (magnitude >= response.peak && n < peak_index) peak_index = n;
      if (magnitude >= onset_floor && n < onset_index) onset_index = n;
    }
  }
  const float plateau_floor = response.peak * (1.0f - kPlateauFraction);
  int plateau_end = peak_index;
  const auto level_with_peak = [&](int n) {
    return std::any_of(channels.begin(), channels.end(), [&](const std::vector<float>& channel) {
      return std::abs(channel[static_cast<size_t>(n)]) >= plateau_floor;
    });
  };
  while (plateau_end + 1 < num_samples && level_with_peak(plateau_end + 1)) ++plateau_end;
  response.peak_lag = peak_index - kImpulseIndex;
  response.onset_lag = onset_index - kImpulseIndex;
  response.plateau_end_lag = plateau_end - kImpulseIndex;
  return response;
}

/// Measures @p config against the latency insert_timing() reports for it, under
/// the exemption rules. Failures are soft so one pass reports every drift.
void CheckAlignment(const Configuration& config) {
  CAPTURE(config.name, config.key, config.value, config.json);
  int declared = 0;
  std::unique_ptr<ProcessorBase> processor;
  try {
    declared = insert_timing(config.name, config.json, kSampleRate).latency_samples;
    processor = make_insert(config.name, config.json);
  } catch (const std::exception& e) {
    FAIL_CHECK("the configuration was refused: " << e.what());
    return;
  }
  if (processor == nullptr) {
    FAIL_CHECK("the factory could not build " << config.name);
    return;
  }
  processor->prepare(kSampleRate, kBlockSize);
  CAPTURE(declared);
  CHECK(declared >= 0);
  // The query answers for the instance a strip would build.
  CHECK(processor->latency_samples() == declared);
  if (Matches(UnmeasurableConfigurations(), config)) {
    // Nothing to bracket the declaration against, so the declaration itself is
    // the assertion: a swept, non-stationary delay is compensable by zero only.
    CHECK(declared == 0);
    return;
  }

  // Room for the declared delay and a response as long again after it.
  const int num_blocks = std::max(kNumBlocks, (kImpulseIndex + 2 * declared) / kBlockSize + 2);
  const Response response = Measure(*processor, num_blocks);
  CAPTURE(response.peak, response.onset_lag, response.peak_lag, response.plateau_end_lag);
  if (!(response.peak > kMeasurableFloor)) {
    FAIL_CHECK("no measurable impulse response for " << config.name);
    return;
  }
  // Nothing may leave the processor before the delay it declares (an
  // undeclared lookahead or oversampling round trip shows up here), and the
  // declared delay may not run past the response it is supposed to describe.
  CHECK(response.onset_lag <= declared);
  if (Matches(FlatPeakConfigurations(), config)) {
    CHECK(response.peak_lag <= declared);
    CHECK(declared <= response.plateau_end_lag);
    return;
  }
  CHECK(declared <= response.peak_lag);
  if (!Matches(PeakShiftedConfigurations(), config)) {
    CHECK(response.peak_lag == declared);
  }
}

/// Three-band saturation with one audible band, so the summed output is that
/// band's own contribution. A muted band still runs its stage and its alignment
/// padding, so the timing under test is the one the full configuration has.
MultibandSaturationConfig MakeSoloedBands(SaturationType high_band_type, size_t solo_band) {
  MultibandSaturationConfig config;
  config.bands.back().type = high_band_type;
  for (size_t band = 0; band < config.bands.size(); ++band) {
    config.bands[band].output_gain_db = band == solo_band ? 0.0f : -160.0f;
  }
  return config;
}

}  // namespace

TEST_CASE("Every factory insert declares the latency its impulse response shows",
          "[mastering][insert_factory][latency]") {
  const std::vector<std::string> names = insert_factory_names();
  REQUIRE_FALSE(names.empty());

  std::vector<Configuration> defaults;
  for (const std::string& name : names) defaults.push_back(DefaultConfiguration(name));
  RequireExemptionsExist(PeakShiftedConfigurations());
  RequireExemptionsExist(UnmeasurableConfigurations());
  RequireExemptionsExist(FlatPeakConfigurations());

  for (const Configuration& config : defaults) CheckAlignment(config);
}

TEST_CASE("Every published choice declares the latency its impulse response shows",
          "[mastering][insert_factory][latency][.][slow]") {
  const std::vector<Configuration>& configs = SweptConfigurations();
  REQUIRE(configs.size() > insert_factory_names().size());
  RequireExemptionsExist(PeakShiftedConfigurations(), configs);
  RequireExemptionsExist(UnmeasurableConfigurations(), configs);
  RequireExemptionsExist(FlatPeakConfigurations(), configs);

  for (const Configuration& config : configs) CheckAlignment(config);
}

TEST_CASE("An insert's latency does not depend on the block size it is prepared at",
          "[mastering][insert_factory][latency][.][slow]") {
  for (const Configuration& config : SweptConfigurations()) {
    CAPTURE(config.name, config.key, config.value);
    std::array<int, 3> latencies{};
    const std::array<int, 3> block_sizes = {64, 512, 4096};
    for (size_t index = 0; index < block_sizes.size(); ++index) {
      auto processor = make_insert(config.name, config.json);
      REQUIRE(processor != nullptr);
      processor->prepare(kSampleRate, block_sizes[index]);
      latencies[index] = processor->latency_samples();
    }
    CAPTURE(latencies[0], latencies[1], latencies[2]);
    CHECK(latencies[0] == latencies[1]);
    CHECK(latencies[2] == latencies[1]);
  }
}

TEST_CASE("Selecting 4x oversampling reports its round-trip latency",
          "[mastering][insert_factory][latency]") {
  constexpr double kOversample4x = 3.0;
  for (const char* name :
       {"saturation.softClipper", "saturation.hardClipper", "saturation.waveshaper",
        "saturation.exciter", "spectral.presenceEnhancer"}) {
    const Configuration config = ProbeConfiguration(name, "aliasing", kOversample4x);
    CAPTURE(config.name, config.json);
    CHECK(insert_timing(config.name, config.json, kSampleRate).latency_samples > 0);
    CheckAlignment(config);
  }
}

TEST_CASE("MultibandSaturation keeps a latent band aligned at the summing point",
          "[mastering][multiband][latency]") {
  // Only the tube stage oversamples, so without alignment its band would arrive
  // late into a sum whose flat reconstruction assumes every band shares one time
  // reference. Each band is measured against the same band with a zero-latency
  // stage: the whole processor may shift, but the bands may not shift apart.
  sonare::mastering::saturation::Tube tube;
  tube.prepare(kSampleRate, kBlockSize);
  const int tube_latency = tube.latency_samples();
  REQUIRE(tube_latency > 0);

  MultibandSaturation tubed(MakeSoloedBands(SaturationType::Tube, 0));
  tubed.prepare(kSampleRate, kBlockSize);
  // The default crossover is a zero-latency IIR, so the deepest band stage is
  // the whole declared delay.
  CHECK(tubed.latency_samples() == tube_latency);

  const size_t num_bands = MultibandSaturationConfig{}.bands.size();
  for (size_t band = 0; band < num_bands; ++band) {
    MultibandSaturation reference(MakeSoloedBands(SaturationType::SoftClip, band));
    reference.prepare(kSampleRate, kBlockSize);
    MultibandSaturation aligned(MakeSoloedBands(SaturationType::Tube, band));
    aligned.prepare(kSampleRate, kBlockSize);

    const Response without_latency = Measure(reference);
    const Response with_latency = Measure(aligned);
    const int shift = with_latency.peak_lag - without_latency.peak_lag;
    CAPTURE(band, tube_latency, without_latency.peak_lag, with_latency.peak_lag, shift);
    CHECK(reference.latency_samples() == 0);
    // Every band shifts by the declared amount. The tube band's own shift can
    // land a sample either side because its model and Miller filter reshape the
    // band, whereas an unaligned band would sit a whole round trip away.
    CHECK(std::abs(shift - tube_latency) <= 1);
  }
}
