#include "editing/voice_changer/streaming_retune.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
// Argument validation in prepare() throws SonareException; process_block
// is noexcept by contract and must not throw — see below.

namespace sonare::editing::voice_changer {

using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kSpectrumEpsilon;
using sonare::constants::kTwoPi;

namespace {
constexpr double kDefaultGrainSeconds = 2048.0 / 44100.0;
constexpr float kMaxSemitones = 24.0f;  // Clamp shift range to +/- 2 octaves.
constexpr int kMaxGrainSize = 8192;
// Source delay the coherent read may drift through before it is re-anchored: the
// longest a transient can be re-read late, against how often a jump is audible.
constexpr int kWrapRoomGrains = 2;

StreamingRetuneConfig sanitize_config(StreamingRetuneConfig config) noexcept {
  config.semitones = std::isfinite(config.semitones)
                         ? std::clamp(config.semitones, -kMaxSemitones, kMaxSemitones)
                         : 0.0f;
  config.mix = std::isfinite(config.mix) ? std::clamp(config.mix, 0.0f, 1.0f) : 1.0f;
  config.grain_size = std::clamp(config.grain_size, 0, kMaxGrainSize);
  return config;
}

int resolve_grain_size(int requested, double sample_rate) noexcept {
  const int derived =
      requested > 0 ? requested : static_cast<int>(std::lround(sample_rate * kDefaultGrainSeconds));
  const int grain = std::max(4, derived);
  return grain + ((4 - grain % 4) % 4);
}
}  // namespace

StreamingRetune::StreamingRetune(StreamingRetuneConfig config) { set_config(config); }

void StreamingRetune::update_ratio() noexcept {
  const float semis = std::clamp(semitones_smoother_.current(), -kMaxSemitones, kMaxSemitones);
  pitch_ratio_ = std::pow(2.0, static_cast<double>(semis) / kSemitonesPerOctave);
}

void StreamingRetune::prepare(double sample_rate, int max_block_size) {
  // Finite, not merely positive: the default grain size rounds sample_rate to an int, and
  // std::lround of an infinity is undefined before std::max ever narrows the result.
  if (!numeric::finite_positive(sample_rate)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be finite and positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;

  grain_size_ = resolve_grain_size(requested_grain_size_, sample_rate_);
  hop_a_ = grain_size_ / 4;
  wrap_room_ = kWrapRoomGrains * grain_size_;
  // The ring holds the widest grain source span, the whole wrap room behind it, and the
  // hop of source the alignment search reads ahead of the previous grain's anchor.
  const int max_span = static_cast<int>(
      std::ceil(static_cast<double>(grain_size_) * std::exp2(kMaxSemitones / kSemitonesPerOctave)));
  ring_cap_ = static_cast<std::size_t>(max_span + wrap_room_ + 2 * hop_a_);
  accum_cap_ = static_cast<std::size_t>(2 * grain_size_);

  // Precompute periodic Hann window.
  window_.assign(static_cast<std::size_t>(grain_size_), 0.0f);
  for (int n = 0; n < grain_size_; ++n) {
    const float phase = kTwoPi * static_cast<float>(n) / static_cast<float>(grain_size_);
    window_[static_cast<std::size_t>(n)] = 0.5f * (1.0f - std::cos(phase));
  }

  ring_buf_.assign(ring_cap_, 0.0f);
  synth_acc_.assign(accum_cap_, 0.0f);
  norm_acc_.assign(accum_cap_, 0.0f);
  dry_delay_.assign(static_cast<std::size_t>(latency_samples()), 0.0f);

  semitones_smoother_.prepare(sample_rate_, 12.0f);
  mix_smoother_.prepare(sample_rate_, 10.0f);
  semitones_smoother_.set_target(config_.semitones);
  mix_smoother_.set_target(config_.mix);
  update_ratio();
  reset();
}

void StreamingRetune::reset() {
  std::fill(ring_buf_.begin(), ring_buf_.end(), 0.0f);
  std::fill(synth_acc_.begin(), synth_acc_.end(), 0.0f);
  std::fill(norm_acc_.begin(), norm_acc_.end(), 0.0f);
  write_head_ = 0;
  input_phase_ = 0;
  anchor_delay_ = 0.0;
  drain_pos_ = 0;
  // Place the first grain one hop ahead of the drain tap. A slot is written by
  // every grain that overlaps it, and the last of those is only emitted one hop
  // after the slot would otherwise be read; draining a slot early would divide
  // a partial sum of grains by a partial window sum and modulate the output at
  // the hop rate. The lag costs exactly one hop of latency, which is why
  // latency_samples() reports a full grain.
  synth_pos_ = static_cast<std::size_t>(hop_a_);
  std::fill(dry_delay_.begin(), dry_delay_.end(), 0.0f);
  dry_delay_pos_ = 0;
  // reset is an explicit state boundary, unlike a live set_config update.
  semitones_smoother_.reset(config_.semitones);
  mix_smoother_.reset(config_.mix);
  update_ratio();
}

void StreamingRetune::set_config(const StreamingRetuneConfig& config) {
  config_ = sanitize_config(config);
  // Record the request BEFORE the effective-value overwrite below lands on the
  // same field, so the next prepare() resolves what the caller asked for.
  requested_grain_size_ = config_.grain_size;
  // grain_size is a structural parameter fixed at prepare() time: changing it
  // would reallocate the grain/ring buffers, which is forbidden here because
  // set_config runs on the audio thread via RealtimeVoiceChanger snapshot
  // adoption. Once prepared, report the effective grain size so config() never
  // advertises a size that is not actually in use; a new grain_size takes
  // effect on the next prepare().
  if (grain_size_ > 0) {
    config_.grain_size = grain_size_;
  }
  semitones_smoother_.set_target(config_.semitones);
  mix_smoother_.set_target(config_.mix);
  // Before prepare() no audio thread is consuming the values, so preserve the
  // existing immediate ratio initialization. Once prepared, process_block()
  // advances the smoother sample-by-sample; emit_grain() derives the ratio
  // only when it starts a new grain.
  if (sample_rate_ <= 0.0) update_ratio();
}

float StreamingRetune::read_ring_linear(double position) const noexcept {
  // Wrap fractional position into [0, ring_cap_) and linearly interpolate.
  const double cap = static_cast<double>(ring_cap_);
  double pos = std::fmod(position, cap);
  if (pos < 0.0) {
    pos += cap;
  }
  const std::size_t i0 = static_cast<std::size_t>(pos);
  const std::size_t i1 = (i0 + 1) % ring_cap_;
  const float frac = static_cast<float>(pos - static_cast<double>(i0));
  return ring_buf_[i0] * (1.0f - frac) + ring_buf_[i1] * frac;
}

void StreamingRetune::realign_anchor(double previous_end, double source_span) noexcept {
  // The room ends where the history written since reset() ends, so the first grains after a
  // reset re-read the stream's start rather than the silence before it.
  const double room = std::clamp(static_cast<double>(write_head_) - source_span, 0.0,
                                 static_cast<double>(wrap_room_));
  // Candidates fill one hop at the end of the room opposite to the drift, so the next jump is
  // as far off as it can be; the best match to the previous grain's last hop keeps the phase.
  const bool toward_history = anchor_delay_ < 0.0;
  const double lo = toward_history ? std::max(0.0, room - static_cast<double>(hop_a_)) : 0.0;
  const double hi = toward_history ? room : std::min(static_cast<double>(hop_a_), room);
  // Integer jumps keep the anchor's fraction, so the reference and every candidate are read
  // at whole ring indices.
  const std::int64_t ref_begin = static_cast<std::int64_t>(std::floor(previous_end)) - hop_a_;
  const std::int64_t jump_min = static_cast<std::int64_t>(std::ceil(anchor_delay_ - hi));
  const std::int64_t jump_max = static_cast<std::int64_t>(std::floor(anchor_delay_ - lo));
  // Indices run negative while the ring is still filling after reset().
  const auto ring_at = [&](std::int64_t index) noexcept {
    const std::int64_t cap = static_cast<std::int64_t>(ring_cap_);
    return ring_buf_[static_cast<std::size_t>(((index % cap) + cap) % cap)];
  };
  double best_score = -1.0;
  std::int64_t best_jump = toward_history ? jump_min : jump_max;
  // Scan from the far end inward so a silent reference keeps the full room.
  const std::int64_t step = toward_history ? 1 : -1;
  for (std::int64_t jump = best_jump; jump >= jump_min && jump <= jump_max; jump += step) {
    double dot = 0.0;
    double energy = 0.0;
    for (int m = 0; m < hop_a_; ++m) {
      const double ref = ring_at(ref_begin + m);
      const double cand = ring_at(ref_begin + m + jump);
      dot += ref * cand;
      energy += cand * cand;
    }
    const double score = dot / std::sqrt(energy + static_cast<double>(kSpectrumEpsilon));
    if (score > best_score) {
      best_score = score;
      best_jump = jump;
    }
  }
  // Only a room narrower than one sample leaves no integer jump; it then collapses to its end.
  anchor_delay_ = std::clamp(anchor_delay_ - static_cast<double>(best_jump), lo, hi);
}

void StreamingRetune::emit_grain() noexcept {
  // The semitone smoother still advances at every sample, but the expensive
  // exponential is only needed when a new grain captures its ratio. Keeping
  // the ratio fixed for the complete grain also makes the cost independent of
  // the caller's block partition.
  update_ratio();
  // Grain-resampling pitch shift: a grain of grain_size output samples is
  // sourced from grain_size * pitch_ratio input samples via linear
  // interpolation. Grains are overlap-added at hop_a on both the analysis and
  // synthesis sides; the per-grain resampling is what shifts pitch. For
  // pitch_ratio > 1 (positive semitones) the read advances faster than one
  // sample per output sample, packing more signal cycles into the fixed grain
  // length and raising the pitch.
  const double source_span = static_cast<double>(grain_size_) * pitch_ratio_;
  const double previous_end =
      static_cast<double>(write_head_) - static_cast<double>(hop_a_) - anchor_delay_;
  // Coherent with the previous grain: its source end moved by hop * ratio while the head moved
  // by one hop. Snapping to the head instead re-locks the overlap-add to the input period.
  anchor_delay_ += static_cast<double>(hop_a_) * (1.0 - pitch_ratio_);
  if (anchor_delay_ < 0.0 || anchor_delay_ > static_cast<double>(wrap_room_)) {
    realign_anchor(previous_end, source_span);
  }
  const double start = static_cast<double>(write_head_) - anchor_delay_ - source_span;

  for (int n = 0; n < grain_size_; ++n) {
    const double read_pos = start + static_cast<double>(n) * pitch_ratio_;
    const float w = window_[static_cast<std::size_t>(n)];
    const float sample = read_ring_linear(read_pos) * w;
    const std::size_t idx = (synth_pos_ + static_cast<std::size_t>(n)) % accum_cap_;
    synth_acc_[idx] += sample;
    // The window is applied once (analysis only), so the drain divides by the
    // sum of the window values that actually contributed, not their squares:
    // sum(x*w)/sum(w) is a weighted average of the overlapping grains and is
    // unity for identical grains. Accumulating w*w would be the correct
    // normalization only for a doubly-windowed (analysis + synthesis) overlap
    // add, and against a single windowing it is a fixed 2/1.5 = +2.5 dB gain.
    norm_acc_[idx] += w;
  }

  synth_pos_ = (synth_pos_ + static_cast<std::size_t>(hop_a_)) % accum_cap_;
}

void StreamingRetune::process_block(const float* input, float* output, int num_samples) noexcept {
  // Pre-condition violations are silent no-ops to keep this RT-safe (no throw,
  // no allocation). Callers that need diagnostics must validate at prepare()
  // time, which is the only place where exceptions are appropriate. Throwing
  // here would terminate the audio thread via std::terminate because the
  // immediate caller (RealtimeVoiceChanger::process_block) is noexcept.
  if (num_samples <= 0) return;
  if (input == nullptr || output == nullptr) return;
  if (sample_rate_ <= 0.0) {
    // prepare() not called yet. Pass through rather than producing garbage
    // from an uninitialized ring/window state. Mirrors StreamingFormant's
    // behaviour for symmetry. This must precede the max_block_size_ check
    // because the default max_block_size_ is 0, which would otherwise treat
    // every non-empty block as oversized and silently drop the passthrough.
    if (input != output) std::copy_n(input, num_samples, output);
    return;
  }
  if (num_samples > max_block_size_) return;

  for (int i = 0; i < num_samples; ++i) {
    semitones_smoother_.process();
    const float mix = mix_smoother_.process();
    // 1) Write incoming sample into the history ring.
    ring_buf_[write_head_ % ring_cap_] = input[i];
    ++write_head_;

    // 2) Emit a grain every hop_a samples.
    if (++input_phase_ >= hop_a_) {
      input_phase_ = 0;
      emit_grain();
    }

    // 3) Drain exactly one output sample from the front of the OLA accumulator.
    // The drain tap trails the newest grain by a full grain length, so every
    // grain overlapping this slot has already been accumulated (see reset()).
    // The epsilon only guards the pre-roll slots, where no grain has landed
    // yet and both sums are zero.
    const float norm = norm_acc_[drain_pos_] + kSpectrumEpsilon;
    const float out_sample = synth_acc_[drain_pos_] / norm;
    synth_acc_[drain_pos_] = 0.0f;
    norm_acc_[drain_pos_] = 0.0f;
    drain_pos_ = (drain_pos_ + 1) % accum_cap_;

    // The OLA output trails the input by one grain. Delay the dry path by that
    // same fixed amount before blending so intermediate mix values are an
    // actual level blend rather than a slapback echo.
    float delayed_dry = input[i];
    if (!dry_delay_.empty()) {
      delayed_dry = dry_delay_[dry_delay_pos_];
      dry_delay_[dry_delay_pos_] = input[i];
      dry_delay_pos_ = (dry_delay_pos_ + 1) % dry_delay_.size();
    }
    output[i] = delayed_dry * (1.0f - mix) + out_sample * mix;
  }
}

}  // namespace sonare::editing::voice_changer
