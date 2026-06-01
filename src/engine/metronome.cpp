#include "engine/metronome.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::engine {

using sonare::constants::kPiD;

// Floating-point fuzz for the inclusive PPQ beat-boundary comparison.
constexpr double kPpqEpsilon = 1.0e-9;

void MetronomeEventList::clear() noexcept {
  size = 0;
  overflowed = false;
}

bool MetronomeEventList::add(MetronomeEvent event) noexcept {
  if (size >= events.size()) {
    overflowed = true;
    return false;
  }
  events[size++] = event;
  return true;
}

void Metronome::prepare(double sample_rate, const transport::TempoMap* tempo_map) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  tempo_map_ = tempo_map;
}

void Metronome::collect_events(int64_t block_start_sample, int num_frames,
                               MetronomeEventList* out) const noexcept {
  if (!out) return;
  out->clear();
  if (!config_.enabled || !tempo_map_ || num_frames <= 0) return;

  const int64_t block_end_sample = block_start_sample + num_frames;
  const double start_ppq = tempo_map_->sample_to_ppq(block_start_sample);
  const double end_ppq = tempo_map_->sample_to_ppq(block_end_sample);
  const double lo = std::min(start_ppq, end_ppq);
  const double hi = std::max(start_ppq, end_ppq);
  const transport::TimeSignature sig = tempo_map_->time_signature_at_ppq(lo);
  const double beat_len = 4.0 / static_cast<double>(std::max(sig.denominator, 1));
  const double first = std::ceil(lo / beat_len) * beat_len;
  for (double ppq = first; ppq <= hi + kPpqEpsilon; ppq += beat_len) {
    const int64_t sample = tempo_map_->ppq_to_sample(ppq);
    if (sample < block_start_sample || sample >= block_end_sample) continue;
    const transport::BarBeat beat = tempo_map_->ppq_to_bar_beat(ppq);
    out->add({static_cast<int>(sample - block_start_sample), beat.beat == 1, sample});
  }
}

void Metronome::process(float* const* channels, int num_channels, int num_frames,
                        int64_t block_start_sample) const noexcept {
  if (!channels || num_channels <= 0 || num_frames <= 0) return;
  MetronomeEventList events;
  collect_events(block_start_sample, num_frames, &events);
  const int click_len =
      config_.click_samples > 0
          ? config_.click_samples
          : std::max(1, static_cast<int>(std::lround(config_.click_seconds * sample_rate_)));
  // Short raised-cosine attack so the click ramps up from silence instead of
  // stepping 0 -> peak, plus an exponential decay forced to exactly zero at the
  // final sample. This removes the start/end discontinuities that caused an
  // audible "tick" artifact while keeping the click transient and roughly the
  // same peak loudness as before.
  const int attack = std::max(1, std::min(click_len / 4, click_len));
  // Decay constant chosen so the exponential reaches ~exp(-6) (about -52 dB) by
  // the end of the click; the explicit zero at the final sample removes any
  // residual tail.
  constexpr double kDecayDecades = 6.0;
  for (size_t e = 0; e < events.size; ++e) {
    const MetronomeEvent& event = events.events[e];
    const float gain = event.accent ? config_.accent_gain : config_.beat_gain;
    for (int i = 0; i < click_len && event.offset + i < num_frames; ++i) {
      float env;
      if (i == click_len - 1) {
        // Guarantee the click ends exactly at zero (no hard step at the tail).
        env = 0.0f;
      } else if (i < attack) {
        // Raised-cosine fade-in from 0 to the peak over the attack window.
        const double phase = static_cast<double>(i + 1) / static_cast<double>(attack + 1);
        env = static_cast<float>(0.5 - 0.5 * std::cos(kPiD * phase));
      } else {
        // Exponential decay from the peak back toward zero.
        const double t =
            static_cast<double>(i - attack) / static_cast<double>(std::max(click_len - attack, 1));
        env = static_cast<float>(std::exp(-kDecayDecades * t));
      }
      const float value = gain * env;
      for (int ch = 0; ch < num_channels; ++ch) {
        if (channels[ch]) channels[ch][event.offset + i] += value;
      }
    }
  }
}

int64_t Metronome::count_in_end_sample(int64_t start_sample, int bars) const noexcept {
  if (!tempo_map_ || bars <= 0) return start_sample;
  const double start_ppq = tempo_map_->sample_to_ppq(start_sample);
  const double bar_start = tempo_map_->bar_start_ppq(start_ppq);
  const transport::TimeSignature sig = tempo_map_->time_signature_at_ppq(start_ppq);
  const double bar_len = static_cast<double>(std::max(sig.numerator, 1)) * 4.0 /
                         static_cast<double>(std::max(sig.denominator, 1));
  return tempo_map_->ppq_to_sample(bar_start + bar_len * static_cast<double>(bars));
}

}  // namespace sonare::engine
