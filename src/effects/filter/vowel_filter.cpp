#include "effects/filter/vowel_filter.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/parameter_domain.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"

namespace sonare::effects::filter {
namespace {

// The hardware-measured vowel (a): a peaking-form fit of three sections, each
// 20 dB, summed with signed weights beside a weighted direct path, then a
// constant. Each peaking section is 1 + (A^2 - 1) * (unity-peak band-pass of
// Q = A * q), which is how the bank realises it.
constexpr std::array<float, kVowelBandCount> kFitHz = {1003.972f, 1312.844f, 1903.283f};
constexpr std::array<float, kVowelBandCount> kFitQ = {1.582894f, 0.764481f, 2.161686f};
constexpr std::array<float, kVowelBandCount> kFitWeight = {0.162137595f, -0.027334462f,
                                                           0.080888403f};
constexpr float kFitDirectWeight = -0.150716821f;
constexpr float kFitConstantDb = 3.064f;
constexpr float kFitPeakingDb = 20.0f;

// Japanese male mean formants (Hz) per vowel, printed order a i u e o.
// Source: Yazawa and Kondo (2019), short vowels at the midpoint, 8 male speakers.
constexpr float kJapaneseHz[kVowelCount][kVowelBandCount] = {
    {687.0f, 1283.0f, 2605.0f},  // a
    {301.0f, 2154.0f, 2929.0f},  // i
    {348.0f, 1435.0f, 2355.0f},  // u
    {443.0f, 1947.0f, 2611.0f},  // e
    {462.0f, 949.0f, 2544.0f},   // o
};

// Per-vowel bank frequencies. a: the fit itself (measured). i u e o: the fit's
// triple scaled by each vowel's formant ratio to the Japanese /a/ (carried),
// sharing the fit's q and weights.
struct VowelTable {
  float hz[kVowelCount][kVowelBandCount];
};

constexpr VowelTable make_table() {
  VowelTable table{};
  for (int v = 0; v < kVowelCount; ++v) {
    for (int b = 0; b < kVowelBandCount; ++b) {
      table.hz[v][b] = v == 0
                           ? kFitHz[static_cast<size_t>(b)]
                           : kFitHz[static_cast<size_t>(b)] * kJapaneseHz[v][b] / kJapaneseHz[0][b];
    }
  }
  return table;
}

constexpr VowelTable kTable = make_table();

// Coefficients are rebuilt every kSubBlock samples.
constexpr int kSubBlock = 16;
// Pre-gain at drive = 1, in dB.
constexpr float kDriveMaxDb = 36.0f;
constexpr float kDbPerAmplitudeDecade = 20.0f;

float clamp_finite(float value, float lo, float hi, float fallback) noexcept {
  return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

}  // namespace

float vowel_table_hz(int vowel, int band) noexcept {
  return kTable.hz[std::clamp(vowel, 0, kVowelCount - 1)][std::clamp(band, 0, kVowelBandCount - 1)];
}

VowelFilter::VowelFilter(VowelFilterConfig config) : config_(config) {
  const VowelFilterConfig defaults;
  config_.vowel =
      clamp_finite(config_.vowel, 0.0f, static_cast<float>(kVowelCount - 1), defaults.vowel);
  config_.accel_ms = clamp_finite(config_.accel_ms, 0.0f, 1.0e6f, defaults.accel_ms);
  config_.drive = clamp_finite(config_.drive, 0.0f, 1.0f, defaults.drive);
  config_.dry_wet = clamp_finite(config_.dry_wet, 0.0f, 1.0f, defaults.dry_wet);
  drive_gain_ = std::pow(10.0f, config_.drive * kDriveMaxDb / kDbPerAmplitudeDecade);
  paths_.set_path_latencies_q8({0, rt::kAdaa1LatencySamplesQ8});
  paths_.ensure_channels(kPlanes);
}

void VowelFilter::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  for (auto& plane : bands_) {
    for (auto& band : plane) band.prepare(sample_rate_);
  }
  update_glide_coefficient();
  reset();
}

int VowelFilter::tail_samples() const noexcept {
  if (!(std::clamp(config_.dry_wet, 0.0f, 1.0f) > 0.0f)) return 0;
  // The bank glides between table vowels, so take each band's longest ring over the table; the
  // bands run in parallel after the saturator's one remembered sample.
  const float amp = std::pow(10.0f, kFitPeakingDb / (2.0f * kDbPerAmplitudeDecade));
  rt::TailBudget bank;
  for (int vowel = 0; vowel < kVowelCount; ++vowel) {
    for (const Triple& band : target_for(static_cast<float>(vowel))) {
      bank.alongside(modulation::SvfBandpass::ring(std::exp(band.log_hz),
                                                   amp * std::exp(band.log_q), sample_rate_));
    }
  }
  rt::TailBudget tail;
  tail.delay(1.0).then(bank);
  return tail.samples();
}

void VowelFilter::reset() {
  for (auto& plane : bands_) {
    for (auto& band : plane) band.reset();
  }
  for (auto& adaa : adaa_) adaa.reset();
  adaa_primed_.fill(false);
  previous_in_.fill(0.0f);
  paths_.reset();
  snap_ = true;
  countdown_ = 0;
}

void VowelFilter::update_glide_coefficient() {
  const double tau_s = static_cast<double>(config_.accel_ms) * 1.0e-3;
  glide_ =
      tau_s > 0.0 ? static_cast<float>(1.0 - std::exp(-kSubBlock / (tau_s * sample_rate_))) : 1.0f;
}

std::array<VowelFilter::Triple, kVowelBandCount> VowelFilter::target_for(
    float vowel) const noexcept {
  const int lower = std::min(static_cast<int>(vowel), kVowelCount - 2);
  const float t = vowel - static_cast<float>(lower);
  std::array<Triple, kVowelBandCount> out{};
  for (int b = 0; b < kVowelBandCount; ++b) {
    // q and the weights are shared by every vowel; only the frequency moves.
    const float lo = std::log(kTable.hz[lower][b]);
    const float hi = std::log(kTable.hz[lower + 1][b]);
    out[static_cast<size_t>(b)] = {lo + (hi - lo) * t, std::log(kFitQ[static_cast<size_t>(b)]),
                                   kFitWeight[static_cast<size_t>(b)]};
  }
  return out;
}

void VowelFilter::update_bank() {
  const auto target = target_for(config_.vowel);
  const float amp = std::pow(10.0f, kFitPeakingDb / (2.0f * kDbPerAmplitudeDecade));
  const float lift = amp * amp - 1.0f;
  const float constant = std::pow(10.0f, kFitConstantDb / kDbPerAmplitudeDecade);
  float weight_sum = kFitDirectWeight;
  for (int b = 0; b < kVowelBandCount; ++b) {
    const auto i = static_cast<size_t>(b);
    if (snap_) {
      current_[i] = target[i];
    } else {
      // Frequency and q glide in the log domain; the signed weight, which has no
      // log, glides linearly.
      current_[i].log_hz += glide_ * (target[i].log_hz - current_[i].log_hz);
      current_[i].log_q += glide_ * (target[i].log_q - current_[i].log_q);
      current_[i].weight += glide_ * (target[i].weight - current_[i].weight);
    }
    weight_sum += current_[i].weight;
    band_gain_[i] = constant * lift * current_[i].weight;
    const float hz = std::exp(current_[i].log_hz);
    const float q = amp * std::exp(current_[i].log_q);
    for (auto& plane : bands_) plane[i].set(hz, q);
  }
  direct_gain_ = constant * weight_sum;
  snap_ = false;
}

void VowelFilter::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const bool drive_on = config_.drive_on;
  const float post = 1.0f / std::tanh(drive_gain_);
  const int active = std::min(num_channels, kPlanes);
  for (int i = 0; i < num_samples; ++i) {
    if (countdown_ == 0) {
      update_bank();
      countdown_ = kSubBlock;
    }
    --countdown_;
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const auto p = static_cast<size_t>(ch);
      const float in = channels[ch][i];
      float u = 0.0f;
      if (drive_on) {
        // Full scale in stays full scale out; the antialiased tanh ahead of the bank.
        // Primed on the previous input so engaging keeps the half-sample delay.
        if (!adaa_primed_[p]) {
          adaa_[p].reset(drive_gain_ * previous_in_[p]);
          adaa_primed_[p] = true;
        }
        u = post * adaa_[p].process(drive_gain_ * in);
      } else {
        // ADAA1's average without the tanh, so the wet delay is the same.
        u = 0.5f * (in + previous_in_[p]);
        adaa_primed_[p] = false;
      }
      previous_in_[p] = in;
      float y = direct_gain_ * u;
      for (int b = 0; b < kVowelBandCount; ++b) {
        y += band_gain_[static_cast<size_t>(b)] * bands_[p][static_cast<size_t>(b)].tick(u);
      }
      channels[ch][i] = dry * paths_.align(0, p, in) + wet * paths_.align(1, p, y);
    }
  }
  bool discarded = false;
  for (auto& plane : bands_) {
    for (auto& band : plane) discarded |= band.discard_non_finite();
  }
  if (discarded) note_non_finite_discard();
}

bool VowelFilter::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.vowel = std::clamp(value, 0.0f, static_cast<float>(kVowelCount - 1));
      return true;
    case 1:
      config_.accel_ms = std::clamp(value, 0.0f, 1.0e6f);
      update_glide_coefficient();
      return true;
    case 2:
      config_.drive = std::clamp(value, 0.0f, 1.0f);
      drive_gain_ = std::pow(10.0f, config_.drive * kDriveMaxDb / kDbPerAmplitudeDecade);
      return true;
    case 3:
      if (!mastering::common::valid_switch_value(value)) return false;
      config_.drive_on = value == 1.0f;
      return true;
    case 4:
      config_.dry_wet = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> VowelFilter::parameter_descriptors() const {
  return {{"vowel", 0}, {"accelMs", 1}, {"drive", 2}, {"driveOn", 3}, {"dryWet", 4}};
}

}  // namespace sonare::effects::filter
