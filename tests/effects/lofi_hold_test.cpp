/// @file lofi_hold_test.cpp
/// @brief The measured lofi degrader: a sample-and-hold kept as a rate in hertz,
///        the three behaviours its printed entries collapse to, and a quantizer
///        whose step is fixed in the signal's own units.
///
/// Expectations are structural -- where a null falls, whether a profile returns
/// after one, how far three behaviours stand apart, whether a step moves when
/// the signal does. None is a recorded waveform and none could be: the contract
/// this module keeps is the control protocol and not the audio
/// (src/midi/synth/docs/gs.md).
///
/// Two readings, two instruments. Where the null falls to one twelfth-octave is
/// read the way the archive read it, as a band profile of broadband noise
/// against the same chain with the hold off. Where it falls in hertz is read by
/// demodulating one probe tone at a time, which has no band width of its own --
/// the coarse reading locates the band first, so the fine one is a refinement
/// and not a search told where to look.
///
/// Reach is an output: the case reports how many comparisons it made, because a
/// run that compared nothing looks exactly like a run that passed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/fft.h"
#include "mastering/saturation/bitcrusher.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::mastering::saturation::BitCrusher;
using sonare::mastering::saturation::BitCrusherConfig;
using sonare::mastering::saturation::QuantizerMode;

/// Counts every comparison it makes, so the case can report its own reach.
class Tally {
 public:
  void near(double got, double reading, double tolerance, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", expected " << reading << ", tolerance " << tolerance);
    CHECK(std::fabs(got - reading) <= tolerance);
  }

  void at_least(double got, double bound, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", needs at least " << bound);
    CHECK(got >= bound);
  }

  void at_most(double got, double bound, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", needs at most " << bound);
    CHECK(got <= bound);
  }

  void same(bool held, const std::string& what) {
    ++count_;
    INFO(what);
    CHECK(held);
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
};

// --- rates ------------------------------------------------------------------

// The rate the readings were taken at, and the two host rates the null has to
// land on the same frequency at.
constexpr double kMachineRate = 32000.0;
constexpr double kSecondHostRate = 44100.0;
constexpr double kHostRate = 48000.0;

// The two holds the archive separates, expressed the way this processor stores
// them: a rate, not a count of samples. The published nulls are 10679 Hz and
// 8000 Hz, which these reproduce to 0.11 per cent and exactly.
constexpr float kShortHoldHz = 32000.0f / 3.0f;
constexpr float kLongHoldHz = 32000.0f / 4.0f;
// The hold the archive could not tell from no hold at all, because its only
// null lands on the last band the reading had.
constexpr float kUnresolvedHoldHz = 32000.0f / 2.0f;

// --- what the records publish, and how --------------------------------------

// The two null frequencies, each read only to the twelfth-octave band it falls
// in: the record says so outright, and both figures are centres of that grid.
constexpr double kPublishedShortNullHz = 10679.0;
constexpr double kPublishedLongNullHz = 8000.0;
// One twelfth-octave band, which is the whole of what places either null.
constexpr double kBandWidth = 0.05946309435929531;

// Success criterion for this insert: the first null in hertz, to one per cent,
// at both host rates.
constexpr double kNullRateTolerance = 0.01;

// The nearest two entries from different groups differ by this much; inside a
// group they agree to 0.34 dB at a floor of 0.25 dB. A pair of behaviours that
// came closer than the first figure would collapse the three into fewer.
constexpr double kCrossGroupDb = 0.66;

// A null has to be this far down to be read as one here. Not a published depth:
// the archive's 19 dB and 26 dB were read through the machine's own chain with
// the type's filters parked, and this instrument reads the aperture alone, so
// the two are not the same quantity. Set well over the 0.66 dB that separates
// the nearest pair of behaviours.
constexpr double kNullDepthDb = 10.0;

// How far the profile has to come back up after a null, and fall again at twice
// it. This is what rules out a table of low-pass filters: a corner that went
// that deep does not return. Same footing as the figure above -- a detection
// threshold several times the separation of two behaviours, not a reading.
constexpr double kReturnDb = 4.0;

// Where the quantizer's added series sits against the converter, across the
// whole 36.1 dB of stimulus the record swept. Published as a range over that
// sweep rather than as one value, and derived: the raw record carries levels
// only under each take's own first order, and states that nothing in it is a
// level. The record does not read how many bits the step leaves -- it says so
// in as many words -- so the depth below is a choice this tree makes and the
// tolerance is one whole bit.
constexpr double kQuantizerStepLowDb = -87.0;
constexpr double kQuantizerStepHighDb = -84.0;
constexpr double kOneBitDb = 6.0206;
constexpr double kStimulusSpanDb = 36.1;
constexpr int kQuantizerOnBits = 14;

// The repeats of one setting failed to agree by this much at the closest of the
// three levels, which is why the field's first two printed entries are one
// value. Two states that stand further apart than this are two states.
constexpr double kRepeatFloorDb = 13.1;

// --- instruments ------------------------------------------------------------

// 0.5 s of noise, and a transform that leaves each twelfth-octave band several
// bins to average over.
constexpr int kProfileFft = 4096;
constexpr double kProfileSeconds = 0.5;
// Below this the bands hold too few bins of this transform to read.
constexpr double kProfileLowHz = 1000.0;

// The refinement runs inside the band the profile already placed the null in,
// so its span only has to cover one band and its step only has to beat the one
// per cent the criterion asks for.
constexpr double kProbeSeconds = 0.15;
constexpr double kRefineSpan = 0.035;
constexpr double kRefineStep = 0.0025;

/// Deterministic uniform noise. A fixed generator rather than a random one, so
/// two rates are compared on the same stimulus and a failure repeats.
std::vector<float> noise(std::size_t count, std::uint32_t seed) {
  std::vector<float> out(count);
  std::uint32_t state = seed;
  for (float& sample : out) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<float>((state >> 8) & 0x00FFFFFFu) / 16777216.0f - 0.5f;
  }
  return out;
}

/// A config with the hold at @p hold_hz and nothing else in the path.
BitCrusherConfig only_hold(float hold_hz) {
  BitCrusherConfig config;
  config.quantizer_mode = QuantizerMode::kOff;
  config.hold_hz = hold_hz;
  return config;
}

std::vector<float> run(const BitCrusherConfig& config, double sample_rate,
                       const std::vector<float>& input) {
  BitCrusher processor(config);
  processor.prepare(sample_rate, static_cast<int>(input.size()));
  std::vector<float> buffer = input;
  float* channels[] = {buffer.data()};
  processor.process(channels, 1, static_cast<int>(buffer.size()));
  return buffer;
}

/// The twelfth-octave band centres the published profiles were read on: 113 of
/// them ending at 16 kHz, which is the measured unit's own Nyquist.
std::vector<double> band_centres() {
  std::vector<double> bands;
  bands.reserve(113);
  for (int k = 112; k >= 0; --k) {
    bands.push_back(16000.0 * std::pow(2.0, -k / 12.0));
  }
  return bands;
}

/// Bands of that grid this rate can actually carry a reading in.
std::vector<double> readable_bands(double sample_rate) {
  std::vector<double> bands;
  for (const double centre : band_centres()) {
    if (centre >= kProfileLowHz && centre < sample_rate * 0.49) bands.push_back(centre);
  }
  return bands;
}

/// Welch power spectrum of @p signal, half-overlapped and Hann-windowed.
std::vector<double> power_spectrum(const std::vector<float>& signal) {
  sonare::FFT plan(kProfileFft);
  const std::size_t bins = static_cast<std::size_t>(plan.n_bins());
  std::vector<std::complex<float>> spectrum(bins);
  std::vector<double> power(bins, 0.0);
  std::vector<float> window(kProfileFft);
  for (int i = 0; i < kProfileFft; ++i) {
    window[static_cast<std::size_t>(i)] =
        static_cast<float>(0.5 - 0.5 * std::cos(kTwoPiD * i / kProfileFft));
  }
  std::vector<float> segment(kProfileFft);
  int frames = 0;
  const std::size_t hop = kProfileFft / 2;
  for (std::size_t start = 0; start + kProfileFft <= signal.size(); start += hop) {
    for (int i = 0; i < kProfileFft; ++i) {
      const std::size_t j = static_cast<std::size_t>(i);
      segment[j] = signal[start + j] * window[j];
    }
    plan.forward(segment.data(), spectrum.data());
    for (std::size_t b = 0; b < bins; ++b) {
      power[b] += static_cast<double>(std::norm(spectrum[b]));
    }
    ++frames;
  }
  REQUIRE(frames > 0);
  for (double& value : power) value /= frames;
  return power;
}

/// Mean power of @p power over one twelfth-octave band, in decibels.
double band_db(const std::vector<double>& power, double sample_rate, double centre_hz) {
  const double edge = std::pow(2.0, 1.0 / 24.0);
  const double lo = centre_hz / edge;
  const double hi = centre_hz * edge;
  double sum = 0.0;
  int used = 0;
  for (std::size_t b = 0; b < power.size(); ++b) {
    const double hz = static_cast<double>(b) * sample_rate / kProfileFft;
    if (hz < lo || hz >= hi) continue;
    sum += power[b];
    ++used;
  }
  REQUIRE(used > 0);
  // The 1e-30 guard is neither kEpsilon nor kSpectrumEpsilon: it floors a raw
  // power before a log, where either named epsilon would sit above a real null.
  return 10.0 * std::log10(sum / used + 1e-30);
}

/// Band profiles at one rate: every hold read against the same stimulus and the
/// same chain with the hold off, which is how the archive read these entries.
class Profiler {
 public:
  explicit Profiler(double sample_rate)
      : rate_(sample_rate),
        input_(noise(static_cast<std::size_t>(sample_rate * kProfileSeconds), 0x10F1u)),
        flat_(power_spectrum(input_)),
        bands_(readable_bands(sample_rate)) {}

  std::vector<double> of(float hold_hz) const {
    const std::vector<double> held = power_spectrum(run(only_hold(hold_hz), rate_, input_));
    std::vector<double> profile;
    profile.reserve(bands_.size());
    for (const double centre : bands_) {
      profile.push_back(band_db(held, rate_, centre) - band_db(flat_, rate_, centre));
    }
    return profile;
  }

  const std::vector<double>& bands() const { return bands_; }
  double rate() const { return rate_; }

 private:
  double rate_;
  std::vector<float> input_;
  std::vector<double> flat_;
  std::vector<double> bands_;
};

/// Index of the deepest band of a profile.
std::size_t deepest(const std::vector<double>& profile) {
  std::size_t best = 0;
  for (std::size_t i = 1; i < profile.size(); ++i) {
    if (profile[i] < profile[best]) best = i;
  }
  return best;
}

/// Level of a profile at the band nearest @p hz.
double profile_at(const std::vector<double>& profile, const std::vector<double>& bands, double hz) {
  std::size_t best = 0;
  for (std::size_t i = 1; i < bands.size(); ++i) {
    if (std::fabs(std::log(bands[i] / hz)) < std::fabs(std::log(bands[best] / hz))) best = i;
  }
  return profile[best];
}

/// Magnitude the hold returns at one frequency, read by demodulating the probe
/// tone itself: the aperture at that frequency, with the images it folds
/// elsewhere left out.
double response_db(float hold_hz, double sample_rate, double hz) {
  const std::size_t count = static_cast<std::size_t>(sample_rate * kProbeSeconds);
  const double step = kTwoPiD * hz / sample_rate;
  std::vector<float> probe(count);
  std::vector<double> turn_re(count);
  std::vector<double> turn_im(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double angle = step * static_cast<double>(i);
    turn_re[i] = std::cos(angle);
    turn_im[i] = std::sin(angle);
    probe[i] = static_cast<float>(turn_im[i]);
  }
  const std::vector<float> held = run(only_hold(hold_hz), sample_rate, probe);
  std::complex<double> in(0.0, 0.0);
  std::complex<double> out(0.0, 0.0);
  for (std::size_t i = 0; i < count; ++i) {
    const std::complex<double> turn(turn_re[i], -turn_im[i]);
    in += static_cast<double>(probe[i]) * turn;
    out += static_cast<double>(held[i]) * turn;
  }
  return 20.0 * std::log10(std::abs(out) / std::abs(in) + 1e-30);
}

/// The null in hertz, refined inside the band the profile already placed it in.
double refined_null_hz(float hold_hz, double sample_rate, double band_centre_hz) {
  double best_hz = band_centre_hz;
  double best_db = 0.0;
  bool first = true;
  for (double offset = -kRefineSpan; offset <= kRefineSpan + 1e-12; offset += kRefineStep) {
    const double hz = band_centre_hz * (1.0 + offset);
    const double db = response_db(hold_hz, sample_rate, hz);
    if (first || db < best_db) {
      best_db = db;
      best_hz = hz;
      first = false;
    }
  }
  return best_hz;
}

double median_distance(const std::vector<double>& lhs, const std::vector<double>& rhs) {
  std::vector<double> apart;
  apart.reserve(lhs.size());
  for (std::size_t i = 0; i < lhs.size(); ++i) apart.push_back(std::fabs(lhs[i] - rhs[i]));
  std::sort(apart.begin(), apart.end());
  return apart[apart.size() / 2];
}

/// A config with the quantizer in the path and nothing else.
BitCrusherConfig only_quantizer(int bit_depth) {
  BitCrusherConfig config;
  config.bit_depth = bit_depth;
  return config;
}

/// The quantizer's added series against full scale, driven by a tone @p level_db
/// below it.
double added_series_db(const BitCrusherConfig& config, double sample_rate, double level_db) {
  constexpr double kPeak = 0.9;
  constexpr double kToneHz = 997.0;
  const std::size_t count = static_cast<std::size_t>(sample_rate * kProbeSeconds);
  const double amplitude = kPeak * std::pow(10.0, level_db / 20.0);
  std::vector<float> tone(count);
  for (std::size_t i = 0; i < count; ++i) {
    tone[i] = static_cast<float>(
        amplitude * std::sin(kTwoPiD * kToneHz * static_cast<double>(i) / sample_rate));
  }
  const std::vector<float> quantized = run(config, sample_rate, tone);
  double sum = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double error = static_cast<double>(quantized[i]) - static_cast<double>(tone[i]);
    sum += error * error;
  }
  return 20.0 * std::log10(std::sqrt(sum / static_cast<double>(count)) + 1e-30);
}

/// The configuration with nothing in the path at all: no hold, no quantizer.
BitCrusherConfig bare_config() {
  BitCrusherConfig config;
  config.quantizer_mode = QuantizerMode::kOff;
  config.hold_hz = 0.0f;
  return config;
}

}  // namespace

TEST_CASE("the lofi hold's nulls, its three behaviours and the quantizer's fixed step",
          "[lofi-hold]") {
  Tally tally;

  // One stimulus and one reference per rate, read once and shared by every hold
  // compared at that rate.
  const Profiler at_machine(kMachineRate);
  const Profiler at_44100(kSecondHostRate);
  const Profiler at_48000(kHostRate);
  const Profiler* const profilers[] = {&at_machine, &at_44100, &at_48000};

  // --- where the nulls fall, to one band, and then in hertz -----------------
  // The coarse reading is the archive's own: a band profile against the same
  // chain with the hold off. The fine one refines inside the band it named.
  struct Published {
    float hold_hz;
    double null_hz;
    const char* what;
  };
  const Published published[] = {
      {kShortHoldHz, kPublishedShortNullHz, "the shorter hold's null"},
      {kLongHoldHz, kPublishedLongNullHz, "the longer hold's null"},
  };

  std::vector<std::vector<double>> long_hold_profiles;
  for (const Published& entry : published) {
    std::vector<double> refined_hz;
    for (const Profiler* profiler : profilers) {
      const std::vector<double> profile = profiler->of(entry.hold_hz);
      const std::vector<double>& bands = profiler->bands();
      const std::size_t at = deepest(profile);
      const std::string where = std::string(entry.what) + " at " +
                                std::to_string(static_cast<int>(profiler->rate())) + " Hz";
      tally.at_most(std::fabs(bands[at] - entry.null_hz) / entry.null_hz, kBandWidth,
                    where + " falls in the band the record names");
      tally.at_least(-profile[at], kNullDepthDb, where + " is a null and not a slope");

      // Against the stored rate, per rate. This is the check that catches a
      // hold kept as a count of samples; the cross-rate one below does not,
      // because two rates can round to counts whose nulls agree with each other
      // and with neither the rate asked for. Neither check subsumes the other.
      const double refined = refined_null_hz(entry.hold_hz, profiler->rate(), bands[at]);
      tally.at_most(std::fabs(refined - entry.hold_hz) / entry.hold_hz, kNullRateTolerance,
                    where + " in hertz, against the rate the hold is stored as");
      refined_hz.push_back(refined);
      if (entry.hold_hz == kLongHoldHz) long_hold_profiles.push_back(profile);
    }
    // And the two host rates have to agree with each other, which is what a
    // stored rate buys: stability across a rate change, as distinct from being
    // the right frequency at either of them.
    tally.at_most(std::fabs(refined_hz[1] - refined_hz[2]) / refined_hz[2], kNullRateTolerance,
                  std::string(entry.what) + " lands on one frequency at 44100 and 48000");
  }

  // --- the longer hold returns after its null, and falls again --------------
  // This is what separates a hold from a table of low-pass filters: a corner
  // that went that deep does not come back up.
  for (std::size_t i = 1; i < long_hold_profiles.size(); ++i) {
    const std::vector<double>& profile = long_hold_profiles[i];
    const std::vector<double>& bands = profilers[i]->bands();
    const double at_null = profile_at(profile, bands, kLongHoldHz);
    const double between = profile_at(profile, bands, kLongHoldHz * 1.5);
    const double at_second = profile_at(profile, bands, kLongHoldHz * 2.0);
    const std::string where =
        " at " + std::to_string(static_cast<int>(profilers[i]->rate())) + " Hz";
    tally.at_least(between - at_null, kReturnDb,
                   "the longer hold's profile comes back up after its null" + where);
    tally.at_least(between - at_second, kReturnDb,
                   "and falls again where a hold of four has its second zero" + where);
  }

  // --- three behaviours, and why the flat one is two ------------------------
  const std::vector<double> flat = at_machine.of(0.0f);
  const std::vector<double> shorter = at_machine.of(kShortHoldHz);
  const std::vector<double> longer = at_machine.of(kLongHoldHz);
  tally.at_least(median_distance(flat, shorter), kCrossGroupDb,
                 "no hold and the shorter hold are two behaviours");
  tally.at_least(median_distance(flat, longer), kCrossGroupDb,
                 "no hold and the longer hold are two behaviours");
  tally.at_least(median_distance(shorter, longer), kCrossGroupDb,
                 "the two holds are two behaviours");

  // The archive cannot say which hold its flat group is, because a hold of two
  // puts its only null on the last band the reading had. Same here: at the rate
  // it was read at, the profile is still falling where the band set ends.
  {
    const std::vector<double> profile = at_machine.of(kUnresolvedHoldHz);
    const double top = at_machine.bands().back() * std::pow(2.0, 1.0 / 12.0);
    tally.same(deepest(profile) == profile.size() - 1,
               "a hold of two is still falling at the top band, so it cannot be told from none");
    tally.at_most(std::fabs(top - kMachineRate * 0.5) / (kMachineRate * 0.5), 1e-9,
                  "and the band above the last one readable is the rate's own Nyquist");
  }

  // --- two ways to hold, and only one of them counts ------------------------
  // A file and a preset can reach this state together, so what happens with
  // both set is pinned rather than left to emerge.
  {
    const std::vector<float> stimulus =
        noise(static_cast<std::size_t>(kHostRate * kProfileSeconds), 0x2C0Eu);
    const BitCrusherConfig rate_only = only_hold(kLongHoldHz);
    BitCrusherConfig both = rate_only;
    both.downsample_factor = 8;
    BitCrusherConfig count_only = only_hold(0.0f);
    count_only.downsample_factor = 8;
    const std::vector<float> by_rate = run(rate_only, kHostRate, stimulus);
    tally.same(run(both, kHostRate, stimulus) == by_rate,
               "a sample count set beside a hold rate changes nothing: the rate is counting");
    // Without which the first comparison would pass on a factor that does
    // nothing to begin with.
    tally.same(run(count_only, kHostRate, stimulus) != by_rate,
               "and that same sample count is what holds when no rate is set");
  }

  // --- the quantizer's step -------------------------------------------------
  // Fixed in the signal's own units: what the record separates a quantizer from
  // a noise source by is that this figure does not move as the stimulus falls.
  const BitCrusherConfig on = only_quantizer(kQuantizerOnBits);
  std::vector<double> steps_db;
  for (const double level_db : {0.0, -kStimulusSpanDb * 0.5, -kStimulusSpanDb}) {
    const double step_db = added_series_db(on, kHostRate, level_db);
    steps_db.push_back(step_db);
    // Within one bit of the published band. The record does not read the bit
    // count, so one bit is the resolution of the thing being chosen.
    const double outside = std::max(step_db - kQuantizerStepHighDb, kQuantizerStepLowDb - step_db);
    tally.at_most(outside, kOneBitDb,
                  "the step with the stimulus " + std::to_string(static_cast<int>(-level_db)) +
                      " dB down is within one bit of the published band");
  }
  const double loudest = *std::max_element(steps_db.begin(), steps_db.end());
  const double quietest = *std::min_element(steps_db.begin(), steps_db.end());
  tally.at_most(loudest - quietest, kQuantizerStepHighDb - kQuantizerStepLowDb,
                "and does not move across the 36.1 dB the record swept");

  // Two values of the field, and they are two: the record needed 13.1 dB to
  // call two entries one value, so anything standing further apart is two.
  const double off_step = added_series_db(bare_config(), kHostRate, 0.0);
  tally.at_least(added_series_db(on, kHostRate, 0.0) - off_step, kRepeatFloorDb,
                 "the quantizer on and off are two states, not one");

  // --- nothing in the path is nothing in the path ---------------------------
  // A bypass built as a transparent setting rather than as a branch survives
  // both halves of the bit-identity net, so it is checked here instead: the
  // bare configuration has to return its input sample for sample.
  const std::vector<float> input =
      noise(static_cast<std::size_t>(kHostRate * kProfileSeconds), 0xB47Eu);
  {
    BitCrusher processor(bare_config());
    processor.prepare(kHostRate, static_cast<int>(input.size()));
    std::vector<float> buffer = input;
    float* channels[] = {buffer.data()};
    processor.process(channels, 1, static_cast<int>(buffer.size()));
    tally.same(buffer == input, "the bare configuration returns its input, bit for bit");

    // And still does after the hold has been away and come back. A bypass that
    // only holds at construction stops being one the first time a file
    // automates the byte to zero.
    REQUIRE(processor.set_parameter(2, kLongHoldHz));
    std::vector<float> held = input;
    float* held_channels[] = {held.data()};
    processor.process(held_channels, 1, static_cast<int>(held.size()));
    tally.same(held != input, "the hold reaches the audio when it is set live");

    REQUIRE(processor.set_parameter(2, 0.0f));
    std::vector<float> returned = input;
    float* returned_channels[] = {returned.data()};
    processor.process(returned_channels, 1, static_cast<int>(returned.size()));
    tally.same(returned == input, "and the bare configuration is bare again once it is set back");
  }

  // The hold rate is published as automatable and the mode is not: one is a
  // value and the other selects what the processor is made of.
  {
    const BitCrusher processor{};
    const auto descriptors = processor.parameter_descriptors();
    REQUIRE_FALSE(descriptors.empty());
    bool publishes_hold = false;
    for (const auto& descriptor : descriptors) {
      if (descriptor.key == "holdHz") {
        publishes_hold = true;
        tally.same(processor.parameter_is_realtime_safe(descriptor.id),
                   "the hold rate is safe to move from the audio thread");
      }
    }
    tally.same(publishes_hold, "the hold rate is published by its own key");
  }

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 30);
}
