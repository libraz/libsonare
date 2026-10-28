#include "editing/voice_changer/psola_shifter.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rt/polyphase_fir.h"
#include "util/constants.h"

namespace sonare::editing::voice_changer {

using constants::kButterworth4QHigh;
using constants::kButterworth4QLow;
using constants::kPiD;

namespace {

// Analysis marks snap to the largest low-passed local maximum within this fraction of a period.
constexpr double kPeakSearchFraction = 0.25;
constexpr float kMarkLowpassHz = 1000.0f;
// Unvoiced input: fixed marks and grains with no pitch change; voicing switches crossfade.
constexpr double kUnvoicedSpacingS = 0.005;
constexpr double kCrossfadeS = 0.005;
// Fractional-delay read: 16 taps at offsets -7..+8 of floor(pos), Kaiser-windowed sinc.
constexpr int kTaps = 16;
constexpr int kTapsBefore = 7;
constexpr double kKernelHalfWidth = 8.0;
constexpr int kPhases = 128;
constexpr double kKaiserBeta = 8.0;
// Read positions stay this many samples behind the newest input, so every tap has arrived.
constexpr double kReadMargin = kKernelHalfWidth + 1.0;
// Anti-alias tables for alpha > 1: cutoff fs / (2 alpha) with alpha quantised to 1/16 octave.
constexpr int kTablesPerOctave = 16;
constexpr int kTableOctaves = 3;
constexpr double kMinAlpha = 0.125;
constexpr double kMaxAlpha = 8.0;
// Highest input f0 an estimate may carry; lower ones are clamped to f0_min.
constexpr double kMaxInputF0Hz = 1600.0;
// Shortest output period in samples, and the widest grain in output periods.
constexpr double kMinOutputPeriod = 4.0;
constexpr double kMaxOverlap = 6.0;
constexpr float kMaxGrainGain = 8.0f;
// Input times this close to a mark are read as on it, so rounding cannot pick its neighbour.
constexpr double kMarkTolerance = 1e-6;
constexpr int kEstimateCapacity = 128;
constexpr int kMarkCapacity = 128;

double finite_positive_or(double x, double fallback) { return x > 0.0 && x < 1e30 ? x : fallback; }

double clamp_alpha(double alpha) {
  return std::clamp(finite_positive_or(alpha, 1.0), kMinAlpha, kMaxAlpha);
}

double sinc(double x) {
  if (std::abs(x) < 1e-12) return 1.0;
  return std::sin(kPiD * x) / (kPiD * x);
}

}  // namespace

// Processing is sample-major with no block scratch, so the block size needs no reservation.
void PsolaShifter::prepare(double sr, int /*max_block*/, float f0_min_hz) {
  sr_ = sr;
  latency_ = static_cast<int>(std::ceil(sr / static_cast<double>(f0_min_hz)));
  unvoiced_spacing_ = std::max(1, static_cast<int>(std::lround(kUnvoicedSpacingS * sr)));
  crossfade_samples_ = std::max(1, static_cast<int>(std::lround(kCrossfadeS * sr)));
  min_period_ = sr / kMaxInputF0Hz;

  // History reaches back past a grain read at the lowest alpha from the oldest selectable mark.
  const std::int64_t needed = 8 * static_cast<std::int64_t>(latency_) + 4 * unvoiced_spacing_ + 64;
  std::int64_t size = 1;
  while (size < needed) size <<= 1;
  ring_.assign(static_cast<std::size_t>(size), 0.0f);
  lowpassed_.assign(static_cast<std::size_t>(size), 0.0f);
  ring_mask_ = size - 1;

  const float corner = std::min(kMarkLowpassHz, kMaxDesignFrequencyRatio * static_cast<float>(sr));
  const float w0 = rt::frequency_to_w0(corner, sr);
  lowpass_[0].set(rt::rbj_lowpass(w0, kButterworth4QLow));
  lowpass_[1].set(rt::rbj_lowpass(w0, kButterworth4QHigh));

  table_count_ = kTablesPerOctave * kTableOctaves + 1;
  tables_.assign(static_cast<std::size_t>(table_count_) * (kPhases + 1) * kTaps, 0.0f);
  const double i0_beta = rt::modified_bessel_i0(kKaiserBeta);
  for (int q = 0; q < table_count_; ++q) {
    const double cutoff = 0.5 / std::exp2(static_cast<double>(q) / kTablesPerOctave);
    for (int p = 0; p <= kPhases; ++p) {
      const double frac = static_cast<double>(p) / kPhases;
      float* row = &tables_[(static_cast<std::size_t>(q) * (kPhases + 1) + p) * kTaps];
      double sum = 0.0;
      double h[kTaps];
      for (int k = 0; k < kTaps; ++k) {
        const double d = static_cast<double>(k - kTapsBefore) - frac;
        const double r = d / kKernelHalfWidth;
        const double window =
            std::abs(r) >= 1.0
                ? 0.0
                : rt::modified_bessel_i0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0_beta;
        h[k] = 2.0 * cutoff * sinc(2.0 * cutoff * d) * window;
        sum += h[k];
      }
      for (int k = 0; k < kTaps; ++k) row[k] = static_cast<float>(h[k] / sum);
    }
  }

  estimates_.assign(kEstimateCapacity, Estimate{});
  marks_.assign(kMarkCapacity, Mark{});
  reset();
}

void PsolaShifter::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  std::fill(lowpassed_.begin(), lowpassed_.end(), 0.0f);
  for (rt::BiquadState& s : lowpass_) s.reset();
  estimate_head_ = 0;
  estimate_count_ = 0;
  run_start_ = 0;
  mark_head_ = 0;
  mark_count_ = 0;
  for (Voice& v : voices_) v = Voice{};
  crossfade_ = 0.0f;
  sample_count_ = 0;
}

int PsolaShifter::latency_samples() const noexcept { return latency_; }

void PsolaShifter::push_estimates(const F0Estimate* e, int count) noexcept {
  for (int i = 0; i < count; ++i) {
    const F0Estimate& in = e[i];
    const Estimate* newest =
        estimate_count_ > 0 ? &estimates_[static_cast<std::size_t>(
                                  (estimate_head_ + kEstimateCapacity - 1) % kEstimateCapacity)]
                            : nullptr;
    // Lookups assume ascending centres.
    if (newest != nullptr && in.centre_sample <= newest->centre) continue;
    const double f0 = static_cast<double>(in.f0_hz);
    const bool voiced = in.voiced && f0 > 0.0 && f0 < 1e30;
    Estimate out;
    out.centre = in.centre_sample;
    out.voiced = voiced;
    out.period = voiced ? std::clamp(sr_ / f0, min_period_, static_cast<double>(latency_))
                        : static_cast<double>(latency_);
    if (voiced && (newest == nullptr || !newest->voiced)) run_start_ = out.centre;
    estimates_[static_cast<std::size_t>(estimate_head_)] = out;
    estimate_head_ = (estimate_head_ + 1) % kEstimateCapacity;
    estimate_count_ = std::min(estimate_count_ + 1, kEstimateCapacity);
  }
}

const PsolaShifter::Estimate* PsolaShifter::estimate_at(double pos) const noexcept {
  for (int i = 0; i < estimate_count_; ++i) {
    const int slot = (estimate_head_ + kEstimateCapacity - 1 - i) % kEstimateCapacity;
    const Estimate& e = estimates_[static_cast<std::size_t>(slot)];
    if (static_cast<double>(e.centre) <= pos) return &e;
  }
  return nullptr;
}

const PsolaShifter::Mark& PsolaShifter::mark_at(int age) const noexcept {
  return marks_[static_cast<std::size_t>((mark_head_ + kMarkCapacity - 1 - age) % kMarkCapacity)];
}

int PsolaShifter::table_for(double alpha) const noexcept {
  if (alpha <= 1.0) return 0;
  const int q = static_cast<int>(std::ceil(kTablesPerOctave * std::log2(alpha) - 1e-6));
  return std::clamp(q, 0, table_count_ - 1);
}

float PsolaShifter::read(double pos, int table) const noexcept {
  const double base = std::floor(pos);
  const double phase = (pos - base) * kPhases;
  const int p = std::min(static_cast<int>(phase), kPhases - 1);
  const float lambda = static_cast<float>(phase - p);
  const float* row0 = &tables_[(static_cast<std::size_t>(table) * (kPhases + 1) + p) * kTaps];
  const float* row1 = row0 + kTaps;
  const std::int64_t first = static_cast<std::int64_t>(base) - kTapsBefore;
  float acc = 0.0f;
  for (int k = 0; k < kTaps; ++k) {
    const float c = row0[k] + lambda * (row1[k] - row0[k]);
    acc += c * ring_[static_cast<std::size_t>((first + k) & ring_mask_)];
  }
  return acc;
}

float PsolaShifter::grain_sample(const Grain& grain, std::int64_t t) const noexcept {
  const double v = static_cast<double>(t) - grain.out_centre;
  const double half = v < 0.0 ? grain.half_left : grain.half_right;
  if (std::abs(v) >= half) return 0.0f;
  // The Hann window is applied in output time, after resampling.
  const double w = 0.5 + 0.5 * std::cos(kPiD * v / half);
  return grain.gain * static_cast<float>(w) * read(grain.in_centre + grain.alpha * v, grain.table);
}

double PsolaShifter::peak_near(double lo, double hi, std::int64_t t, bool* found) const noexcept {
  *found = false;
  const auto first = static_cast<std::int64_t>(std::ceil(lo));
  const std::int64_t last = std::min(static_cast<std::int64_t>(std::floor(hi)), t - 1);
  const auto at = [this](std::int64_t i) {
    return lowpassed_[static_cast<std::size_t>(i & ring_mask_)];
  };
  std::int64_t best = 0;
  float best_value = 0.0f;
  for (std::int64_t i = first; i <= last; ++i) {
    const float y = at(i);
    if (y >= at(i - 1) && y > at(i + 1) && (!*found || y > best_value)) {
      best = i;
      best_value = y;
      *found = true;
    }
  }
  if (!*found) return 0.0;
  const double l = at(best - 1);
  const double c = at(best);
  const double r = at(best + 1);
  const double curvature = l - 2.0 * c + r;
  const double shift = curvature < 0.0 ? std::clamp(0.5 * (l - r) / curvature, -0.5, 0.5) : 0.0;
  return static_cast<double>(best) + shift;
}

bool PsolaShifter::start_chain(double alpha, std::int64_t t) noexcept {
  const Estimate* newest = estimate_at(static_cast<double>(t));
  if (newest == nullptr || !newest->voiced) return false;
  const double guess = newest->period;
  // Anchor at the oldest input a grain alive now reads, or at the voicing onset if later.
  const double from_now = static_cast<double>(t) - latency_ - guess / alpha;
  const double begin = std::max(static_cast<double>(run_start_), from_now);
  // A search anchored on the onset waits for one whole period of input after it.
  if (begin > from_now && static_cast<double>(t) - 1.0 < begin + guess) return false;
  const Estimate* e = estimate_at(begin);
  if (e == nullptr || !e->voiced) return false;
  bool found = false;
  const double pos = peak_near(begin, begin + e->period, t, &found);
  if (!found) return false;
  // One mark is backfilled a period earlier so the grain overlapping the anchor's is complete.
  mark_count_ = 0;
  for (const Mark m : {Mark{pos - e->period, e->period}, Mark{pos, e->period}}) {
    marks_[static_cast<std::size_t>(mark_head_)] = m;
    mark_head_ = (mark_head_ + 1) % kMarkCapacity;
    ++mark_count_;
  }
  return true;
}

bool PsolaShifter::extend_chain(std::int64_t t) noexcept {
  if (mark_count_ == 0) return false;
  const Mark last = mark_at(0);
  const Estimate* e = estimate_at(last.pos + last.period);
  const bool voiced = e != nullptr && e->voiced;
  // Past the voiced estimates the chain runs on with the held period and no peak search.
  const double period = voiced ? e->period : last.period;
  const double predicted = last.pos + 0.5 * (last.period + period);
  if (predicted > static_cast<double>(t) - 1.0) return false;
  double pos = predicted;
  if (voiced) {
    bool found = false;
    const double reach = kPeakSearchFraction * period;
    const double peak = peak_near(predicted - reach, predicted + reach, t, &found);
    if (found) pos = peak;
  }
  marks_[static_cast<std::size_t>(mark_head_)] = Mark{pos, period};
  mark_head_ = (mark_head_ + 1) % kMarkCapacity;
  mark_count_ = std::min(mark_count_ + 1, kMarkCapacity);
  return true;
}

void PsolaShifter::extend_chain_to(double target, std::int64_t t) noexcept {
  while (mark_count_ > 0 && mark_at(0).pos < target) {
    if (!extend_chain(t)) break;
  }
}

void PsolaShifter::bracket(double target, double latest, const Mark** before,
                           const Mark** after) const noexcept {
  *before = nullptr;
  *after = nullptr;
  for (int age = 0; age < mark_count_; ++age) {
    const Mark& m = mark_at(age);
    if (m.pos > latest) continue;
    if (m.pos <= target + kMarkTolerance) {
      *before = &m;
      return;
    }
    *after = &m;
  }
}

double PsolaShifter::output_period(double input_period, const SampleTarget& target) const noexcept {
  // f_out = (f_m * ratio)^(1 - w) * target_hz^w, f_m the input f0 at the mark.
  const double relative = sr_ / input_period * target.ratio;
  double f_out = relative;
  if (target.weight > 0.0 && target.target_hz > 0.0) {
    f_out = std::exp((1.0 - target.weight) * std::log(relative) +
                     target.weight * std::log(target.target_hz));
  }
  return std::clamp(sr_ / f_out, kMinOutputPeriod, 4.0 * latency_);
}

bool PsolaShifter::start_voiced(Voice& v, const SampleTarget& target, std::int64_t t) noexcept {
  const double alpha = target.alpha;
  const double now = static_cast<double>(t);
  const bool stale =
      mark_count_ == 0 || mark_at(0).pos < now - 2.0 * latency_ - 2.0 * mark_at(0).period;
  if (stale && !start_chain(alpha, t)) return false;
  // The first grain is the earliest one still sounding now, centred exactly on its mark, so at
  // ratio 1 the output reproduces the input from the first voiced sample on.
  const double aim = now - latency_ - mark_at(0).period / alpha;
  extend_chain_to(aim + 1.0, t);
  const Mark* chosen = nullptr;
  for (int age = 0; age < mark_count_; ++age) {
    const Mark& m = mark_at(age);
    if (m.pos <= aim) break;
    chosen = &m;
  }
  if (chosen == nullptr) return false;
  v.voiced_running = true;
  v.joining = true;
  v.has_previous = false;
  v.voiced_count = 0;
  v.voiced_next = chosen->pos + latency_;
  v.last_period_out = output_period(chosen->period, target);
  return true;
}

double PsolaShifter::input_period(double aim) const noexcept {
  const Mark* before = nullptr;
  const Mark* after = nullptr;
  bracket(aim, std::numeric_limits<double>::max(), &before, &after);
  if (before == nullptr) return after != nullptr ? after->period : mark_at(0).period;
  if (after == nullptr) return before->period;
  // The marks' own spacing is the period of the waveform they were snapped to.
  const double spacing = after->pos - before->pos;
  if (spacing >= 0.5 * before->period && spacing <= 2.0 * before->period) return spacing;
  return before->period;
}

void PsolaShifter::schedule_voiced(Voice& v, const SampleTarget& target, std::int64_t t) noexcept {
  const double alpha = target.alpha;
  const double stretch = std::abs(alpha - 1.0);
  const double now = static_cast<double>(t);
  while (mark_count_ > 0) {
    const double predicted_half =
        std::min(mark_at(0).period / alpha, kMaxOverlap * v.last_period_out);
    // Fire a sample early so a grain is placed before its first sample is due.
    if (now + 1.0 < v.voiced_next - predicted_half) break;
    double s = v.voiced_next;
    if (v.has_previous) {
      // The spacing follows the input period around the previous grain, measured on the marks
      // bracketing it now that the later one has arrived; at ratio 1 every grain lands on a mark.
      extend_chain_to(v.previous_aim + 1.0, t);
      s = v.previous_out + output_period(input_period(v.previous_aim), target);
    }
    // A grain due earlier than now narrows to fit, except while joining, when grains already
    // under way are taken whole from the current sample on.
    const double room = v.joining ? std::numeric_limits<double>::max() : s - now;
    const double aim = s - latency_;
    v.previous_out = s;
    v.previous_aim = aim;
    v.has_previous = true;
    v.voiced_next = s + v.last_period_out;
    if (room < 1.0) continue;
    const double latest = s - stretch * std::min(predicted_half, room) - kReadMargin;
    extend_chain_to(aim, t);
    const Mark* before = nullptr;
    const Mark* after = nullptr;
    bracket(aim, latest, &before, &after);
    if (before == nullptr && after == nullptr) continue;
    // The grain is taken from the usable mark nearest the input time.
    const Mark* mark = before;
    if (mark == nullptr || (after != nullptr && after->pos - aim < aim - before->pos)) mark = after;
    // Each half window spans the input period on its side, so at ratio 1 the halves of
    // neighbouring grains complement each other even where the marks are unevenly spaced.
    const double period_left = input_period(aim - 2.0 * kMarkTolerance);
    const double period_right = input_period(aim);
    const double out_left = output_period(period_left, target);
    const double out_right = output_period(period_right, target);
    v.voiced_next = s + out_right;
    v.last_period_out = out_right;
    double left = std::min({period_left / alpha, kMaxOverlap * out_left, room});
    double right = std::min(period_right / alpha, kMaxOverlap * out_right);
    if (stretch > 0.0) {
      const double reach = (s - mark->pos - kReadMargin) / stretch;
      left = std::min(left, reach);
      right = std::min(right, reach);
    }
    if (left < 1.0 || right < 1.0 || s + right <= now || v.voiced_count >= kMaxGrains) continue;
    if (s - left >= now) v.joining = false;
    Grain& g = v.voiced[v.voiced_count++];
    g.out_centre = s;
    g.in_centre = mark->pos;
    g.alpha = alpha;
    g.half_left = left;
    g.half_right = right;
    // Normalised so the overlap-added windows average to one at the output spacing.
    g.gain =
        std::min(static_cast<float>(0.5 * (out_left / left + out_right / right)), kMaxGrainGain);
    g.table = table_for(alpha);
  }
}

void PsolaShifter::activate_voice(Voice& v, const VoiceTarget& target, std::int64_t t) noexcept {
  v = Voice{};
  v.active = true;
  v.previous = target;
  // The unvoiced grid keeps output = input delayed by the latency; join it mid-grain.
  const double spacing = unvoiced_spacing_;
  const double k = std::floor((static_cast<double>(t) - latency_ - spacing) / spacing) + 1.0;
  v.unvoiced_next = k * spacing + latency_;
}

void PsolaShifter::process_voice_sample(Voice& v, const SampleTarget& target, std::int64_t t,
                                        float g, bool newest_voiced, float* out) noexcept {
  const double now = static_cast<double>(t);
  const double spacing = unvoiced_spacing_;
  // Unvoiced grains read at rate alpha within the latency budget.
  const double reach = (latency_ - kReadMargin) / spacing;
  const double alpha_u = std::clamp(target.alpha, std::max(kMinAlpha, 1.0 - reach), 1.0 + reach);
  while (now >= v.unvoiced_next - spacing) {
    if (v.unvoiced_count < kMaxUnvoicedGrains) {
      Grain& gr = v.unvoiced[v.unvoiced_count++];
      gr.out_centre = v.unvoiced_next;
      gr.in_centre = v.unvoiced_next - latency_;
      gr.alpha = alpha_u;
      gr.half_left = spacing;
      gr.half_right = spacing;
      gr.gain = 1.0f;
      gr.table = table_for(alpha_u);
    }
    v.unvoiced_next += spacing;
  }

  if (!v.voiced_running && newest_voiced) start_voiced(v, target, t);
  if (v.voiced_running && g <= 0.0f && !newest_voiced) {
    v.voiced_running = false;
    v.voiced_count = 0;
  }
  if (v.voiced_running) schedule_voiced(v, target, t);

  float voiced = 0.0f;
  for (int i = 0; i < v.voiced_count;) {
    if (g > 0.0f) voiced += grain_sample(v.voiced[i], t);
    if (now + 1.0 >= v.voiced[i].out_centre + v.voiced[i].half_right) {
      v.voiced[i] = v.voiced[--v.voiced_count];
    } else {
      ++i;
    }
  }
  float unvoiced = 0.0f;
  for (int i = 0; i < v.unvoiced_count;) {
    if (g < 1.0f) unvoiced += grain_sample(v.unvoiced[i], t);
    if (now + 1.0 >= v.unvoiced[i].out_centre + v.unvoiced[i].half_right) {
      v.unvoiced[i] = v.unvoiced[--v.unvoiced_count];
    } else {
      ++i;
    }
  }
  *out = g * voiced + (1.0f - g) * unvoiced;
}

void PsolaShifter::process(const float* in, const VoiceTargets& targets, float* const* voice_out,
                           int n) noexcept {
  const int count = std::clamp(targets.count, 0, kMaxVoices);
  for (int i = count; i < kMaxVoices; ++i) voices_[i].active = false;
  for (int i = 0; i < count; ++i) {
    const VoiceTarget& target = targets.voices[i];
    if (!target.active) {
      voices_[i].active = false;
    } else if (!voices_[i].active) {
      activate_voice(voices_[i], target, sample_count_);
    }
  }
  const float step = 1.0f / static_cast<float>(crossfade_samples_);
  for (int k = 0; k < n; ++k) {
    const std::int64_t t = sample_count_ + k;
    const float x = std::isfinite(in[k]) ? in[k] : 0.0f;
    ring_[static_cast<std::size_t>(t & ring_mask_)] = x;
    lowpassed_[static_cast<std::size_t>(t & ring_mask_)] =
        lowpass_[1].process(lowpass_[0].process(x));

    const Estimate* at_analysis = estimate_at(static_cast<double>(t - latency_));
    const bool voiced_now = at_analysis != nullptr && at_analysis->voiced;
    crossfade_ = std::clamp(crossfade_ + (voiced_now ? step : -step), 0.0f, 1.0f);
    const Estimate* newest = estimate_at(static_cast<double>(t));
    const bool newest_voiced = newest != nullptr && newest->voiced;

    const float frac = static_cast<float>(k + 1) / static_cast<float>(n);
    for (int i = 0; i < count; ++i) {
      Voice& v = voices_[i];
      if (!v.active) {
        voice_out[i][k] = 0.0f;
        continue;
      }
      const VoiceTarget& a = v.previous;
      const VoiceTarget& b = targets.voices[i];
      const auto lerp = [frac](float x0, float x1) {
        return static_cast<double>(x0 + frac * (x1 - x0));
      };
      SampleTarget st;
      st.ratio = finite_positive_or(lerp(a.ratio, b.ratio), 1.0);
      st.target_hz = finite_positive_or(lerp(a.target_hz, b.target_hz), 0.0);
      const double weight = lerp(a.absolute_weight, b.absolute_weight);
      st.weight = weight > 0.0 ? std::min(weight, 1.0) : 0.0;
      st.alpha = clamp_alpha(lerp(a.formant_alpha, b.formant_alpha));
      process_voice_sample(v, st, t, crossfade_, newest_voiced, &voice_out[i][k]);
    }
  }
  for (int i = 0; i < count; ++i) {
    if (voices_[i].active) voices_[i].previous = targets.voices[i];
  }
  sample_count_ += n;
}

}  // namespace sonare::editing::voice_changer
