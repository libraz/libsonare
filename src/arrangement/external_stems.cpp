// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "arrangement/external_stems.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "util/constants.h"
#include "util/resource_limits.h"

namespace sonare::arrangement {
namespace {

bool valid_utf8(std::string_view text) noexcept {
  if (text.empty()) return false;
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead == 0) return false;
    if (lead <= 0x7Fu) {
      ++i;
      continue;
    }
    size_t continuation_count = 0;
    uint32_t code_point = 0;
    if ((lead & 0xE0u) == 0xC0u) {
      continuation_count = 1;
      code_point = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
      continuation_count = 2;
      code_point = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
      continuation_count = 3;
      code_point = lead & 0x07u;
    } else {
      return false;
    }
    if (i + continuation_count >= text.size()) return false;
    for (size_t j = 1; j <= continuation_count; ++j) {
      const unsigned char byte = static_cast<unsigned char>(text[i + j]);
      if ((byte & 0xC0u) != 0x80u) return false;
      code_point = (code_point << 6u) | (byte & 0x3Fu);
    }
    const uint32_t minimum = continuation_count == 1   ? 0x80u
                             : continuation_count == 2 ? 0x800u
                                                       : 0x10000u;
    if (code_point < minimum || code_point > 0x10FFFFu ||
        (code_point >= 0xD800u && code_point <= 0xDFFFu)) {
      return false;
    }
    i += continuation_count + 1;
  }
  return true;
}

size_t channel_count(ExternalSeparatedStemLayout layout) noexcept {
  switch (layout) {
    case ExternalSeparatedStemLayout::kMono:
      return 1;
    case ExternalSeparatedStemLayout::kStereo:
      return 2;
  }
  return 0;
}

void prepare_tempo_map(const Project& project, transport::TempoMap* map) {
  if (map == nullptr) return;
  map->prepare(project.sample_rate());
  std::vector<transport::TempoSegment> segments = project.tempo_segments();
  if (segments.empty()) {
    segments.push_back({0.0, constants::kDefaultBpm, 0.0});
  }
  map->set_segments(std::move(segments));
}

bool frame_round_trips(const transport::TempoMap& map, int64_t frame) noexcept {
  return frame >= 0 && map.ppq_to_sample(map.sample_to_ppq(frame)) == frame;
}

ExternalSeparatedStemImportError validate(const Project* project,
                                          const ExternalSeparatedStemSet& input,
                                          const transport::TempoMap* map) {
  if (project == nullptr || map == nullptr || input.stems.empty() || input.sample_rate <= 0 ||
      !std::isfinite(project->sample_rate()) || project->sample_rate() <= 0.0) {
    return ExternalSeparatedStemImportError::kInvalidArgument;
  }
  if (project->sample_rate() != static_cast<double>(input.sample_rate)) {
    return ExternalSeparatedStemImportError::kSampleRateMismatch;
  }

  std::unordered_set<std::string> names;
  size_t total_samples = 0;
  for (const ExternalSeparatedStem& stem : input.stems) {
    const size_t channels = channel_count(stem.layout);
    if (!valid_utf8(stem.name) || !names.insert(stem.name).second || channels == 0 ||
        stem.planar_samples.size() != channels || stem.start_frame < 0) {
      return ExternalSeparatedStemImportError::kInvalidArgument;
    }
    if (stem.role && !stem.role->empty() && !valid_utf8(*stem.role)) {
      return ExternalSeparatedStemImportError::kInvalidArgument;
    }
    const size_t frames = stem.planar_samples.front().size();
    if (frames == 0 || frames > resource::kMaxOfflineAudioSamples / channels ||
        total_samples > resource::kMaxOfflineAudioSamples - frames * channels) {
      return ExternalSeparatedStemImportError::kInvalidArgument;
    }
    for (const std::vector<float>& plane : stem.planar_samples) {
      if (plane.size() != frames) return ExternalSeparatedStemImportError::kInvalidArgument;
      for (float sample : plane) {
        if (!std::isfinite(sample)) return ExternalSeparatedStemImportError::kInvalidArgument;
      }
    }
    total_samples += frames * channels;
    if (stem.start_frame > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(frames) ||
        !frame_round_trips(*map, stem.start_frame) ||
        !frame_round_trips(*map, stem.start_frame + static_cast<int64_t>(frames))) {
      return ExternalSeparatedStemImportError::kFramePositionNotRepresentable;
    }
  }
  return ExternalSeparatedStemImportError::kOk;
}

ExternalSeparatedStemImportResult import_into(Project* project, AudioContentStore* audio,
                                              const ExternalSeparatedStemSet& input,
                                              const transport::TempoMap& map) {
  ExternalSeparatedStemImportResult result;
  result.track_ids.reserve(input.stems.size());
  result.clip_ids.reserve(input.stems.size());
  for (const ExternalSeparatedStem& stem : input.stems) {
    Track track;
    track.name = stem.name;
    track.kind = Track::Kind::kAudio;
    const TrackId track_id = project->add_track(std::move(track));
    if (track_id == 0) {
      result.error = ExternalSeparatedStemImportError::kProjectMutationFailed;
      return result;
    }

    AudioSourceRef source;
    source.channel_count = static_cast<uint32_t>(stem.planar_samples.size());
    source.sample_rate_hint = static_cast<double>(input.sample_rate);
    source.external_stem_role = stem.role.value_or("");
    const SourceId source_id = project->add_audio_source(std::move(source));
    if (source_id == 0) {
      result.error = ExternalSeparatedStemImportError::kProjectMutationFailed;
      return result;
    }

    const int64_t end_frame =
        stem.start_frame + static_cast<int64_t>(stem.planar_samples.front().size());
    EditClip clip;
    clip.track_id = track_id;
    clip.source_id = source_id;
    clip.start_ppq = map.sample_to_ppq(stem.start_frame);
    clip.length_ppq = map.sample_to_ppq(end_frame) - clip.start_ppq;
    clip.gain = 1.0f;
    const ClipId clip_id = project->add_clip(std::move(clip));
    if (clip_id == 0) {
      result.error = ExternalSeparatedStemImportError::kProjectMutationFailed;
      return result;
    }

    AudioSourceSamples samples;
    samples.sample_rate = static_cast<double>(input.sample_rate);
    samples.channels = stem.planar_samples;
    if (!audio->sources.emplace(source_id, std::move(samples)).second) {
      result.error = ExternalSeparatedStemImportError::kProjectMutationFailed;
      return result;
    }
    result.track_ids.push_back(track_id);
    result.clip_ids.push_back(clip_id);
  }
  return result;
}

bool has_audio_source_collision(const AudioContentStore& live_audio,
                                const AudioContentStore& staged_audio) noexcept {
  for (const auto& [source_id, samples] : staged_audio.sources) {
    (void)samples;
    if (live_audio.sources.find(source_id) != live_audio.sources.end()) return true;
  }
  return false;
}

void transfer_new_audio(AudioContentStore* destination, AudioContentStore* staged) noexcept {
  if (destination == nullptr || staged == nullptr) return;
  while (!staged->sources.empty()) {
    auto node = staged->sources.extract(staged->sources.begin());
    // All ids are checked for collisions before the first node is extracted.
    // std::map node insertion does not allocate, and the comparator is the
    // non-throwing integer ordering used by AudioContentStore.
    (void)destination->sources.insert(std::move(node));
  }
}

}  // namespace

ExternalSeparatedStemImportResult import_external_separated_stems(
    Project* project, AudioContentStore* audio, const ExternalSeparatedStemSet& input) {
  if (project == nullptr || audio == nullptr) {
    return {ExternalSeparatedStemImportError::kInvalidArgument, {}, {}};
  }
  // Stage only the Project and the newly imported PCM. Copying the complete
  // AudioContentStore here needlessly duplicates every already registered
  // source (and can exceed the WASM heap for long projects). Existing map nodes
  // remain in place; only the new source nodes are transferred at commit.
  try {
    transport::TempoMap map;
    prepare_tempo_map(*project, &map);
    const ExternalSeparatedStemImportError validation = validate(project, input, &map);
    if (validation != ExternalSeparatedStemImportError::kOk) {
      return {validation, {}, {}};
    }
    Project staged_project = *project;
    AudioContentStore staged_audio;
    ExternalSeparatedStemImportResult result =
        import_into(&staged_project, &staged_audio, input, map);
    if (!result.ok()) return result;

    // An inconsistent caller-owned store may already contain a source id that
    // the project allocator is about to issue. Reject before touching either
    // live object rather than allowing a partial node transfer.
    if (has_audio_source_collision(*audio, staged_audio)) {
      return {ExternalSeparatedStemImportError::kProjectMutationFailed, {}, {}};
    }

    *project = std::move(staged_project);
    transfer_new_audio(audio, &staged_audio);
    return result;
  } catch (...) {
    return {ExternalSeparatedStemImportError::kProjectMutationFailed, {}, {}};
  }
}

}  // namespace sonare::arrangement
