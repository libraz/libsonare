#pragma once

/// @file psola_shifter.h
/// @brief Low-latency TD-PSOLA pitch shifter producing one output per voice.
///
/// Analysis marks are shared by every voice: they follow the pushed f0 estimates period by period
/// and snap to the largest local maximum of a low-passed copy within +-0.25 period. Each voice
/// places synthesis marks at its own output period and overlap-adds two-period Hann grains, read
/// at rate `formant_alpha` through a windowed-sinc table, from the analysis mark nearest the input
/// time. The input period is the marks' own spacing, so at ratio 1 every grain lands on a mark and
/// the output is the input delayed exactly. A second, always-scheduled path overlap-adds fixed
/// 5 ms grains with no pitch change for unvoiced input; the two are crossfaded over 5 ms on the
/// voicing of the analysis time. Output sample t synthesizes around input time
/// t - latency_samples(), and every grain reads only input that has arrived by the sample it
/// writes, so the result does not depend on the block partition beyond target interpolation.

#include <cstdint>
#include <vector>

#include "editing/voice_changer/voice_transform_types.h"
#include "rt/biquad_design.h"

namespace sonare::editing::voice_changer {

/// Latency is ceil(sr / f0_min); targets are interpolated linearly across each block.
class PsolaShifter {
 public:
  void prepare(double sr, int max_block, float f0_min_hz);
  void reset();
  void push_estimates(const F0Estimate* e, int count) noexcept;
  /// @param voice_out `targets.count` output buffers of `n` samples.
  void process(const float* in, const VoiceTargets& targets, float* const* voice_out,
               int n) noexcept;
  /// @return ceil(sr / f0_min).
  int latency_samples() const noexcept;

 private:
  /// One overlap-added grain: output and input centres, read rate, the half widths before and
  /// after its centre in output samples, amplitude normalisation and its table.
  struct Grain {
    double out_centre = 0.0;
    double in_centre = 0.0;
    double alpha = 1.0;
    double half_left = 0.0;
    double half_right = 0.0;
    float gain = 0.0f;
    int table = 0;
  };

  /// Analysis mark: input position and the period it propagates with.
  struct Mark {
    double pos = 0.0;
    double period = 0.0;
  };

  struct Estimate {
    std::int64_t centre = 0;
    double period = 0.0;
    bool voiced = false;
  };

  static constexpr int kMaxGrains = 16;
  static constexpr int kMaxUnvoicedGrains = 4;

  struct Voice {
    bool active = false;
    VoiceTarget previous;
    bool voiced_running = false;
    bool joining = false;       ///< The next voiced grain may already be under way.
    double voiced_next = 0.0;   ///< Expected output position of the next synthesis mark.
    double previous_out = 0.0;  ///< Output position of the last synthesis mark.
    double previous_aim = 0.0;  ///< Input time it was synthesized from.
    bool has_previous = false;  ///< The next mark follows from the previous one.
    double last_period_out = 0.0;
    Grain voiced[kMaxGrains];
    int voiced_count = 0;
    double unvoiced_next = 0.0;
    Grain unvoiced[kMaxUnvoicedGrains];
    int unvoiced_count = 0;
  };

  /// Per-sample interpolated target of one voice.
  struct SampleTarget {
    double ratio;
    double target_hz;
    double weight;
    double alpha;
  };

  void process_voice_sample(Voice& v, const SampleTarget& target, std::int64_t t, float g,
                            bool newest_voiced, float* out) noexcept;
  void activate_voice(Voice& v, const VoiceTarget& target, std::int64_t t) noexcept;
  bool start_voiced(Voice& v, const SampleTarget& target, std::int64_t t) noexcept;
  void schedule_voiced(Voice& v, const SampleTarget& target, std::int64_t t) noexcept;
  bool start_chain(double alpha, std::int64_t t) noexcept;
  bool extend_chain(std::int64_t t) noexcept;
  void extend_chain_to(double target, std::int64_t t) noexcept;
  /// Latest mark at or before `target` and earliest after it, both no later than `latest`.
  void bracket(double target, double latest, const Mark** before,
               const Mark** after) const noexcept;
  const Estimate* estimate_at(double pos) const noexcept;
  double peak_near(double lo, double hi, std::int64_t t, bool* found) const noexcept;
  /// Input period at `aim`: the spacing of the marks around it, else the estimate's.
  double input_period(double aim) const noexcept;
  double output_period(double input_period, const SampleTarget& target) const noexcept;
  int table_for(double alpha) const noexcept;
  float read(double pos, int table) const noexcept;
  float grain_sample(const Grain& grain, std::int64_t t) const noexcept;
  const Mark& mark_at(int age) const noexcept;

  double sr_ = 0.0;
  int latency_ = 0;
  int unvoiced_spacing_ = 0;
  int crossfade_samples_ = 1;
  double min_period_ = 1.0;

  // Input and its low-passed copy; slot i & ring_mask_ holds input sample i.
  std::vector<float> ring_;
  std::vector<float> lowpassed_;
  std::int64_t ring_mask_ = 0;
  rt::BiquadState lowpass_[2];

  // Fractional-delay tables: table q, phase p, tap k at ((q * (kPhases + 1)) + p) * kTaps + k.
  std::vector<float> tables_;
  int table_count_ = 0;

  std::vector<Estimate> estimates_;
  int estimate_head_ = 0;  ///< Slot of the next push.
  int estimate_count_ = 0;
  std::int64_t run_start_ = 0;  ///< Centre of the first voiced estimate of the newest voiced run.

  std::vector<Mark> marks_;
  int mark_head_ = 0;
  int mark_count_ = 0;

  Voice voices_[kMaxVoices];
  float crossfade_ = 0.0f;
  std::int64_t sample_count_ = 0;
};

}  // namespace sonare::editing::voice_changer
