#include "mastering/repair/dehum.h"

#include <Eigen/Dense>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "core/stereo_pair.h"
#include "mastering/common/noise_profile.h"
#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/repair/dehum_streaming.h"
#include "rt/biquad_design.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

using sonare::constants::kEpsilon;
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

/// One harmonic's subtraction: a running estimate of its quadrature amplitudes,
/// corrected from the residual it leaves behind.
/// @details The pair is the harmonic's amplitude and phase in Cartesian form,
///   so @c estimate is a resynthesis of the tone they describe and the output is
///   the input minus it. Programme material at the same frequency is
///   uncorrelated with the reference and drives the estimate nowhere, which is
///   what separates this from a notch: the notch removes the band, this removes
///   the tone.
struct HarmonicCanceller {
  float cosine_gain = 0.0f;
  float sine_gain = 0.0f;
  float step = 0.0f;

  /// @details The correction decays at step*sample_rate/2, which is the same
  ///   selectivity a notch of bandwidth @p frequency_hz / @p q has, expressed as
  ///   a rate rather than as a pole pair -- so q means one thing in both modes.
  ///   The update contracts the estimate only for steps below 2, so a band too
  ///   wide for the rate is held at 1, where the correction lands in one sample.
  void set_selectivity(float frequency_hz, float sample_rate, float q) {
    step = std::min(kTwoPi * (frequency_hz / q) / sample_rate, 1.0f);
  }

  float process(float x, float cosine, float sine) {
    const float residual = x - (cosine_gain * cosine + sine_gain * sine);
    cosine_gain += step * residual * cosine;
    sine_gain += step * residual * sine;
    return residual;
  }
};

/// Corner of the detector's low-pass as a fraction of the tracked frequency.
/// Two cascaded poles at 0.06*f0 put the detector's own image at 2*f0 sixty dB
/// down and still sit six times above the loop bandwidth. The tracked frequency
/// moved by under a factor of two across a sweep from 0.02 to 0.5, so the value
/// is a margin on both bounds rather than a tuned one.
constexpr float kDetectorCornerRatio = 0.06f;

/// Damping of the PI loop. Critically damped, so a frequency step settles
/// without overshoot rather than swinging past the harmonic it is tracking.
constexpr float kLoopDamping = 1.0f;

/// Rate at which the applied frequency relaxes towards the frame estimate, as a
/// fraction of the loop bandwidth. A frame cannot resolve the window it chooses
/// from, so its winner jumps a grid step between frames, and an order under the
/// loop keeps that jitter off the applied frequency. The ordering is what
/// matters: slewed four times faster than the loop, as the removed pull term
/// was, the coarse estimate overrides the loop instead of seeding it and the
/// harmonics come out five to seven dB less removed.
constexpr float kAnchorSlewRatio = 0.1f;

/// Decay of the PI integrator as a fraction of the loop's natural frequency.
/// An order below it, so the leak cannot fight the tracking while still
/// returning the offset to the anchor over seconds when nothing locks.
constexpr float kIntegratorLeakRatio = 0.1f;

/// Second-order PLL around the per-frame frequency estimate.
/// @details A quadrature detector, a low-pass pair, and a PI loop filter. The
///   products a phase detector forms carry an image at twice the tracked
///   frequency; without the low-pass that image reached the frequency
///   integrator and modulated the applied frequency at 2*f0. Dividing the
///   quadrature arm by the pair's magnitude leaves the sine of the phase error,
///   so the loop gain no longer scales with the input level.
struct PllTracker {
  float frequency_hz = 50.0f;
  float phase = 0.0f;
  float anchor_hz = 50.0f;  ///< The frame estimate, slewed to a per-sample rate.
  float in_phase = 0.0f;
  float in_phase_smoothed = 0.0f;
  float quadrature = 0.0f;
  float quadrature_smoothed = 0.0f;
  float offset_hz = 0.0f;  ///< PI integrator: the standing error from the anchor.

  float process(float sample, float target_hz, int sample_rate, const DehumConfig& config) {
    const float rate = static_cast<float>(sample_rate);
    const float alpha = std::min(1.0f, kTwoPi * kDetectorCornerRatio * target_hz / rate);
    in_phase += alpha * (sample * std::sin(phase) - in_phase);
    in_phase_smoothed += alpha * (in_phase - in_phase_smoothed);
    quadrature += alpha * (sample * std::cos(phase) - quadrature);
    quadrature_smoothed += alpha * (quadrature - quadrature_smoothed);

    const float magnitude = std::sqrt(in_phase_smoothed * in_phase_smoothed +
                                      quadrature_smoothed * quadrature_smoothed);
    const float error = quadrature_smoothed / std::max(magnitude, kEpsilon);

    const float loop_hz = config.pll_bandwidth * target_hz;
    anchor_hz += kAnchorSlewRatio * kTwoPi * loop_hz / rate * (target_hz - anchor_hz);
    offset_hz += kTwoPi * loop_hz * loop_hz * error / rate;
    offset_hz -= kIntegratorLeakRatio * kTwoPi * loop_hz * offset_hz / rate;
    offset_hz = std::clamp(offset_hz, -config.search_range_hz, config.search_range_hz);

    frequency_hz = anchor_hz + 2.0f * kLoopDamping * loop_hz * error + offset_hz;
    frequency_hz =
        std::clamp(frequency_hz, std::max(1.0f, config.fundamental_hz - config.search_range_hz),
                   std::min(static_cast<float>(sample_rate) * 0.49f,
                            config.fundamental_hz + config.search_range_hz));
    phase += kTwoPi * frequency_hz / rate;
    if (phase > kTwoPi) {
      phase -= kTwoPi;
    }
    return frequency_hz;
  }
};

// @p index_offset places sample i of @p samples at stream index i + index_offset, so a frame held
// on its own projects with the phase it has in the whole signal.
double projected_energy(const std::vector<float>& samples, size_t begin, size_t end,
                        float frequency_hz, int sample_rate, size_t index_offset = 0) {
  double sin_sum = 0.0;
  double cos_sum = 0.0;
  for (size_t i = begin; i < end; ++i) {
    const double phase = kTwoPiD * frequency_hz * static_cast<double>(i + index_offset) /
                         static_cast<double>(sample_rate);
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

FundamentalSearch search_fundamental(
    const std::vector<float>& samples, size_t begin, size_t end, int sample_rate,
    const DehumConfig& config, float previous_hz,
    const std::vector<const std::vector<float>*>* channels = nullptr, size_t index_offset = 0) {
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
    double energy = projected_energy(samples, begin, end, hz, sample_rate, index_offset);
    if (channels != nullptr) {
      // The primary vector is also the first entry in the stereo list.
      for (const std::vector<float>* channel : *channels) {
        if (channel != &samples) {
          energy += projected_energy(*channel, begin, end, hz, sample_rate, index_offset);
        }
      }
    }
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
                           int sample_rate, const DehumConfig& config, float previous_hz,
                           const std::vector<const std::vector<float>*>* channels = nullptr,
                           size_t index_offset = 0) {
  const FundamentalSearch search = search_fundamental(samples, begin, end, sample_rate, config,
                                                      previous_hz, channels, index_offset);
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

/// What one channel's pass did, in either mode, for the report.
struct PassTrace {
  int notched_harmonics = 0;
  float applied_fundamental_hz = 0.0f;
  float fundamental_drift_hz = 0.0f;
};

PassTrace run_fixed_notch(std::vector<float>& samples, int sample_rate, const DehumConfig& config) {
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

/// Smallest ratio of the reference Gram matrix's eigenvalues for which the opening window
/// separates the harmonics well enough to seed them.
constexpr double kSeedMinConditionRatio = 0.05;

/// Seeds every canceller from one joint least-squares fit of the harmonic references over the
/// opening window.
/// @details Starting the pairs at zero costs the loop its own time constant -- an eighth of a
///   second at the default q on a 50 Hz series -- during which the harmonic passes through
///   unattenuated. The references are orthogonal only over whole cycles of every harmonic, so a
///   per-harmonic projection overcounts on a shorter or non-integer window; the fit is solved
///   jointly, and a window that cannot tell the harmonics apart leaves every pair unseeded.
void seed_cancellers(std::vector<HarmonicCanceller>& cancellers,
                     const std::vector<double>& increments, const std::vector<float>& samples,
                     size_t count) {
  const size_t unknowns = 2 * cancellers.size();
  if (count == 0 || unknowns == 0 || count < unknowns) return;
  Eigen::MatrixXd gram = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(unknowns),
                                               static_cast<Eigen::Index>(unknowns));
  Eigen::VectorXd projection = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(unknowns));
  Eigen::VectorXd row(static_cast<Eigen::Index>(unknowns));
  for (size_t i = 0; i < count; ++i) {
    for (size_t k = 0; k < cancellers.size(); ++k) {
      const double phase = increments[k] * static_cast<double>(i);
      row(static_cast<Eigen::Index>(2 * k)) = std::cos(phase);
      row(static_cast<Eigen::Index>(2 * k + 1)) = std::sin(phase);
    }
    gram.noalias() += row * row.transpose();
    projection += row * static_cast<double>(samples[i]);
  }
  const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigen(gram);
  const double largest = eigen.eigenvalues().maxCoeff();
  if (!(largest > 0.0) || eigen.eigenvalues().minCoeff() < kSeedMinConditionRatio * largest) {
    return;
  }
  const Eigen::VectorXd gains =
      eigen.eigenvectors() * (eigen.eigenvalues().cwiseInverse().asDiagonal() *
                              (eigen.eigenvectors().transpose() * projection));
  for (size_t k = 0; k < cancellers.size(); ++k) {
    cancellers[k].cosine_gain = static_cast<float>(gains(static_cast<Eigen::Index>(2 * k)));
    cancellers[k].sine_gain = static_cast<float>(gains(static_cast<Eigen::Index>(2 * k + 1)));
  }
}

PassTrace run_fixed_subtract(std::vector<float>& samples, int sample_rate,
                             const DehumConfig& config) {
  PassTrace trace;
  trace.applied_fundamental_hz = config.fundamental_hz;
  const float rate = static_cast<float>(sample_rate);
  const size_t seed_window = detection_frame(samples.size(), sample_rate);

  std::vector<HarmonicCanceller> cancellers;
  std::vector<double> increments;
  std::vector<double> phases;
  for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
    const float frequency = config.fundamental_hz * static_cast<float>(harmonic);
    if (frequency >= rate * 0.5f) break;
    HarmonicCanceller canceller;
    canceller.set_selectivity(frequency, rate, config.q);
    cancellers.push_back(canceller);
    increments.push_back(kTwoPiD * frequency / static_cast<double>(sample_rate));
    phases.push_back(0.0);
    ++trace.notched_harmonics;
  }
  seed_cancellers(cancellers, increments, samples, seed_window);

  // The references are the same phase origin the seed projected onto, so a
  // cancellation that was correct over the window stays correct past it.
  for (size_t i = 0; i < samples.size(); ++i) {
    float residual = samples[i];
    for (size_t k = 0; k < cancellers.size(); ++k) {
      residual = cancellers[k].process(residual, static_cast<float>(std::cos(phases[k])),
                                       static_cast<float>(std::sin(phases[k])));
      phases[k] += increments[k];
      if (phases[k] > kTwoPiD) phases[k] -= kTwoPiD;
    }
    samples[i] = residual;
  }
  return trace;
}

PassTrace run_fixed(std::vector<float>& samples, int sample_rate, const DehumConfig& config) {
  if (config.mode == DehumMode::Subtract) return run_fixed_subtract(samples, sample_rate, config);
  return run_fixed_notch(samples, sample_rate, config);
}

size_t strongest_tracking_channel(const std::vector<const std::vector<float>*>& channels,
                                  int sample_rate, const DehumConfig& config) {
  if (channels.empty()) return 0;
  const size_t frame = detection_frame(channels.front()->size(), sample_rate);
  size_t strongest = 0;
  double strongest_energy = -1.0;
  for (size_t index = 0; index < channels.size(); ++index) {
    double channel_energy = 0.0;
    for (size_t begin = 0; begin < channels[index]->size(); begin += frame) {
      const size_t end = std::min(channels[index]->size(), begin + frame);
      const FundamentalSearch search = search_fundamental(*channels[index], begin, end, sample_rate,
                                                          config, config.fundamental_hz);
      channel_energy += std::max(0.0, search.best_energy);
    }
    if (channel_energy > strongest_energy) {
      strongest_energy = channel_energy;
      strongest = index;
    }
  }
  return strongest;
}

/// One adaptive pass's state from frame to frame, so the offline pass and the insert run the
/// same loop. @p tracking drives the search and the PLL; for a single channel it is that
/// channel's own unfiltered samples, which is what makes the mono result identical whichever
/// entrypoint produced it. When @p search_channels is set, it points to immutable original
/// channels used for the summed stereo search.
class AdaptivePass {
 public:
  AdaptivePass(size_t channel_count, int sample_rate, const DehumConfig& config)
      : config_(config),
        sample_rate_(sample_rate),
        subtracting_(config.mode == DehumMode::Subtract),
        fundamental_(config.fundamental_hz),
        target_fundamental_(config.fundamental_hz) {
    const size_t harmonic_count = static_cast<size_t>(config.harmonics);
    tracker_.frequency_hz = fundamental_;
    tracker_.anchor_hz = fundamental_;
    // Unlike the fixed path the cancellers start at zero: their phase origin is
    // the tracker's own oscillator, which does not exist before the pass runs, so
    // there is nothing for a projection to be referred to. Each converges over
    // the same time constant the notch it replaces rings for.
    if (subtracting_) {
      cancellers_.assign(channel_count, std::vector<HarmonicCanceller>(harmonic_count));
      for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
        HarmonicCanceller seed;
        seed.set_selectivity(fundamental_ * static_cast<float>(harmonic),
                             static_cast<float>(sample_rate), config.q);
        for (auto& channel : cancellers_) channel[static_cast<size_t>(harmonic - 1)] = seed;
      }
    } else {
      cascades_.assign(channel_count, std::vector<Notch>(harmonic_count));
      for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
        const Notch seed = make_notch(fundamental_ * static_cast<float>(harmonic),
                                      static_cast<float>(sample_rate), config.q);
        for (auto& cascade : cascades_) cascade[static_cast<size_t>(harmonic - 1)] = seed;
      }
    }
    trace_.applied_fundamental_hz = config.fundamental_hz;
  }

  /// Filters [begin, end) of every channel on the fundamental estimated from the same span of
  /// @p tracking, whose sample i sits at stream index i + @p index_offset.
  void run_frame(const std::vector<std::vector<float>*>& channels,
                 const std::vector<float>& tracking, size_t begin, size_t end,
                 const std::vector<const std::vector<float>*>* search_channels,
                 size_t index_offset) {
    const DehumConfig& config = config_;
    const int sample_rate = sample_rate_;
    const bool subtracting = subtracting_;
    const size_t channel_count = channels.size();
    float& fundamental = fundamental_;
    float& target_fundamental = target_fundamental_;
    PllTracker& tracker = tracker_;
    auto& cascades = cascades_;
    auto& cancellers = cancellers_;
    float& last_coeff_fundamental = last_coeff_fundamental_;
    PassTrace& trace = trace_;
    if (search_channels == nullptr) {
      target_fundamental = estimate_fundamental(tracking, begin, end, sample_rate, config,
                                                target_fundamental, nullptr, index_offset);
    } else {
      target_fundamental =
          estimate_fundamental(*search_channels->front(), begin, end, sample_rate, config,
                               target_fundamental, search_channels, index_offset);
    }
    for (size_t i = begin; i < end; ++i) {
      fundamental = tracker.process(tracking[i], target_fundamental, sample_rate, config);
      trace.fundamental_drift_hz =
          std::max(trace.fundamental_drift_hz, std::abs(fundamental - config.fundamental_hz));
      if (std::abs(fundamental - last_coeff_fundamental) >
          kCoeffRefreshRatio * last_coeff_fundamental) {
        for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
          const float frequency = fundamental * static_cast<float>(harmonic);
          if (frequency >= static_cast<float>(sample_rate) * 0.5f) break;
          const size_t slot = static_cast<size_t>(harmonic - 1);
          if (subtracting) {
            for (auto& channel : cancellers) {
              channel[slot].set_selectivity(frequency, static_cast<float>(sample_rate), config.q);
            }
          } else {
            for (auto& cascade : cascades) {
              cascade[slot].set_coefficients(frequency, static_cast<float>(sample_rate), config.q);
            }
          }
        }
        last_coeff_fundamental = fundamental;
        trace.applied_fundamental_hz = fundamental;
      }
      int applied = 0;
      for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
        const float frequency = fundamental * static_cast<float>(harmonic);
        if (frequency >= static_cast<float>(sample_rate) * 0.5f) break;
        const size_t slot = static_cast<size_t>(harmonic - 1);
        if (subtracting) {
          // The tracker's oscillator is the fundamental's phase, so a harmonic's
          // reference is that phase multiplied -- one oscillator for the series
          // rather than one per harmonic drifting apart from the others.
          const float harmonic_phase = static_cast<float>(harmonic) * tracker.phase;
          const float cosine = std::cos(harmonic_phase);
          const float sine = std::sin(harmonic_phase);
          for (size_t channel = 0; channel < channel_count; ++channel) {
            std::vector<float>& samples = *channels[channel];
            samples[i] = cancellers[channel][slot].process(samples[i], cosine, sine);
          }
        } else {
          for (size_t channel = 0; channel < channel_count; ++channel) {
            std::vector<float>& samples = *channels[channel];
            samples[i] = cascades[channel][slot].process(samples[i]);
          }
        }
        ++applied;
      }
      trace.notched_harmonics = applied;
    }
  }

  const PassTrace& trace() const { return trace_; }

 private:
  // The PLL fundamental drifts slowly (pll_bandwidth is small), so recomputing
  // the RBJ notch coefficients (sin/cos/division per harmonic) or the
  // cancellers' step on every sample is wasteful. Refresh them only when the
  // tracked fundamental has moved by more than a small relative amount since the
  // last refresh; the filter state carries across untouched, so the adaptive
  // tracking behavior is preserved.
  static constexpr float kCoeffRefreshRatio = 1e-3f;

  DehumConfig config_;
  int sample_rate_ = 0;
  bool subtracting_ = false;
  float fundamental_ = 0.0f;
  float target_fundamental_ = 0.0f;
  PllTracker tracker_;
  std::vector<std::vector<Notch>> cascades_;
  std::vector<std::vector<HarmonicCanceller>> cancellers_;
  float last_coeff_fundamental_ = 0.0f;
  PassTrace trace_;
};

PassTrace run_adaptive(const std::vector<std::vector<float>*>& channels,
                       const std::vector<float>& tracking, int sample_rate,
                       const DehumConfig& config,
                       const std::vector<const std::vector<float>*>* search_channels = nullptr) {
  AdaptivePass pass(channels.size(), sample_rate, config);
  for (size_t begin = 0; begin < tracking.size(); begin += static_cast<size_t>(config.frame_size)) {
    const size_t end = std::min(tracking.size(), begin + static_cast<size_t>(config.frame_size));
    pass.run_frame(channels, tracking, begin, end, search_channels, 0);
  }
  return pass.trace();
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
  if (!std::isfinite(config.fundamental_hz) || !(config.fundamental_hz > 0.0f) ||
      config.fundamental_hz > kDehumMaxFundamentalHz) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "fundamental_hz must be in (0, " +
                              std::to_string(static_cast<int>(kDehumMaxFundamentalHz)) + "]");
  }
  if (config.harmonics < 1 || config.harmonics > kDehumMaxHarmonics) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "harmonics must be in [1, " + std::to_string(kDehumMaxHarmonics) + "]");
  }
  if (!std::isfinite(config.q) || !(config.q > 0.0f) || config.q > kDehumMaxQ) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "q must be in (0, " + std::to_string(static_cast<int>(kDehumMaxQ)) + "]");
  }
  if (!std::isfinite(config.search_range_hz) || config.search_range_hz < 0.0f ||
      config.search_range_hz > kDehumMaxSearchRangeHz) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "search_range_hz must be in [0, " +
                              std::to_string(static_cast<int>(kDehumMaxSearchRangeHz)) + "]");
  }
  if (!(config.adaptation >= 0.0f) || !(config.adaptation <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "adaptation must be in [0, 1]");
  }
  if (config.frame_size < 16 || config.frame_size > kDehumMaxFrameSize) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "frame_size must be in [16, " + std::to_string(kDehumMaxFrameSize) + "]");
  }
  if (!std::isfinite(config.pll_bandwidth) || config.pll_bandwidth < 0.0f ||
      config.pll_bandwidth > kDehumMaxPllBandwidth) {
    throw SonareException(ErrorCode::InvalidParameter, "pll_bandwidth must be in [0, 1]");
  }
  if (config.mode != DehumMode::Subtract && config.mode != DehumMode::Notch) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid dehum mode");
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
  const Audio* channels[2] = {&left, &right};
  std::vector<Audio> out;
  const std::vector<DehumReport> reports = dehum_linked(channels, 2, &out, config);

  DehumStereoResult result;
  result.left_report = reports[0];
  result.right_report = reports[1];
  result.left = std::move(out[0]);
  result.right = std::move(out[1]);
  return result;
}

std::vector<DehumReport> dehum_linked(const Audio* const* channels, size_t channel_count,
                                      std::vector<Audio>* out, const DehumConfig& config) {
  const auto validated = Validated<DehumConfig>::make(config);
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "dehum output must not be null");
  }
  if (channels == nullptr || channel_count == 0 || channels[0] == nullptr || channels[0]->empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  common::validate_linked_channels(channels, channel_count);
  out->clear();
  // One channel tracks on its own samples, which is the mono entry's search.
  if (channel_count == 1) {
    std::vector<DehumReport> reports(1);
    out->push_back(dehum(*channels[0], validated.get(), &reports[0]));
    return reports;
  }

  const int sample_rate = channels[0]->sample_rate();
  const size_t size = channels[0]->size();
  std::vector<std::vector<float>> samples;
  samples.reserve(channel_count);
  for (size_t c = 0; c < channel_count; ++c) {
    samples.emplace_back(channels[c]->data(), channels[c]->data() + size);
  }
  std::vector<PassTrace> traces(channel_count);
  if (config.adaptive) {
    const std::vector<std::vector<float>> originals = samples;
    std::vector<const std::vector<float>*> search_channels;
    std::vector<std::vector<float>*> targets;
    for (size_t c = 0; c < channel_count; ++c) {
      search_channels.push_back(&originals[c]);
      targets.push_back(&samples[c]);
    }
    const size_t reference = strongest_tracking_channel(search_channels, sample_rate, config);
    const std::vector<float> tracking = *search_channels[reference];
    const PassTrace shared =
        run_adaptive(targets, tracking, sample_rate, validated.get(), &search_channels);
    for (PassTrace& trace : traces) trace = shared;
  } else {
    for (size_t c = 0; c < channel_count; ++c) {
      traces[c] = run_fixed(samples[c], sample_rate, validated.get());
    }
  }

  std::vector<DehumReport> reports(channel_count);
  out->reserve(channel_count);
  for (size_t c = 0; c < channel_count; ++c) {
    fill_report(&reports[c], traces[c], channels[c]->data(), size, sample_rate, validated.get());
    out->push_back(Audio::from_vector(std::move(samples[c]), sample_rate));
  }
  return reports;
}

// ------------------------------------------------------------------ streaming

struct StreamingDehum::State {
  int sample_rate = 0;
  int max_channels = 0;
  int max_block_size = 0;
  // Fixed notch: one cascade per channel.
  std::vector<std::vector<Notch>> notches;
  // Fixed subtract: one canceller set per channel on references shared by every channel.
  std::vector<std::vector<HarmonicCanceller>> cancellers;
  std::vector<double> increments;
  std::vector<double> phases;
  // Adaptive: one pass per channel, the block being filled and the block being emitted.
  std::vector<std::unique_ptr<AdaptivePass>> passes;
  std::vector<std::vector<float>> filling;
  std::vector<std::vector<float>> emitting;
  std::vector<std::vector<float>*> target;
  int fill = 0;
  size_t blocks_done = 0;
};

StreamingDehum::StreamingDehum(const DehumConfig& config)
    : config_(Validated<DehumConfig>::make(config).get()), state_(std::make_unique<State>()) {}

StreamingDehum::~StreamingDehum() = default;

void StreamingDehum::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void StreamingDehum::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "StreamingDehum");
  state_->sample_rate = static_cast<int>(std::lround(sample_rate));
  state_->max_channels = max_channels;
  state_->max_block_size = max_block_size;
  reset();
}

void StreamingDehum::reset() {
  State& state = *state_;
  const auto channels = static_cast<size_t>(state.max_channels);
  const float rate = static_cast<float>(state.sample_rate);
  state.notches.assign(channels, {});
  state.cancellers.assign(channels, {});
  state.increments.clear();
  state.phases.clear();
  state.passes.clear();
  state.filling.assign(channels, {});
  state.emitting.assign(channels, {});
  state.target.assign(1, nullptr);
  state.fill = 0;
  state.blocks_done = 0;
  if (state.sample_rate <= 0) return;
  if (config_.adaptive) {
    const auto frame = static_cast<size_t>(config_.frame_size);
    for (size_t c = 0; c < channels; ++c) {
      state.passes.push_back(std::make_unique<AdaptivePass>(1, state.sample_rate, config_));
      state.filling[c].assign(frame, 0.0f);
      state.emitting[c].assign(frame, 0.0f);
    }
    return;
  }
  for (int harmonic = 1; harmonic <= config_.harmonics; ++harmonic) {
    const float frequency = config_.fundamental_hz * static_cast<float>(harmonic);
    if (frequency >= rate * 0.5f) break;
    for (size_t c = 0; c < channels; ++c) {
      if (config_.mode == DehumMode::Subtract) {
        HarmonicCanceller canceller;
        canceller.set_selectivity(frequency, rate, config_.q);
        state.cancellers[c].push_back(canceller);
      } else {
        state.notches[c].push_back(make_notch(frequency, rate, config_.q));
      }
    }
    if (config_.mode == DehumMode::Subtract) {
      state.increments.push_back(kTwoPiD * frequency / static_cast<double>(state.sample_rate));
      state.phases.push_back(0.0);
    }
  }
}

void StreamingDehum::process(float* const* channels, int num_channels, int num_samples) {
  State& state = *state_;
  ensure_prepared(state.sample_rate > 0, "StreamingDehum");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  if (num_channels > state.max_channels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared StreamingDehum capacity");
  }
  if (num_samples > state.max_block_size) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared StreamingDehum block size");
  }

  if (!config_.adaptive && config_.mode == DehumMode::Notch) {
    for (int c = 0; c < num_channels; ++c) {
      auto& cascade = state.notches[static_cast<size_t>(c)];
      for (int i = 0; i < num_samples; ++i) {
        float sample = channels[c][i];
        for (Notch& notch : cascade) sample = notch.process(sample);
        channels[c][i] = sample;
      }
    }
    return;
  }

  if (!config_.adaptive) {
    // The references advance once per sample whichever channels the block carries.
    for (int i = 0; i < num_samples; ++i) {
      for (int c = 0; c < num_channels; ++c) {
        auto& set = state.cancellers[static_cast<size_t>(c)];
        float residual = channels[c][i];
        for (size_t k = 0; k < set.size(); ++k) {
          residual = set[k].process(residual, static_cast<float>(std::cos(state.phases[k])),
                                    static_cast<float>(std::sin(state.phases[k])));
        }
        channels[c][i] = residual;
      }
      for (size_t k = 0; k < state.phases.size(); ++k) {
        state.phases[k] += state.increments[k];
        if (state.phases[k] > kTwoPiD) state.phases[k] -= kTwoPiD;
      }
    }
    return;
  }

  // Adaptive: each block is estimated before it is filtered, so it is emitted one block late.
  const auto frame = static_cast<size_t>(config_.frame_size);
  for (int i = 0; i < num_samples; ++i) {
    const auto slot = static_cast<size_t>(state.fill);
    for (int c = 0; c < num_channels; ++c) {
      const auto channel = static_cast<size_t>(c);
      const float incoming = channels[c][i];
      channels[c][i] = state.emitting[channel][slot];
      state.filling[channel][slot] = incoming;
    }
    if (++state.fill < config_.frame_size) continue;
    state.fill = 0;
    for (size_t c = 0; c < static_cast<size_t>(num_channels); ++c) {
      std::copy(state.filling[c].begin(), state.filling[c].end(), state.emitting[c].begin());
      state.target[0] = &state.emitting[c];
      state.passes[c]->run_frame(state.target, state.filling[c], 0, frame, nullptr,
                                 state.blocks_done * frame);
    }
    ++state.blocks_done;
  }
}

int StreamingDehum::tail_samples() const noexcept {
  const double rate = state_->sample_rate > 0 ? state_->sample_rate : 48000.0;
  const double fundamental =
      config_.adaptive
          ? std::max(1.0, static_cast<double>(config_.fundamental_hz - config_.search_range_hz))
          : static_cast<double>(config_.fundamental_hz);
  rt::TailBudget tail;
  for (int harmonic = 1; harmonic <= config_.harmonics; ++harmonic) {
    const double frequency = fundamental * harmonic;
    if (frequency >= rate * 0.5) break;
    if (config_.mode == DehumMode::Subtract) {
      const double step = std::min(kTwoPiD * (frequency / config_.q) / rate, 1.0);
      tail.decay(1.0 - 0.5 * step);
    } else {
      tail.section(rt::rbj_notch(static_cast<float>(kTwoPiD * frequency / rate), config_.q));
    }
  }
  return tail.samples();
}

int StreamingDehum::latency_samples() const noexcept {
  return config_.adaptive ? config_.frame_size : 0;
}

}  // namespace sonare::mastering::repair