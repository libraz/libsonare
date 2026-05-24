#include "analysis/pitch_editor/note_editor.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::analysis::pitch_editor {

using sonare::constants::kPi;

NoteEditor::NoteEditor(NoteEditorConfig config) : config_(config) {}

Audio NoteEditor::move_note(const Audio& audio, const NoteRegion& region,
                            int target_onset_sample) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  NoteRegion clipped = clamp_region(audio, region);
  const int length = clipped.offset_sample - clipped.onset_sample;
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);

  std::vector<float> output(audio.begin(), audio.end());
  std::vector<float> segment(audio.begin() + clipped.onset_sample,
                             audio.begin() + clipped.offset_sample);
  apply_edge_fades(segment, fade_samples(audio.sample_rate(), length));

  std::fill(output.begin() + clipped.onset_sample, output.begin() + clipped.offset_sample, 0.0f);

  const int target_start = std::clamp(target_onset_sample, 0, static_cast<int>(output.size()));
  const int target_end = std::min(target_start + length, static_cast<int>(output.size()));
  for (int i = target_start; i < target_end; ++i) {
    output[static_cast<size_t>(i)] += segment[static_cast<size_t>(i - target_start)];
  }

  return Audio::from_vector(std::move(output), audio.sample_rate());
}

Audio NoteEditor::stretch_note(const Audio& audio, const NoteRegion& region,
                               float stretch_ratio) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(stretch_ratio > 0.0f && std::isfinite(stretch_ratio), ErrorCode::InvalidParameter);
  NoteRegion clipped = clamp_region(audio, region);
  const int length = clipped.offset_sample - clipped.onset_sample;
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);

  std::vector<float> segment(audio.begin() + clipped.onset_sample,
                             audio.begin() + clipped.offset_sample);
  apply_edge_fades(segment, fade_samples(audio.sample_rate(), length));
  Audio segment_audio = Audio::from_vector(std::move(segment), audio.sample_rate());

  TimeStretchConfig stretch_config;
  stretch_config.backend = config_.stretch_backend;
  Audio stretched = time_stretch(segment_audio, 1.0f / stretch_ratio, stretch_config);

  std::vector<float> stretched_samples(stretched.begin(), stretched.end());
  apply_edge_fades(stretched_samples,
                   fade_samples(audio.sample_rate(), static_cast<int>(stretched_samples.size())));

  std::vector<float> output;
  output.reserve(audio.size() - static_cast<size_t>(length) + stretched_samples.size());
  output.insert(output.end(), audio.begin(), audio.begin() + clipped.onset_sample);
  output.insert(output.end(), stretched_samples.begin(), stretched_samples.end());
  output.insert(output.end(), audio.begin() + clipped.offset_sample, audio.end());

  return Audio::from_vector(std::move(output), audio.sample_rate());
}

int NoteEditor::fade_samples(int sample_rate, int region_length) const noexcept {
  const int requested =
      static_cast<int>(std::round(config_.fade_ms * 0.001f * static_cast<float>(sample_rate)));
  return std::clamp(requested, 0, std::max(0, region_length / 2));
}

void NoteEditor::apply_edge_fades(std::vector<float>& samples, int fade_samples) {
  if (fade_samples <= 0 || samples.empty()) {
    return;
  }
  const int n = std::min(fade_samples, static_cast<int>(samples.size() / 2));
  for (int i = 0; i < n; ++i) {
    const float phase = static_cast<float>(i + 1) / static_cast<float>(n + 1);
    const float in_gain = 0.5f - 0.5f * std::cos(kPi * phase);
    const float out_gain = 1.0f - in_gain;
    samples[static_cast<size_t>(i)] *= in_gain;
    samples[samples.size() - 1U - static_cast<size_t>(i)] *= out_gain;
  }
}

NoteRegion NoteEditor::clamp_region(const Audio& audio, const NoteRegion& region) {
  const int size = static_cast<int>(audio.size());
  NoteRegion clipped = region;
  clipped.onset_sample = std::clamp(clipped.onset_sample, 0, size);
  clipped.offset_sample = std::clamp(clipped.offset_sample, clipped.onset_sample, size);
  return clipped;
}

}  // namespace sonare::analysis::pitch_editor
