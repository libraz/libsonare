#include "editing/pitch_editor/note_editor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "editing/note_model/note_renderer.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::editing::pitch_editor {

using sonare::constants::kPi;

NoteEditor::NoteEditor(NoteEditorConfig config) : config_(config) {}

Audio NoteEditor::move_note(const Audio& audio, const NoteRegion& region,
                            int target_onset_sample) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  NoteRegion clipped = clamp_region(audio, region);
  const int length = clipped.offset_sample - clipped.onset_sample;
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);
  // The vacated span is ramped out whatever the target is, so a target outside the
  // buffer would erase the note and paste it nowhere.
  SONARE_CHECK_MSG(target_onset_sample >= 0 && target_onset_sample < static_cast<int>(audio.size()),
                   ErrorCode::InvalidParameter,
                   "NoteEditor::move_note: targetOnsetSample must fall inside the buffer, got " +
                       std::to_string(target_onset_sample) + " for " +
                       std::to_string(audio.size()) + " samples");

  const int fade = fade_samples(audio.sample_rate(), length);
  std::vector<float> output(audio.begin(), audio.end());
  std::vector<float> segment(audio.begin() + clipped.onset_sample,
                             audio.begin() + clipped.offset_sample);
  apply_edge_fades(segment, fade);

  // Ramps the vacated span out instead of cutting it, the way note rendering
  // vacates the same kind of span.
  note_model::erase_span(output, audio, clipped.onset_sample, clipped.offset_sample, fade);

  // A note landing near the end keeps the part that fits; only a target with no
  // room at all is refused above.
  const int target_end = std::min(target_onset_sample + length, static_cast<int>(output.size()));
  for (int i = target_onset_sample; i < target_end; ++i) {
    output[static_cast<size_t>(i)] += segment[static_cast<size_t>(i - target_onset_sample)];
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
  Audio segment_audio = Audio::from_vector(std::move(segment), audio.sample_rate());

  TimeStretchConfig stretch_config;
  stretch_config.backend = config_.stretch_backend;
  Audio stretched = time_stretch(segment_audio, 1.0f / stretch_ratio, stretch_config);

  std::vector<float> stretched_samples(stretched.begin(), stretched.end());

  // Each seam cross-fades the stretched region against the source samples that
  // are continuous with the untouched neighbour -- the note's own start at the
  // head seam, its own end at the tail seam -- with the equal-power weights
  // note_model::overlay splices with. Nothing overlaps the neighbour itself, so
  // the result is only the region's length change away from the input. A seam at
  // a buffer edge has no neighbour and is left uncrossed.
  const int stretched_len = static_cast<int>(stretched_samples.size());
  const int fade = std::min(fade_samples(audio.sample_rate(), stretched_len), length);
  const int head_fade = clipped.onset_sample > 0 ? fade : 0;
  const int tail_fade = clipped.offset_sample < static_cast<int>(audio.size()) ? fade : 0;

  std::vector<float> output;
  output.reserve(audio.size() - static_cast<size_t>(length) + stretched_samples.size());
  output.insert(output.end(), audio.begin(), audio.begin() + clipped.onset_sample);
  const size_t region_start = output.size();
  output.insert(output.end(), stretched_samples.begin(), stretched_samples.end());
  for (int k = 0; k < head_fade; ++k) {
    const float phase = (static_cast<float>(k) + 0.5f) / static_cast<float>(head_fade);
    float& sample = output[region_start + static_cast<size_t>(k)];
    sample = std::sin(0.5f * kPi * phase) * sample +
             std::cos(0.5f * kPi * phase) * audio[static_cast<size_t>(clipped.onset_sample + k)];
  }
  const size_t tail_start = region_start + static_cast<size_t>(stretched_len - tail_fade);
  const int source_tail = clipped.offset_sample - tail_fade;
  for (int k = 0; k < tail_fade; ++k) {
    const float phase = (static_cast<float>(k) + 0.5f) / static_cast<float>(tail_fade);
    float& sample = output[tail_start + static_cast<size_t>(k)];
    sample = std::cos(0.5f * kPi * phase) * sample +
             std::sin(0.5f * kPi * phase) * audio[static_cast<size_t>(source_tail + k)];
  }
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
    samples[static_cast<size_t>(i)] *= in_gain;
    samples[samples.size() - 1U - static_cast<size_t>(i)] *= in_gain;
  }
}

NoteRegion NoteEditor::clamp_region(const Audio& audio, const NoteRegion& region) {
  const int size = static_cast<int>(audio.size());
  NoteRegion clipped = region;
  clipped.onset_sample = std::clamp(clipped.onset_sample, 0, size);
  clipped.offset_sample = std::clamp(clipped.offset_sample, clipped.onset_sample, size);
  return clipped;
}

}  // namespace sonare::editing::pitch_editor
