/// @file insert_latency_alignment_test.cpp
/// @brief Impulse-alignment guard for every processor the insert factory builds.
///
/// latency_samples() is what a host compensates an insert by and what the
/// offline runner trims from the head of a rendered buffer, so it has to be the
/// delay a caller actually observes — including delay contributed by a
/// sub-processor the class merely holds. The check is driven from
/// insert_factory_names() rather than a hand-written list so a new insert is
/// covered the day it is registered.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/tube.h"
#include "rt/processor_base.h"

namespace {

using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::multiband::MultibandSaturation;
using sonare::mastering::multiband::MultibandSaturationConfig;
using sonare::mastering::multiband::SaturationType;
using sonare::rt::ProcessorBase;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kNumChannels = 2;
constexpr int kNumBlocks = 16;
constexpr int kNumSamples = kBlockSize * kNumBlocks;
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

/// Impulse response summary, with both indices expressed as a lag against the
/// input impulse.
struct Response {
  float peak = 0.0f;
  int peak_lag = 0;
  int onset_lag = 0;
};

/// Inserts whose impulse-response peak is not their I/O latency: a resonant or
/// steep filter shape moves the peak by its own group delay, which is phase
/// response rather than a delay a host should compensate. The arrival/peak
/// bracket below still applies to them.
const std::vector<std::string>& PeakShiftedInserts() {
  static const std::vector<std::string> kNames = {
      // A fully-wet 1 kHz band-pass: the resonator peaks a quarter period in.
      "eq.bandPass",
      // The 4x12 cab roll-off adds its own group delay after the tube stage.
      "saturation.ampSim",
      // Both drive a magnetization state rather than passing the input through,
      // so an impulse produces a decaying step and not a matching impulse: the
      // response arrives at the right sample, then holds a long plateau whose
      // largest excursion is placed by the relaxation envelope rather than by
      // any delay. The arrival/peak bracket below is the part that still says
      // something about latency for these two.
      "saturation.tape",
      "saturation.transformer",
      // The crossover spreads the impulse across each band's filter response
      // rather than one sample, and the top band's envelope follower starts
      // cold, so it under-reads the arrival sample and expands it down harder
      // than the smaller sample that follows.
      "multiband.expander",
#ifdef SONARE_WITH_FX
      // Fully-wet swept resonant band-passes, same resonator shape as eq.bandPass.
      "effects.modulation.wah",
      "effects.modulation.autoWah",
#endif
  };
  return kNames;
}

/// Inserts with no fixed arrival to measure at all.
const std::vector<std::string>& UnmeasurableInserts() {
  static const std::vector<std::string> kNames = {
#ifdef SONARE_WITH_FX
      // Both rotors read fully-wet doppler delay lines whose delay is swept by
      // the rotor LFOs, so the response has no stationary arrival.
      "effects.modulation.rotary",
#endif
  };
  return kNames;
}

bool Contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

/// Two correlated impulse channels, silent apart from one sample each.
std::vector<std::vector<float>> MakeImpulse() {
  std::vector<std::vector<float>> channels(static_cast<size_t>(kNumChannels),
                                           std::vector<float>(kNumSamples, 0.0f));
  for (auto& channel : channels) {
    channel[kImpulseIndex] = kImpulseAmplitude;
  }
  return channels;
}

/// Drives an impulse through a prepared processor and reads back where the
/// response arrives and where it peaks. Both are the earliest qualifying index
/// over all channels, so a processor that delays one side only (a Haas-style
/// insert) reports the undelayed side rather than its widening delay.
Response Measure(ProcessorBase& processor) {
  auto channels = MakeImpulse();
  std::array<float*, kNumChannels> pointers{};
  for (int block = 0; block < kNumBlocks; ++block) {
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
  int peak_index = kNumSamples;
  int onset_index = kNumSamples;
  for (const auto& channel : channels) {
    for (int n = 0; n < kNumSamples; ++n) {
      const float magnitude = std::abs(channel[static_cast<size_t>(n)]);
      if (magnitude >= response.peak && n < peak_index) peak_index = n;
      if (magnitude >= onset_floor && n < onset_index) onset_index = n;
    }
  }
  response.peak_lag = peak_index - kImpulseIndex;
  response.onset_lag = onset_index - kImpulseIndex;
  return response;
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

  // Both exemption lists are strict subsets of the factory: a renamed or dropped
  // insert fails here instead of silently carrying its exemption forward.
  REQUIRE(PeakShiftedInserts().size() + UnmeasurableInserts().size() < names.size());
  for (const auto& exempt : PeakShiftedInserts()) {
    INFO(exempt);
    REQUIRE(Contains(names, exempt));
  }
  for (const auto& exempt : UnmeasurableInserts()) {
    INFO(exempt);
    REQUIRE(Contains(names, exempt));
  }

  // Every name is measured in one pass, so a run reports every drifted
  // processor rather than stopping at the first.
  for (const std::string& name : names) {
    auto processor = make_insert(name, "{}");
    CAPTURE(name);
    if (processor == nullptr) {
      FAIL_CHECK("the factory could not build " << name);
      continue;
    }
    processor->prepare(kSampleRate, kBlockSize);
    const int declared = processor->latency_samples();
    CAPTURE(declared);
    CHECK(declared >= 0);
    if (Contains(UnmeasurableInserts(), name)) {
      continue;
    }

    const Response response = Measure(*processor);
    CAPTURE(response.peak, response.onset_lag, response.peak_lag);
    if (!(response.peak > kMeasurableFloor)) {
      FAIL_CHECK("no measurable impulse response for " << name);
      continue;
    }
    // Nothing may leave the processor before the delay it declares (an
    // undeclared lookahead or oversampling round trip shows up here), and the
    // declared delay may not run past the response it is supposed to describe.
    CHECK(response.onset_lag <= declared);
    CHECK(declared <= response.peak_lag);
    if (!Contains(PeakShiftedInserts(), name)) {
      CHECK(response.peak_lag == declared);
    }
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
