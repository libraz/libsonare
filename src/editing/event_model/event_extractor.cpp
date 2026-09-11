#include "editing/event_model/event_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/spectrum.h"
#include "util/exception.h"

namespace sonare::editing::event_model {
namespace {

StftConfig stft_for(const PercussiveSeparationConfig& separation) {
  StftConfig stft;
  stft.n_fft = separation.n_fft;
  stft.hop_length = separation.hop_length;
  return stft;
}

/// Onset time to a source sample, clamped into the audio. A non-finite or
/// negative time lands on 0 rather than propagating into an index.
int64_t onset_to_sample(float time, int sample_rate, int64_t n_samples) noexcept {
  const double samples = std::round(static_cast<double>(time) * static_cast<double>(sample_rate));
  if (!(samples > 0.0)) return 0;
  if (samples >= static_cast<double>(n_samples)) return n_samples;
  return static_cast<int64_t>(samples);
}

/// Span cap in samples, capped in turn by the track so that adding it to an
/// onset cannot overflow. A positive duration is at least one sample: a
/// sub-sample cap would otherwise make every span empty and drop the whole set.
int64_t max_event_samples(float max_event_ms, int sample_rate, int64_t n_samples) noexcept {
  const double samples =
      std::round(static_cast<double>(max_event_ms) * 0.001 * static_cast<double>(sample_rate));
  if (samples >= static_cast<double>(n_samples)) return n_samples;
  return std::max<int64_t>(1, static_cast<int64_t>(samples));
}

float peak_over(const Audio& audio, int64_t begin, int64_t end) {
  float peak = 0.0f;
  for (int64_t i = begin; i < end; ++i) {
    peak = std::max(peak, std::abs(audio[static_cast<size_t>(i)]));
  }
  return peak;
}

/// Summed squares over [@p begin, @p end), in double so that a long span of a
/// quiet component does not lose the ratio's denominator.
double energy_over(const Audio& audio, int64_t begin, int64_t end) {
  double sum = 0.0;
  for (int64_t i = begin; i < end; ++i) {
    const double value = static_cast<double>(audio[static_cast<size_t>(i)]);
    sum += value * value;
  }
  return sum;
}

}  // namespace

std::vector<PercussiveEvent> extract_percussive_events(
    const Audio& audio, const PercussiveEventExtractorConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  // The separation inverts an STFT, so its framing has to overlap-add.
  validate_cola_geometry(config.separation.n_fft, config.separation.hop_length);
  SONARE_CHECK(std::isfinite(config.max_event_ms) && config.max_event_ms > 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.min_percussive_ratio >= 0.0f && config.min_percussive_ratio <= 1.0f,
               ErrorCode::InvalidParameter);

  const HpssAudioResult separated =
      hpss(audio, config.separation.hpss, stft_for(config.separation));

  // The detector runs on the percussive component and on the separation's own
  // framing, so a harmonic attack is attenuated before it is seen rather than
  // filtered out afterwards.
  OnsetDetectConfig onset_config = config.onset;
  onset_config.n_fft = config.separation.n_fft;
  onset_config.hop_length = config.separation.hop_length;
  const OnsetAnalyzer analyzer(separated.percussive, onset_config);
  const std::vector<Onset>& onsets = analyzer.onsets();

  const int64_t n_samples = static_cast<int64_t>(audio.size());
  const int64_t max_length = max_event_samples(config.max_event_ms, audio.sample_rate(), n_samples);

  std::vector<int64_t> starts(onsets.size());
  for (size_t i = 0; i < onsets.size(); ++i) {
    starts[i] = onset_to_sample(onsets[i].time, audio.sample_rate(), n_samples);
  }

  std::vector<PercussiveEvent> events;
  events.reserve(onsets.size());
  for (size_t i = 0; i < onsets.size(); ++i) {
    // Every span is fixed before any event is dropped, so min_percussive_ratio
    // only selects: a dropped event must not lengthen the one before it.
    const int64_t begin = starts[i];
    const int64_t next = i + 1 < onsets.size() ? starts[i + 1] : n_samples;
    const int64_t end = std::min({next, begin + max_length, n_samples});
    // Two onsets that round onto the same sample; an empty span is no event.
    if (end <= begin) continue;

    PercussiveEvent event;
    event.onset_sample = begin;
    event.offset_sample = end;
    event.strength = onsets[i].strength;
    event.peak_amplitude = peak_over(separated.percussive, begin, end);

    const double percussive_energy = energy_over(separated.percussive, begin, end);
    const double total = percussive_energy + energy_over(separated.harmonic, begin, end);
    event.percussive_ratio =
        total > 0.0 ? static_cast<float>(std::clamp(percussive_energy / total, 0.0, 1.0)) : 0.0f;

    if (event.percussive_ratio < config.min_percussive_ratio) continue;
    events.push_back(event);
  }

  return events;
}

}  // namespace sonare::editing::event_model
