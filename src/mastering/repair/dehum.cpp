#include "mastering/repair/dehum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

using sonare::constants::kFloorDb;
using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;

namespace {

struct Notch {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  void set_coefficients(float frequency_hz, float sample_rate, float q) {
    const float omega = kTwoPi * frequency_hz / sample_rate;
    const auto coeffs = rt::rbj_notch(omega, q);
    b0 = coeffs.b0;
    b1 = coeffs.b1;
    b2 = coeffs.b2;
    a1 = coeffs.a1;
    a2 = coeffs.a2;
  }

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Notch make_notch(float frequency_hz, float sample_rate, float q) {
  Notch notch;
  notch.set_coefficients(frequency_hz, sample_rate, q);
  return notch;
}

struct PllTracker {
  float frequency_hz = 50.0f;
  float phase = 0.0f;

  float process(float sample, float target_hz, int sample_rate, const DehumConfig& config) {
    const float detector = sample * std::cos(phase);
    const float pull = config.adaptation * 0.001f * (target_hz - frequency_hz);
    frequency_hz += config.pll_bandwidth * detector + pull;
    frequency_hz =
        std::clamp(frequency_hz, std::max(1.0f, config.fundamental_hz - config.search_range_hz),
                   std::min(static_cast<float>(sample_rate) * 0.49f,
                            config.fundamental_hz + config.search_range_hz));
    phase += kTwoPi * frequency_hz / static_cast<float>(sample_rate);
    if (phase > kTwoPi) {
      phase -= kTwoPi;
    }
    return frequency_hz;
  }
};

double projected_energy(const std::vector<float>& samples, size_t begin, size_t end,
                        float frequency_hz, int sample_rate) {
  double sin_sum = 0.0;
  double cos_sum = 0.0;
  for (size_t i = begin; i < end; ++i) {
    const double phase =
        kTwoPiD * frequency_hz * static_cast<double>(i) / static_cast<double>(sample_rate);
    sin_sum += samples[i] * std::sin(phase);
    cos_sum += samples[i] * std::cos(phase);
  }
  return sin_sum * sin_sum + cos_sum * cos_sum;
}

constexpr int kFundamentalSearchSteps = 16;

/// One pass of the fundamental search: the winning candidate and the energies of
/// all of them. The tracker uses the winner; the detector reads the rest to say
/// how far it stood above the field.
struct FundamentalSearch {
  float best_hz = 0.0f;
  double best_energy = -1.0;
  float window_low = 0.0f;
  float window_high = 0.0f;
  int candidates = 0;  ///< Zero when the window collapsed and no search ran.
  std::array<double, kFundamentalSearchSteps + 1> energy = {};
};

FundamentalSearch search_fundamental(const std::vector<float>& samples, size_t begin, size_t end,
                                     int sample_rate, const DehumConfig& config,
                                     float previous_hz) {
  // Anchor the search on the configured fundamental, the same window the PLL
  // clamps its applied frequency to. Centring on the previous estimate alone
  // lets the target walk one search range per frame, so material without a
  // strong tone in band drags the notch to the edge of the window and holds it
  // there, cutting programme content instead of hum.
  FundamentalSearch search;
  search.window_low = std::max(1.0f, config.fundamental_hz - config.search_range_hz);
  search.window_high = std::min(static_cast<float>(sample_rate) * 0.49f,
                                config.fundamental_hz + config.search_range_hz);
  if (!(search.window_high > search.window_low)) {
    search.best_hz = std::clamp(previous_hz, search.window_low, search.window_high);
    return search;
  }
  const float anchored_previous = std::clamp(previous_hz, search.window_low, search.window_high);
  search.best_hz = anchored_previous;
  double best_energy = -1.0;
  for (int step = 0; step <= kFundamentalSearchSteps; ++step) {
    const float hz = search.window_low + (search.window_high - search.window_low) *
                                             static_cast<float>(step) /
                                             static_cast<float>(kFundamentalSearchSteps);
    const double energy = projected_energy(samples, begin, end, hz, sample_rate);
    search.energy[static_cast<size_t>(step)] = energy;
    if (energy > best_energy) {
      best_energy = energy;
      search.best_hz = hz;
    }
  }
  search.best_energy = best_energy;
  search.candidates = kFundamentalSearchSteps + 1;
  return search;
}

float estimate_fundamental(const std::vector<float>& samples, size_t begin, size_t end,
                           int sample_rate, const DehumConfig& config, float previous_hz) {
  const FundamentalSearch search =
      search_fundamental(samples, begin, end, sample_rate, config, previous_hz);
  if (search.candidates == 0) return search.best_hz;
  // Smooth from the anchored estimate rather than the raw previous value, so a
  // frame cannot hand the next one a target the PLL is unable to follow.
  const float anchored_previous = std::clamp(previous_hz, search.window_low, search.window_high);
  return std::clamp(anchored_previous + config.adaptation * (search.best_hz - anchored_previous),
                    search.window_low, search.window_high);
}

// ------------------------------------------------------------------ detection

/// Detection frames run a second rather than config.frame_size: the search
/// window spans a few hertz, which a 2048-sample frame cannot resolve at all.
constexpr double kHumDetectionFrameSeconds = 1.0;

/// A harmonic counts when it stands this far above the interharmonic level to
/// either side, and above the library dB floor. The margin alone is not enough:
/// at an empty harmonic slot both the level and its floor are numerical residue
/// near -180 dBFS, and the ratio of two such values clears any margin at random.
constexpr float kHumHarmonicMarginDb = 6.0f;

template <typename T>
T median_of(std::vector<T> values) {
  if (values.empty()) return T{};
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

/// Amplitude at @p hz in dBFS, from the same sin/cos projection the search uses.
float projection_dbfs(const std::vector<float>& samples, size_t begin, size_t end, float hz,
                      int sample_rate) {
  if (end <= begin || !(hz > 0.0f)) return kFloorDb;
  const double count = static_cast<double>(end - begin);
  const double amplitude =
      2.0 * std::sqrt(projected_energy(samples, begin, end, hz, sample_rate)) / count;
  return linear_to_db(static_cast<float>(amplitude));
}

float prominence_of(const FundamentalSearch& search) {
  if (search.candidates == 0) return 1.0f;
  std::vector<double> energies(search.energy.begin(), search.energy.begin() + search.candidates);
  // An exactly nulled candidate would divide by zero; cap the ratio instead.
  const double reference = std::max(median_of(energies), search.best_energy * 1.0e-12);
  if (!(reference > 0.0)) return 1.0f;
  return static_cast<float>(search.best_energy / reference);
}

size_t detection_frame(size_t size, int sample_rate) {
  const double seconds = kHumDetectionFrameSeconds * static_cast<double>(sample_rate);
  return std::min(size, std::max<size_t>(1, static_cast<size_t>(seconds)));
}

// ----------------------------------------------------------------- processing

/// What one channel's cascade did, for the report.
struct PassTrace {
  int notched_harmonics = 0;
  float applied_fundamental_hz = 0.0f;
  float fundamental_drift_hz = 0.0f;
};

PassTrace run_fixed(std::vector<float>& samples, int sample_rate, const DehumConfig& config) {
  PassTrace trace;
  trace.applied_fundamental_hz = config.fundamental_hz;
  for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
    const float frequency = config.fundamental_hz * static_cast<float>(harmonic);
    if (frequency >= static_cast<float>(sample_rate) * 0.5f) break;
    auto notch = make_notch(frequency, static_cast<float>(sample_rate), config.q);
    for (auto& sample : samples) sample = notch.process(sample);
    ++trace.notched_harmonics;
  }
  return trace;
}

/// Runs the tracking cascade over every channel from one shared frequency.
/// @p tracking drives the search and the PLL; for a single channel it is that
/// channel's own unfiltered samples, which is what makes the mono result
/// identical whichever entrypoint produced it.
PassTrace run_adaptive(const std::vector<std::vector<float>*>& channels,
                       const std::vector<float>& tracking, int sample_rate,
                       const DehumConfig& config) {
  const size_t channel_count = channels.size();
  const size_t harmonic_count = static_cast<size_t>(config.harmonics);
  float fundamental = config.fundamental_hz;
  float target_fundamental = fundamental;
  PllTracker tracker{fundamental, 0.0f};
  std::vector<std::vector<Notch>> cascades(channel_count, std::vector<Notch>(harmonic_count));
  for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
    const Notch seed = make_notch(fundamental * static_cast<float>(harmonic),
                                  static_cast<float>(sample_rate), config.q);
    for (auto& cascade : cascades) cascade[static_cast<size_t>(harmonic - 1)] = seed;
  }

  // The PLL fundamental drifts slowly (pll_bandwidth is small), so recomputing
  // the RBJ notch coefficients (sin/cos/division per harmonic) on every sample
  // is wasteful. Refresh them only when the tracked fundamental has moved by
  // more than a small relative amount since the last refresh; the filter state
  // carries across untouched, so the adaptive tracking behavior is preserved.
  constexpr float kCoeffRefreshRatio = 1e-3f;
  float last_coeff_fundamental = 0.0f;
  PassTrace trace;
  trace.applied_fundamental_hz = config.fundamental_hz;
  for (size_t begin = 0; begin < tracking.size(); begin += static_cast<size_t>(config.frame_size)) {
    const size_t end = std::min(tracking.size(), begin + static_cast<size_t>(config.frame_size));
    target_fundamental =
        estimate_fundamental(tracking, begin, end, sample_rate, config, target_fundamental);
    for (size_t i = begin; i < end; ++i) {
      fundamental = tracker.process(tracking[i], target_fundamental, sample_rate, config);
      trace.fundamental_drift_hz =
          std::max(trace.fundamental_drift_hz, std::abs(fundamental - config.fundamental_hz));
      if (std::abs(fundamental - last_coeff_fundamental) >
          kCoeffRefreshRatio * last_coeff_fundamental) {
        for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
          const float frequency = fundamental * static_cast<float>(harmonic);
          if (frequency >= static_cast<float>(sample_rate) * 0.5f) break;
          for (auto& cascade : cascades) {
            cascade[static_cast<size_t>(harmonic - 1)].set_coefficients(
                frequency, static_cast<float>(sample_rate), config.q);
          }
        }
        last_coeff_fundamental = fundamental;
        trace.applied_fundamental_hz = fundamental;
      }
      int applied = 0;
      for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
        const float frequency = fundamental * static_cast<float>(harmonic);
        if (frequency >= static_cast<float>(sample_rate) * 0.5f) break;
        for (size_t channel = 0; channel < channel_count; ++channel) {
          std::vector<float>& samples = *channels[channel];
          samples[i] = cascades[channel][static_cast<size_t>(harmonic - 1)].process(samples[i]);
        }
        ++applied;
      }
      trace.notched_harmonics = applied;
    }
  }
  return trace;
}

void require_stereo_pair(const Audio& left, const Audio& right) {
  if (left.empty() || right.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (left.size() != right.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channels must have the same length");
  }
  if (left.sample_rate() != right.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "stereo channels must share a sample rate");
  }
}

void fill_report(DehumReport* report, const PassTrace& trace, const float* samples, size_t size,
                 int sample_rate, const DehumConfig& config) {
  if (report == nullptr) return;
  report->detected = detect_hum(samples, size, sample_rate, config);
  report->notched_harmonics = trace.notched_harmonics;
  report->applied_fundamental_hz = trace.applied_fundamental_hz;
  report->fundamental_drift_hz = trace.fundamental_drift_hz;
}

}  // namespace

void validate_config(const DehumConfig& config) {
  if (!std::isfinite(config.fundamental_hz) || !(config.fundamental_hz > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "fundamental_hz must be finite and positive");
  }
  if (config.harmonics < 1 || config.harmonics > kDehumMaxHarmonics) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "harmonics must be in [1, " + std::to_string(kDehumMaxHarmonics) + "]");
  }
  if (!std::isfinite(config.q) || !(config.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "q must be finite and positive");
  }
  if (!std::isfinite(config.search_range_hz) || config.search_range_hz < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "search_range_hz must be finite and non-negative");
  }
  if (!(config.adaptation >= 0.0f) || !(config.adaptation <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "adaptation must be in [0, 1]");
  }
  if (config.frame_size < 16) {
    throw SonareException(ErrorCode::InvalidParameter, "frame_size must be at least 16");
  }
  if (!std::isfinite(config.pll_bandwidth) || config.pll_bandwidth < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "pll_bandwidth must be finite and non-negative");
  }
}

HumDetection detect_hum(const float* samples, size_t size, int sample_rate,
                        const DehumConfig& config) {
  const auto validated = Validated<DehumConfig>::make(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (samples == nullptr || size == 0) return {};

  const std::vector<float> buffer(samples, samples + size);
  const size_t frame = detection_frame(size, sample_rate);
  const float nyquist = static_cast<float>(sample_rate) * 0.5f;

  std::vector<float> fundamentals;
  std::vector<float> prominences;
  std::array<std::vector<float>, kDehumMaxHarmonics> levels;
  std::array<std::vector<float>, kDehumMaxHarmonics> floors;
  for (size_t begin = 0; begin + frame <= size; begin += frame) {
    const size_t end = begin + frame;
    const FundamentalSearch search =
        search_fundamental(buffer, begin, end, sample_rate, validated.get(), config.fundamental_hz);
    fundamentals.push_back(search.best_hz);
    prominences.push_back(prominence_of(search));
    for (int harmonic = 1; harmonic <= kDehumMaxHarmonics; ++harmonic) {
      const size_t slot = static_cast<size_t>(harmonic - 1);
      const float hz = search.best_hz * static_cast<float>(harmonic);
      if (!(hz < nyquist)) {
        levels[slot].push_back(kFloorDb);
        floors[slot].push_back(kFloorDb);
        continue;
      }
      levels[slot].push_back(projection_dbfs(buffer, begin, end, hz, sample_rate));
      // The floor is read between harmonics, where hum has nothing: a level that
      // does not stand above both sides is the programme material, not hum.
      const float below = hz - 0.5f * search.best_hz;
      const float above = hz + 0.5f * search.best_hz;
      float floor_db = projection_dbfs(buffer, begin, end, below, sample_rate);
      if (above < nyquist) {
        floor_db = std::max(floor_db, projection_dbfs(buffer, begin, end, above, sample_rate));
      }
      floors[slot].push_back(floor_db);
    }
  }

  HumDetection detection;
  detection.fundamental_hz = median_of(fundamentals);
  detection.fundamental_prominence = median_of(prominences);
  for (int harmonic = 1; harmonic <= kDehumMaxHarmonics; ++harmonic) {
    const size_t slot = static_cast<size_t>(harmonic - 1);
    const float level = median_of(levels[slot]);
    detection.harmonic_dbfs[slot] = std::max(level, kFloorDb);
    if (level > kFloorDb && level > median_of(floors[slot]) + kHumHarmonicMarginDb) {
      ++detection.harmonics;
    }
  }
  return detection;
}

Audio dehum(const Audio& audio, const DehumConfig& config) { return dehum(audio, config, nullptr); }

Audio dehum(const Audio& audio, const DehumConfig& config, DehumReport* report) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DehumConfig>::make(config);
  const int sample_rate = audio.sample_rate();

  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  PassTrace trace;
  if (config.adaptive) {
    const std::vector<float> tracking = samples;
    const std::vector<std::vector<float>*> channels = {&samples};
    trace = run_adaptive(channels, tracking, sample_rate, validated.get());
  } else {
    trace = run_fixed(samples, sample_rate, validated.get());
  }
  fill_report(report, trace, audio.data(), audio.size(), sample_rate, validated.get());
  return Audio::from_vector(std::move(samples), sample_rate);
}

DehumStereoResult dehum_stereo(const Audio& left, const Audio& right, const DehumConfig& config) {
  require_stereo_pair(left, right);
  const auto validated = Validated<DehumConfig>::make(config);
  const int sample_rate = left.sample_rate();
  const size_t size = left.size();

  std::vector<float> left_samples(left.data(), left.data() + size);
  std::vector<float> right_samples(right.data(), right.data() + size);
  PassTrace left_trace;
  PassTrace right_trace;
  if (config.adaptive) {
    std::vector<float> tracking(size, 0.0f);
    for (size_t i = 0; i < size; ++i) tracking[i] = 0.5f * (left_samples[i] + right_samples[i]);
    const std::vector<std::vector<float>*> channels = {&left_samples, &right_samples};
    left_trace = run_adaptive(channels, tracking, sample_rate, validated.get());
    right_trace = left_trace;
  } else {
    left_trace = run_fixed(left_samples, sample_rate, validated.get());
    right_trace = run_fixed(right_samples, sample_rate, validated.get());
  }

  DehumStereoResult result;
  fill_report(&result.left_report, left_trace, left.data(), size, sample_rate, validated.get());
  fill_report(&result.right_report, right_trace, right.data(), size, sample_rate, validated.get());
  result.left = Audio::from_vector(std::move(left_samples), sample_rate);
  result.right = Audio::from_vector(std::move(right_samples), sample_rate);
  return result;
}

}  // namespace sonare::mastering::repair
