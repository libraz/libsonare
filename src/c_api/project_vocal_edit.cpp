#include <sonare/sonare_c_vocal_project.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "c_api/project_internal.h"
#include "util/resource_limits.h"
#include "util/sha256.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "arrangement/vocal_edit_sidecar.h"
#endif

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "editing/vocal_edit/state_codec.h"
#endif

namespace {

template <typename T>
void init_value(T* value) {
  if (value == nullptr) return;
  *value = {};
  value->struct_size = sizeof(T);
  value->schema_version = SONARE_VOCAL_PROJECT_API_VERSION;
}

template <typename T>
bool initialized(const T* value) noexcept {
  return value != nullptr && value->struct_size >= sizeof(T) &&
         value->schema_version == SONARE_VOCAL_PROJECT_API_VERSION;
}

template <size_t N>
void copy_key(char (&out)[N], const std::string& key) noexcept {
  const size_t length = std::min(key.size(), N - 1u);
  if (length != 0) std::memcpy(out, key.data(), length);
  out[length] = '\0';
}

template <typename T>
void clear_output(T* value) {
  init_value(value);
}

#if defined(SONARE_WITH_ARRANGEMENT)

std::array<uint8_t, 32> digest_samples(const std::vector<float>& samples) {
  sonare::util::Sha256 sha;
  sha.update(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(float));
  return sha.finalize();
}

std::string canonical_hash(const std::array<uint8_t, 32>& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result = "sha256:";
  result.reserve(71);
  for (const uint8_t byte : digest) {
    result.push_back(kHex[byte >> 4u]);
    result.push_back(kHex[byte & 0x0fu]);
  }
  return result;
}

#if defined(SONARE_WITH_PITCH_EDITOR)
bool digest_equal(const uint8_t* lhs, const std::array<uint8_t, 32>& rhs) noexcept {
  return lhs != nullptr && std::memcmp(lhs, rhs.data(), rhs.size()) == 0;
}
#endif

bool finite_pcm(const std::vector<float>& samples) noexcept {
  for (const float sample : samples) {
    if (!std::isfinite(sample)) return false;
  }
  return true;
}

bool valid_source_rate(uint32_t sample_rate) noexcept {
  return sample_rate >= static_cast<uint32_t>(sonare::kMinAudioSampleRate) &&
         sample_rate <= static_cast<uint32_t>(sonare::kMaxAudioSampleRate);
}

bool valid_id(uint32_t id) noexcept {
  return id != 0 && id != std::numeric_limits<uint32_t>::max();
}

using arr::AudioSourceRef;
using arr::AudioSourceSamples;
using arr::ClipId;
using arr::ClipTake;
using arr::EditClip;
using arr::Project;
using arr::SourceId;
using sonare::arrangement::vocal_sidecar::Envelope;
using sonare::arrangement::vocal_sidecar::Key;

constexpr size_t kEnvelopeHeaderBytes = 112u;

const AudioSourceRef* audio_source(const Project& project, SourceId source_id) noexcept {
  const arr::ClipSource* source = project.find_source(source_id);
  return source == nullptr ? nullptr : std::get_if<AudioSourceRef>(source);
}

#if defined(SONARE_WITH_PITCH_EDITOR)
const EditClip* resolve_clip(const Project& project, uint32_t clip_id) noexcept {
  return project.find_clip(static_cast<ClipId>(clip_id));
}

const ClipTake* resolve_take(const EditClip& clip, uint32_t take_id) noexcept {
  for (const ClipTake& take : clip.takes) {
    if (take.id == take_id) return &take;
  }
  return nullptr;
}

SourceId resolved_source(const EditClip& clip, uint32_t take_id, double* offset) noexcept {
  if (take_id == 0) {
    if (offset != nullptr) *offset = clip.source_offset_ppq;
    return clip.source_id;
  }
  const ClipTake* take = resolve_take(clip, take_id);
  if (take == nullptr) return 0;
  if (offset != nullptr) *offset = take->source_offset_ppq;
  return take->source_id == 0 ? clip.source_id : take->source_id;
}
#endif

bool store_pcm_matches(const SonareProject& project, SourceId source_id, uint32_t sample_rate,
                       int64_t sample_count, const std::array<uint8_t, 32>& expected_digest) {
  const auto it = project.audio.sources.find(source_id);
  if (it == project.audio.sources.end() || it->second.channels.size() != 1) return false;
  const auto& samples = it->second.channels.front();
  if (it->second.sample_rate != static_cast<double>(sample_rate) ||
      samples.size() != static_cast<size_t>(sample_count) || !finite_pcm(samples)) {
    return false;
  }
  return digest_samples(samples) == expected_digest;
}

#if defined(SONARE_WITH_PITCH_EDITOR)
bool audio_content_matches(const AudioSourceSamples& content, uint32_t sample_rate,
                           int64_t sample_count, const std::array<uint8_t, 32>& expected_digest) {
  if (content.channels.size() != 1 || content.sample_rate != static_cast<double>(sample_rate)) {
    return false;
  }
  const auto& samples = content.channels.front();
  return samples.size() == static_cast<size_t>(sample_count) && finite_pcm(samples) &&
         digest_samples(samples) == expected_digest;
}

#endif

bool metadata_hash_matches(const AudioSourceRef& source,
                           const std::array<uint8_t, 32>& expected_digest) {
  return source.content_hash == canonical_hash(expected_digest);
}

bool valid_envelope_shape(const Envelope& envelope) noexcept {
  return valid_id(envelope.original_source_id) && valid_id(envelope.derived_source_id) &&
         envelope.original_source_id != envelope.derived_source_id &&
         valid_source_rate(envelope.source_sample_rate) && envelope.source_sample_count > 0 &&
         envelope.source_sample_count <= static_cast<int64_t>(sonare::kMaxAudioBufferSize) &&
         envelope.sve1.size() <=
             sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes;
}

bool known_envelope_header(const std::vector<uint8_t>& payload) noexcept {
  if (payload.size() < 8) return false;
  if (payload[0] != 'S' || payload[1] != 'V' || payload[2] != 'P' || payload[3] != '1') {
    return false;
  }
  const uint16_t version = static_cast<uint16_t>(payload[4]) | static_cast<uint16_t>(payload[5])
                                                                   << 8u;
  const uint16_t flags = static_cast<uint16_t>(payload[6]) | static_cast<uint16_t>(payload[7])
                                                                 << 8u;
  return version == 1u && flags == 1u;
}

uint32_t malformed_reason(const arr::AssistSidecar& sidecar) noexcept {
  if (sidecar.schema_version != sonare::arrangement::vocal_sidecar::kSchemaVersion) {
    return SONARE_VOCAL_REASON_UNSUPPORTED;
  }
  if (sidecar.payload.size() >
      kEnvelopeHeaderBytes +
          sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes) {
    return SONARE_VOCAL_REASON_INVALID_STATE;
  }
  return known_envelope_header(sidecar.payload) ? SONARE_VOCAL_REASON_INVALID_STATE
                                                : SONARE_VOCAL_REASON_UNSUPPORTED;
}

void fill_dependency_header(SonareProjectVocalEditDependency* value) { init_value(value); }

void fill_dependency_from_envelope(const Key& key, const Envelope& envelope,
                                   SonareProjectVocalEditDependency* value) {
  fill_dependency_header(value);
  value->clip_id = key.clip_id;
  value->take_id = key.take_id;
  value->original_source_id = envelope.original_source_id;
  value->derived_source_id = envelope.derived_source_id;
  value->source_sample_rate = envelope.source_sample_rate;
  value->profile_id = envelope.profile_id;
  value->source_sample_count = envelope.source_sample_count;
  value->committed_revision = envelope.committed_revision;
  std::memcpy(value->original_source_sha256, envelope.original_digest.data(), 32);
  std::memcpy(value->derived_source_sha256, envelope.derived_digest.data(), 32);
  copy_key(value->sidecar_key, arr::vocal_sidecar::make_key(key));
}

#if defined(SONARE_WITH_PITCH_EDITOR)
bool state_matches_envelope(const sonare::editing::vocal_edit::VocalPersistedState& state,
                            const Envelope& envelope) noexcept {
  return state.source.sample_rate == envelope.source_sample_rate &&
         state.source.sample_count == envelope.source_sample_count &&
         state.source.digest == envelope.original_digest &&
         state.committed_revision == envelope.committed_revision &&
         static_cast<uint32_t>(state.render_settings.profile) == envelope.profile_id &&
         state.output_length_samples == envelope.source_sample_count;
}

uint32_t state_decode_failure_reason(
    const sonare::editing::vocal_edit::VocalEditException& error) noexcept {
  using sonare::editing::vocal_edit::VocalReason;
  if (error.reason() == VocalReason::kUnsupported) return SONARE_VOCAL_REASON_UNSUPPORTED;
  const std::string_view field = error.field();
  const std::string_view message = error.what();
  if (field == "schema" || field == "render_settings.profile" ||
      field == "render_settings.algorithm_version" ||
      (field.find("algorithm") != std::string_view::npos &&
       message.find("not supported") != std::string_view::npos)) {
    return SONARE_VOCAL_REASON_UNSUPPORTED;
  }
  return SONARE_VOCAL_REASON_INVALID_STATE;
}
#endif

#if defined(SONARE_WITH_PITCH_EDITOR)
bool current_binding_matches(const Project& project, const Key& key,
                             SourceId derived_source_id) noexcept {
  const EditClip* clip = project.find_clip(key.clip_id);
  if (clip == nullptr) return false;
  return resolved_source(*clip, key.take_id, nullptr) == derived_source_id;
}
#endif

struct SourceInput {
  uint32_t source_id = 0;
  const float* mono = nullptr;
  int64_t sample_count = 0;
  uint32_t sample_rate = 0;
  std::array<uint8_t, 32> digest{};
};

#if defined(SONARE_WITH_PITCH_EDITOR)
bool valid_original_input(const SonareProjectVocalOriginalSource& value, SourceInput* out) {
  if (!initialized(&value) || !valid_id(value.source_id) || value.mono == nullptr ||
      value.sample_count <= 0 ||
      value.sample_count > static_cast<int64_t>(sonare::kMaxAudioBufferSize) ||
      !valid_source_rate(value.sample_rate)) {
    return false;
  }
  const size_t count = static_cast<size_t>(value.sample_count);
  std::vector<float> samples(value.mono, value.mono + count);
  if (!finite_pcm(samples)) return false;
  if (out != nullptr) {
    out->source_id = value.source_id;
    out->mono = value.mono;
    out->sample_count = value.sample_count;
    out->sample_rate = value.sample_rate;
    out->digest = digest_samples(samples);
  }
  return true;
}
#endif

#endif  // SONARE_WITH_ARRANGEMENT

}  // namespace

extern "C" {

void sonare_project_vocal_edit_apply_desc_init(SonareProjectVocalEditApplyDesc* value) {
  init_value(value);
}

void sonare_project_vocal_edit_apply_result_init(SonareProjectVocalEditApplyResult* value) {
  init_value(value);
}

void sonare_project_vocal_edit_dependencies_result_init(
    SonareProjectVocalEditDependenciesResult* value) {
  init_value(value);
}

void sonare_project_vocal_original_source_init(SonareProjectVocalOriginalSource* value) {
  init_value(value);
}

void sonare_project_vocal_rehydrate_result_init(SonareProjectVocalRehydrateResult* value) {
  init_value(value);
}

SonareError sonare_project_apply_vocal_edit(SonareProject* project,
                                            const SonareProjectVocalEditApplyDesc* desc,
                                            SonareProjectVocalEditApplyResult* result) {
  SONARE_C_API_ENTRY;
  if (!initialized(result)) return SONARE_ERROR_INVALID_PARAMETER;
  clear_output(result);
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  if (project == nullptr || desc == nullptr || !initialized(desc) || !valid_id(desc->clip_id) ||
      desc->take_id == std::numeric_limits<uint32_t>::max()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (desc->expected_source_sample_count <= 0 ||
      desc->expected_source_sample_count > static_cast<int64_t>(sonare::kMaxAudioBufferSize) ||
      !valid_source_rate(desc->expected_source_sample_rate) ||
      desc->rendered_sample_count != desc->expected_source_sample_count ||
      desc->rendered_sample_rate != desc->expected_source_sample_rate ||
      desc->rendered_start_sample != 0 || desc->rendered_mono == nullptr || desc->sve1 == nullptr ||
      desc->sve1_size == 0 ||
      desc->sve1_size >
          sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes ||
      !std::isfinite(desc->expected_clip_length_ppq) ||
      !std::isfinite(desc->expected_source_offset_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const Project& model = project->history.project();
  const EditClip* clip = resolve_clip(model, desc->clip_id);
  if (clip == nullptr || clip->source_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::Track* track = model.find_track(clip->track_id);
  if (track == nullptr || track->kind != arr::Track::Kind::kAudio) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  double target_offset = 0.0;
  const SourceId original_source_id = resolved_source(*clip, desc->take_id, &target_offset);
  if (!valid_id(original_source_id) || original_source_id != desc->expected_source_id ||
      clip->length_ppq != desc->expected_clip_length_ppq ||
      target_offset != desc->expected_source_offset_ppq) {
    return SONARE_ERROR_INVALID_STATE;
  }
  if (clip->loop_mode != arr::LoopMode::kOff || clip->warp_ref_id != 0 ||
      clip->warp_mode != arr::WarpMode::kOff || !clip->comp_segments.empty() ||
      !clip->comp_render_parts.empty()) {
    return SONARE_ERROR_NOT_SUPPORTED;
  }
  const AudioSourceRef* original_ref = audio_source(model, original_source_id);
  const auto original_pcm_it = project->audio.sources.find(original_source_id);
  if (original_ref == nullptr || original_pcm_it == project->audio.sources.end() ||
      original_pcm_it->second.channels.size() != 1) {
    return SONARE_ERROR_INVALID_STATE;
  }
  const auto& original_pcm = original_pcm_it->second.channels.front();
  if (original_pcm.size() != static_cast<size_t>(desc->expected_source_sample_count) ||
      original_pcm_it->second.sample_rate !=
          static_cast<double>(desc->expected_source_sample_rate) ||
      !finite_pcm(original_pcm)) {
    return SONARE_ERROR_INVALID_STATE;
  }
  const auto original_digest = digest_samples(original_pcm);
  if (!digest_equal(desc->expected_source_sha256, original_digest)) {
    return SONARE_ERROR_INVALID_STATE;
  }

  const auto persisted = sonare::editing::vocal_edit::decode_vocal_state(
      desc->sve1, static_cast<size_t>(desc->sve1_size));
  if (!state_matches_envelope(persisted,
                              Envelope{original_source_id,
                                       0,
                                       desc->expected_source_sample_rate,
                                       static_cast<uint32_t>(persisted.render_settings.profile),
                                       desc->expected_source_sample_count,
                                       persisted.committed_revision,
                                       original_digest,
                                       {},
                                       {}}) ||
      desc->render_token.revision != persisted.committed_revision ||
      desc->render_token.draft_id != 0 || desc->render_token.generation != 0 ||
      desc->render_token.profile_id != static_cast<uint32_t>(persisted.render_settings.profile) ||
      persisted.output_length_samples != desc->expected_source_sample_count) {
    return SONARE_ERROR_INVALID_STATE;
  }
  std::vector<float> rendered(
      desc->rendered_mono, desc->rendered_mono + static_cast<size_t>(desc->rendered_sample_count));
  if (!finite_pcm(rendered)) return SONARE_ERROR_INVALID_PARAMETER;
  const auto derived_digest = digest_samples(rendered);
  const SourceId derived_source_id = model.next_source_id();
  if (!valid_id(derived_source_id) || model.find_source(derived_source_id) != nullptr ||
      project->audio.sources.find(derived_source_id) != project->audio.sources.end()) {
    return SONARE_ERROR_INVALID_STATE;
  }

  arr::AudioSourceRef derived_ref;
  derived_ref.channel_count = 1;
  derived_ref.sample_rate_hint = static_cast<double>(desc->rendered_sample_rate);
  derived_ref.content_hash = canonical_hash(derived_digest);
  auto attach = std::make_unique<arr::AttachAudioSource>(derived_ref);
  attach->reseed_id(derived_source_id);

  std::map<SourceId, AudioSourceSamples> pcm;
  AudioSourceSamples rendered_content;
  rendered_content.sample_rate = static_cast<double>(desc->rendered_sample_rate);
  rendered_content.channels.push_back(std::move(rendered));
  pcm.emplace(derived_source_id, std::move(rendered_content));

  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(6);
  commands.push_back(std::move(attach));
  commands.push_back(
      sonare_project_make_store_audio_content_command(&project->audio, std::move(pcm)));
  if (desc->take_id == 0) {
    std::vector<ClipTake> takes = clip->takes;
    bool materialized = false;
    for (ClipTake& take : takes) {
      if (take.source_id == 0) {
        take.source_id = original_source_id;
        materialized = true;
      }
    }
    if (materialized)
      commands.push_back(
          std::make_unique<arr::SetClipTakes>(clip->id, std::move(takes), clip->active_take_id));
    commands.push_back(std::make_unique<arr::SetClipSource>(clip->id, derived_source_id));
  } else {
    std::vector<ClipTake> takes = clip->takes;
    ClipTake* selected = nullptr;
    for (ClipTake& take : takes) {
      if (take.id == desc->take_id) {
        selected = &take;
        break;
      }
    }
    if (selected == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
    selected->source_id = derived_source_id;
    commands.push_back(
        std::make_unique<arr::SetClipTakes>(clip->id, std::move(takes), clip->active_take_id));
  }

  Envelope envelope;
  envelope.original_source_id = original_source_id;
  envelope.derived_source_id = derived_source_id;
  envelope.source_sample_rate = desc->expected_source_sample_rate;
  envelope.profile_id = static_cast<uint32_t>(persisted.render_settings.profile);
  envelope.source_sample_count = desc->expected_source_sample_count;
  envelope.committed_revision = persisted.committed_revision;
  envelope.original_digest = original_digest;
  envelope.derived_digest = derived_digest;
  envelope.sve1.assign(desc->sve1, desc->sve1 + static_cast<size_t>(desc->sve1_size));
  const Key key{desc->clip_id, desc->take_id};
  const std::string sidecar_key = arr::vocal_sidecar::make_key(key);
  SonareProjectVocalEditApplyResult prepared_result;
  init_value(&prepared_result);
  prepared_result.clip_id = desc->clip_id;
  prepared_result.take_id = desc->take_id;
  prepared_result.original_source_id = original_source_id;
  prepared_result.derived_source_id = derived_source_id;
  prepared_result.committed_revision = persisted.committed_revision;
  prepared_result.profile_id = envelope.profile_id;
  std::memcpy(prepared_result.derived_source_sha256, derived_digest.data(), derived_digest.size());
  copy_key(prepared_result.sidecar_key, sidecar_key);
  commands.push_back(
      arr::vocal_sidecar::make_upsert_command(arr::vocal_sidecar::encode_envelope(key, envelope)));
  if (std::any_of(commands.begin(), commands.end(),
                  [](const arr::EditCommandPtr& command) { return command == nullptr; }) ||
      !project->history.apply_transaction(std::move(commands))) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *result = prepared_result;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, result);
#endif
}

SonareError sonare_project_get_vocal_edit_dependencies(
    const SonareProject* project, SonareProjectVocalEditDependenciesResult* result) {
  SONARE_C_API_ENTRY;
  if (!initialized(result)) return SONARE_ERROR_INVALID_PARAMETER;
  clear_output(result);
#if defined(SONARE_WITH_ARRANGEMENT)
  if (project == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<SonareProjectVocalEditDependency> dependencies;
  for (const arr::AssistSidecar& sidecar : project->history.project().assist_sidecars()) {
    const auto key = arr::vocal_sidecar::parse_key(sidecar.module_id);
    if (!key.has_value()) continue;
    SonareProjectVocalEditDependency dependency;
    Envelope envelope;
    if (sidecar.payload.size() >
            kEnvelopeHeaderBytes +
                sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes ||
        !arr::vocal_sidecar::decode_envelope(sidecar, &envelope)) {
      fill_dependency_header(&dependency);
      dependency.clip_id = key->clip_id;
      dependency.take_id = key->take_id;
      copy_key(dependency.sidecar_key, arr::vocal_sidecar::make_key(*key));
      dependency.reason = malformed_reason(sidecar);
      dependencies.push_back(dependency);
      continue;
    }
    fill_dependency_from_envelope(*key, envelope, &dependency);
    if (!valid_envelope_shape(envelope)) {
      dependency.reason = SONARE_VOCAL_REASON_INVALID_STATE;
      dependencies.push_back(dependency);
      continue;
    }
    dependency.original_pcm_available =
        store_pcm_matches(*project, envelope.original_source_id, envelope.source_sample_rate,
                          envelope.source_sample_count, envelope.original_digest)
            ? 1u
            : 0u;
    dependency.derived_pcm_available =
        store_pcm_matches(*project, envelope.derived_source_id, envelope.source_sample_rate,
                          envelope.source_sample_count, envelope.derived_digest)
            ? 1u
            : 0u;
    const AudioSourceRef* original_ref =
        audio_source(project->history.project(), envelope.original_source_id);
    const AudioSourceRef* derived_ref =
        audio_source(project->history.project(), envelope.derived_source_id);
    if (original_ref == nullptr || derived_ref == nullptr ||
        !metadata_hash_matches(*derived_ref, envelope.derived_digest)) {
      dependency.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
    }
#if defined(SONARE_WITH_PITCH_EDITOR)
    if (dependency.reason == SONARE_VOCAL_REASON_SOURCE_MISMATCH) {
      // Keep source metadata and PCM mismatches distinct from unsupported codecs.
    } else if (envelope.profile_id != 1u || envelope.sve1.empty()) {
      dependency.reason = SONARE_VOCAL_REASON_UNSUPPORTED;
    } else {
      try {
        const auto state = sonare::editing::vocal_edit::decode_vocal_state(envelope.sve1);
        if (!state_matches_envelope(state, envelope)) {
          dependency.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
        }
      } catch (const sonare::editing::vocal_edit::VocalEditException& error) {
        dependency.reason = state_decode_failure_reason(error);
      } catch (const std::bad_alloc&) {
        throw;
      } catch (...) {
        dependency.reason = SONARE_VOCAL_REASON_UNSUPPORTED;
      }
    }
#else
    dependency.reason = SONARE_VOCAL_REASON_UNSUPPORTED;
#endif
    dependencies.push_back(dependency);
  }
  if (dependencies.empty()) return SONARE_OK;
  auto output = std::unique_ptr<SonareProjectVocalEditDependency[]>(
      new SonareProjectVocalEditDependency[dependencies.size()]);
  for (size_t i = 0; i < dependencies.size(); ++i) output[i] = dependencies[i];
  result->dependencies = output.release();
  result->dependency_count = dependencies.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, result);
#endif
}

SonareError sonare_project_rehydrate_vocal_edits(SonareProject* project,
                                                 const SonareProjectVocalOriginalSource* originals,
                                                 uint64_t original_count,
                                                 SonareVocalCancelCallback cancel, void* user_data,
                                                 SonareProjectVocalRehydrateResult* result) {
  SONARE_C_API_ENTRY;
  if (!initialized(result)) return SONARE_ERROR_INVALID_PARAMETER;
  clear_output(result);
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  if (project == nullptr || (original_count != 0 && originals == nullptr) ||
      original_count > sonare::resource::kDefaultProjectImportResourceLimits.max_entities) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<SourceInput> source_inputs;
  source_inputs.reserve(static_cast<size_t>(original_count));
  std::set<uint32_t> source_ids;
  for (uint64_t i = 0; i < original_count; ++i) {
    SourceInput input;
    if (!valid_original_input(originals[i], &input) || !source_ids.insert(input.source_id).second) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    source_inputs.push_back(input);
  }
  const auto find_input = [&](uint32_t source_id) -> const SourceInput* {
    for (const SourceInput& input : source_inputs) {
      if (input.source_id == source_id) return &input;
    }
    return nullptr;
  };

  std::vector<SonareProjectVocalRehydrateItem> items;
  std::map<SourceId, AudioSourceSamples> staged;
  for (const arr::AssistSidecar& sidecar : project->history.project().assist_sidecars()) {
    const auto key = arr::vocal_sidecar::parse_key(sidecar.module_id);
    if (!key.has_value()) continue;
    if (cancel != nullptr && cancel(user_data) != 0) return SONARE_ERROR_CANCELLED;
    SonareProjectVocalRehydrateItem item;
    init_value(&item);
    item.clip_id = key->clip_id;
    item.take_id = key->take_id;
    Envelope envelope;
    if (sidecar.payload.size() >
            kEnvelopeHeaderBytes +
                sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes ||
        !arr::vocal_sidecar::decode_envelope(sidecar, &envelope)) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = malformed_reason(sidecar);
      items.push_back(item);
      continue;
    }
    item.derived_source_id = envelope.derived_source_id;
    if (!valid_envelope_shape(envelope)) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_INVALID_STATE;
      items.push_back(item);
      continue;
    }
    if (envelope.profile_id != 1u || envelope.sve1.empty()) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_UNSUPPORTED;
      items.push_back(item);
      continue;
    }
    const SourceInput* original = find_input(envelope.original_source_id);
    const Project& model = project->history.project();
    const AudioSourceRef* original_ref = audio_source(model, envelope.original_source_id);
    const AudioSourceRef* derived_ref = audio_source(model, envelope.derived_source_id);
    if (original == nullptr || original->sample_rate != envelope.source_sample_rate ||
        original->sample_count != envelope.source_sample_count ||
        original->digest != envelope.original_digest || derived_ref == nullptr ||
        original_ref == nullptr ||
        !current_binding_matches(model, *key, envelope.derived_source_id) ||
        !metadata_hash_matches(*derived_ref, envelope.derived_digest)) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
      items.push_back(item);
      continue;
    }
    const auto existing_original = project->audio.sources.find(envelope.original_source_id);
    if (existing_original != project->audio.sources.end() &&
        !store_pcm_matches(*project, envelope.original_source_id, envelope.source_sample_rate,
                           envelope.source_sample_count, envelope.original_digest)) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
      items.push_back(item);
      continue;
    }
    const auto existing = project->audio.sources.find(envelope.derived_source_id);
    if (existing != project->audio.sources.end() &&
        !store_pcm_matches(*project, envelope.derived_source_id, envelope.source_sample_rate,
                           envelope.source_sample_count, envelope.derived_digest)) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
      items.push_back(item);
      continue;
    }

    try {
      sonare::Audio source_audio =
          sonare::Audio::from_buffer(original->mono, static_cast<size_t>(original->sample_count),
                                     static_cast<int>(original->sample_rate));
      auto session =
          sonare::editing::vocal_edit::VocalEditSession::restore(source_audio, envelope.sve1);
      if (!state_matches_envelope(
              sonare::editing::vocal_edit::VocalPersistedState{{},
                                                               0,
                                                               session.token().committed_revision,
                                                               session.source_descriptor(),
                                                               session.output_length_samples(),
                                                               session.analysis(),
                                                               {},
                                                               session.render_settings()},
              envelope)) {
        item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
        item.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
        items.push_back(item);
        continue;
      }
      const auto stage_original = [&]() {
        if (project->audio.sources.find(envelope.original_source_id) ==
                project->audio.sources.end() &&
            staged.find(envelope.original_source_id) == staged.end()) {
          AudioSourceSamples source_content;
          source_content.sample_rate = static_cast<double>(original->sample_rate);
          source_content.channels.emplace_back(
              original->mono, original->mono + static_cast<size_t>(original->sample_count));
          staged.emplace(envelope.original_source_id, std::move(source_content));
        }
      };
      if (existing != project->audio.sources.end()) {
        stage_original();
        item.status = SONARE_VOCAL_REHYDRATE_ALREADY_READY;
        item.reason = SONARE_VOCAL_REASON_NONE;
        items.push_back(item);
        continue;
      }
      const auto snapshot = session.capture_render_snapshot();
      auto rendered = sonare::editing::vocal_edit::render_snapshot(
          snapshot, {{0, envelope.source_sample_count}, 0}, {});
      if (rendered.samples.size() != static_cast<size_t>(envelope.source_sample_count) ||
          !finite_pcm(rendered.samples) ||
          digest_samples(rendered.samples) != envelope.derived_digest) {
        item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
        item.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
        items.push_back(item);
        continue;
      }
      AudioSourceSamples derived_content;
      derived_content.sample_rate = static_cast<double>(envelope.source_sample_rate);
      derived_content.channels.push_back(std::move(rendered.samples));
      const auto [staged_derived, inserted] =
          staged.emplace(envelope.derived_source_id, std::move(derived_content));
      if (!inserted &&
          !audio_content_matches(staged_derived->second, envelope.source_sample_rate,
                                 envelope.source_sample_count, envelope.derived_digest)) {
        item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
        item.reason = SONARE_VOCAL_REASON_SOURCE_MISMATCH;
        items.push_back(item);
        continue;
      }
      stage_original();
      item.status = SONARE_VOCAL_REHYDRATE_REHYDRATED;
      item.reason = SONARE_VOCAL_REASON_NONE;
    } catch (const sonare::editing::vocal_edit::VocalEditException& error) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = state_decode_failure_reason(error);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception&) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_UNSUPPORTED;
    } catch (...) {
      item.status = SONARE_VOCAL_REHYDRATE_UNRESOLVED;
      item.reason = SONARE_VOCAL_REASON_UNSUPPORTED;
    }
    items.push_back(item);
    if (cancel != nullptr && cancel(user_data) != 0) return SONARE_ERROR_CANCELLED;
  }

  auto output = std::unique_ptr<SonareProjectVocalRehydrateItem[]>(
      new SonareProjectVocalRehydrateItem[items.size()]);
  for (size_t i = 0; i < items.size(); ++i) output[i] = items[i];
  std::vector<SourceId> published_source_ids;
  published_source_ids.reserve(staged.size());
  try {
    for (auto& [source_id, content] : staged) {
      if (project->audio.sources.find(source_id) == project->audio.sources.end()) {
        project->audio.sources.emplace(source_id, std::move(content));
        published_source_ids.push_back(source_id);
      }
    }
  } catch (...) {
    for (const SourceId source_id : published_source_ids) {
      project->audio.sources.erase(source_id);
    }
    throw;
  }
  result->items = output.release();
  result->item_count = items.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, originals, original_count, cancel, user_data, result);
#endif
}

void sonare_project_free_vocal_edit_dependencies(SonareProjectVocalEditDependenciesResult* result) {
  if (result == nullptr) return;
  delete[] result->dependencies;
  init_value(result);
}

void sonare_project_free_vocal_rehydrate_result(SonareProjectVocalRehydrateResult* result) {
  if (result == nullptr) return;
  delete[] result->items;
  init_value(result);
}

}  // extern "C"
