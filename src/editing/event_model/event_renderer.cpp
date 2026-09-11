#include "editing/event_model/event_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "effects/hpss.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::editing::event_model {
namespace {

using sonare::constants::kHalfPi;

int64_t saturating_add(int64_t a, int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
    return std::numeric_limits<int64_t>::max();
  }
  if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
    return std::numeric_limits<int64_t>::min();
  }
  return a + b;
}

StftConfig stft_for(const PercussiveSeparationConfig& separation) {
  StftConfig stft;
  stft.n_fft = separation.n_fft;
  stft.hop_length = separation.hop_length;
  return stft;
}

void check_disjoint_spans(const std::vector<PercussiveEvent>& events) {
  std::vector<size_t> order(events.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(), [&events](size_t a, size_t b) {
    return events[a].onset_sample < events[b].onset_sample;
  });
  for (size_t i = 1; i < order.size(); ++i) {
    SONARE_CHECK(events[order[i]].onset_sample >= events[order[i - 1]].offset_sample,
                 ErrorCode::InvalidParameter);
  }
}

/// The fade zone is bounded by the whole span rather than by half of it as
/// NoteEditor's is: a lifted span carries a tail fade and no head fade, so the
/// two zones cannot reach the same samples.
int64_t fade_samples(float fade_ms, int sample_rate, int64_t span_length) noexcept {
  const double requested =
      std::round(static_cast<double>(fade_ms) * 0.001 * static_cast<double>(sample_rate));
  const int64_t limit = std::max<int64_t>(0, span_length);
  if (!(requested > 0.0)) return 0;
  if (requested >= static_cast<double>(limit)) return limit;
  return static_cast<int64_t>(requested);
}

/// Window at offset @p k of a span of @p length: 1 until the fade zone, then
/// the equal-power (sin) ramp down to 0 at the span's end. There is no fade-in,
/// so a muted hit's attack cannot survive inside one.
float tail_gain(int64_t k, int64_t length, int64_t fade) noexcept {
  if (fade <= 0 || k < length - fade) return 1.0f;
  const float phase = (static_cast<float>(length - k) - 0.5f) / static_cast<float>(fade);
  return std::sin(kHalfPi * phase);
}

}  // namespace

Audio render_percussive_events(const Audio& audio, const std::vector<PercussiveEvent>& events,
                               const PercussiveEventRenderConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.fade_ms) && config.fade_ms > 0.0f, ErrorCode::InvalidParameter);
  // Checked here rather than left to the HPSS call, which the identity path
  // never reaches: an unusable framing is an error on every set.
  validate_cola_geometry(config.separation.n_fft, config.separation.hop_length);

  const int sample_rate = audio.sample_rate();
  const int64_t n_samples = static_cast<int64_t>(audio.size());

  bool all_identity = true;
  for (const PercussiveEvent& event : events) {
    SONARE_CHECK(
        event.onset_sample >= 0 && event.length_samples() > 0 && event.offset_sample <= n_samples,
        ErrorCode::InvalidParameter);
    SONARE_CHECK(std::isfinite(event.edit.gain_db), ErrorCode::InvalidParameter);
    all_identity = all_identity && event.edit.is_identity();
  }
  check_disjoint_spans(events);

  // Nothing to lift: the source passes through unchanged, bit for bit, and no
  // separation runs at all.
  if (all_identity) {
    return Audio::from_buffer(audio.data(), audio.size(), audio.sample_rate());
  }

  // Only the percussive component is read below, so the separation reconstructs
  // that one instead of running a second inverse transform and discarding it.
  const Audio separated_percussive =
      percussive(audio, config.separation.hpss, stft_for(config.separation));
  std::vector<float> output(audio.begin(), audio.end());

  for (const PercussiveEvent& event : events) {
    if (event.edit.is_identity()) continue;

    // Only the percussive component moves, so whatever was sounding under the
    // hit stays where the source put it.
    const int64_t length = event.length_samples();
    const int64_t fade = fade_samples(config.fade_ms, sample_rate, length);
    std::vector<float> segment(static_cast<size_t>(length));
    for (int64_t k = 0; k < length; ++k) {
      const size_t index = static_cast<size_t>(event.onset_sample + k);
      const float lifted = tail_gain(k, length, fade) * separated_percussive[index];
      segment[static_cast<size_t>(k)] = lifted;
      output[index] -= lifted;
    }
    if (event.edit.muted) continue;

    // A shift that runs off either end is truncated there rather than wrapped.
    const float gain = db_to_linear(event.edit.gain_db);
    const int64_t dest = saturating_add(event.onset_sample, event.edit.time_offset_samples);
    const int64_t begin = std::max<int64_t>(dest, 0);
    const int64_t end = std::min(saturating_add(dest, length), n_samples);
    for (int64_t j = begin; j < end; ++j) {
      output[static_cast<size_t>(j)] += gain * segment[static_cast<size_t>(j - dest)];
    }
  }

  return Audio::from_vector(std::move(output), sample_rate);
}

}  // namespace sonare::editing::event_model
