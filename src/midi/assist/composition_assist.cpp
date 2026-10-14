/// @file composition_assist.cpp
/// @brief CompositionAssist driver implementation.

#include "midi/assist/composition_assist.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "midi/assist/assist_budget_detail.h"
#include "midi/part_rig.h"
#include "mixing/api/scene.h"

namespace sonare::midi::assist {

namespace {

/// @brief Copies the ProjectView content store into a scratch store.
///
/// Copies every event list and SysEx payload so MIDI commands validate against
/// the real content and never alias the live store.
arrangement::MidiContentStore copy_content_store(const arrangement::ProjectView& view) {
  return view.midi_content();
}

/// @brief Validates a module's command sequence by dry-applying it on cumulative
///        scratch state, so earlier accepted slots are visible to later slots.
///
/// Returns true only when every command applies cleanly. On failure, the scratch
/// state is left unchanged. On success, scratch advances to include the module's
/// patch. The live project is never mutated.
bool commands_apply_cleanly(arrangement::Project* scratch,
                            arrangement::MidiContentStore* scratch_store,
                            const std::vector<arrangement::EditCommandPtr>& commands,
                            const AssistScope& scope);

// Compile-time aggregate field-count probe. The production comparator is
// intentionally hand-written so every playback-relevant value is visible at
// the call site; this guard makes adding a field fail until that comparator is
// revisited.
template <typename Aggregate>
struct AnyField {
  template <typename Field>
  operator Field() const {  // NOLINT(google-explicit-constructor)
    return Field{};
  }
};

template <typename Aggregate, typename Indices, typename = void>
struct BraceInitializableWith : std::false_type {};

template <typename Aggregate, std::size_t... I>
struct BraceInitializableWith<Aggregate, std::index_sequence<I...>,
                              std::void_t<decltype(Aggregate{(void(I), AnyField<Aggregate>{})...})>>
    : std::true_type {};

template <typename Aggregate, std::size_t N = 0>
constexpr std::size_t field_count() {
  static_assert(std::is_aggregate_v<Aggregate>, "field_count requires an aggregate");
  if constexpr (BraceInitializableWith<Aggregate, std::make_index_sequence<N + 1>>::value) {
    return field_count<Aggregate, N + 1>();
  } else {
    return N;
  }
}

bool clip_fade_equal(const arrangement::ClipFade& a, const arrangement::ClipFade& b) {
  return a.length_ppq == b.length_ppq && a.curve == b.curve;
}

bool clip_equal(const arrangement::EditClip& a, const arrangement::EditClip& b) {
  return a.id == b.id && a.track_id == b.track_id && a.source_id == b.source_id &&
         a.start_ppq == b.start_ppq && a.length_ppq == b.length_ppq &&
         a.source_offset_ppq == b.source_offset_ppq && a.gain == b.gain &&
         clip_fade_equal(a.fade_in, b.fade_in) && clip_fade_equal(a.fade_out, b.fade_out) &&
         a.loop_mode == b.loop_mode && a.loop_length_ppq == b.loop_length_ppq &&
         a.loop_crossfade_ppq == b.loop_crossfade_ppq && a.warp_ref_id == b.warp_ref_id &&
         a.warp_mode == b.warp_mode && a.takes == b.takes && a.active_take_id == b.active_take_id &&
         a.comp_segments == b.comp_segments && a.source_offset_seconds == b.source_offset_seconds &&
         a.loop_anchor == b.loop_anchor && a.comp_render_parts == b.comp_render_parts;
}

bool track_equal(const arrangement::Track& a, const arrangement::Track& b) {
  return a.id == b.id && a.name == b.name && a.kind == b.kind && a.gain == b.gain &&
         a.mute == b.mute && a.solo == b.solo && a.pan == b.pan &&
         a.channel_strip_ref == b.channel_strip_ref && a.automation_lanes == b.automation_lanes &&
         a.output_target == b.output_target && a.midi_destination_id == b.midi_destination_id;
}

static_assert(field_count<arrangement::ClipFade>() == 2,
              "ClipFade gained or lost a field: update clip_fade_equal");
static_assert(field_count<arrangement::ClipTake>() == 6,
              "ClipTake gained or lost a field: update its equality operator");
static_assert(field_count<arrangement::ClipCompSegment>() == 4,
              "ClipCompSegment gained or lost a field: update its equality operator");
static_assert(field_count<arrangement::ClipCompRenderPart>() == 8,
              "ClipCompRenderPart gained or lost a field: update its equality operator");
static_assert(field_count<arrangement::LoopAnchor>() == 2,
              "LoopAnchor gained or lost a field: update its equality operator");
static_assert(field_count<arrangement::EditClip>() == 20,
              "EditClip gained or lost a field: update clip_equal");
static_assert(field_count<arrangement::Track>() == 11,
              "Track gained or lost a field: update track_equal");
static_assert(field_count<arrangement::AudioSourceRef>() == 7,
              "AudioSourceRef gained or lost a field: update source_equal");
static_assert(field_count<arrangement::MidiSourceRef>() == 3,
              "MidiSourceRef gained or lost a field: update source_equal");
static_assert(field_count<arrangement::WarpAnchorRef>() == 2,
              "WarpAnchorRef gained or lost a field: update warp equality");
static_assert(field_count<arrangement::WarpMapRef>() == 3,
              "WarpMapRef gained or lost a field: update warp equality");

bool tempo_segment_equal(const transport::TempoSegment& a, const transport::TempoSegment& b) {
  return a.start_ppq == b.start_ppq && a.bpm == b.bpm && a.start_sample == b.start_sample &&
         a.end_bpm == b.end_bpm && a.end_ppq == b.end_ppq;
}

bool time_signature_equal(const transport::TimeSignatureSegment& a,
                          const transport::TimeSignatureSegment& b) {
  return a.start_ppq == b.start_ppq && a.time_sig.numerator == b.time_sig.numerator &&
         a.time_sig.denominator == b.time_sig.denominator &&
         a.clocks_per_metronome_click == b.clocks_per_metronome_click &&
         a.thirty_seconds_per_quarter == b.thirty_seconds_per_quarter;
}

bool marker_equal(const arrangement::ProjectMarker& a, const arrangement::ProjectMarker& b) {
  return a.ppq == b.ppq && a.id == b.id && a.name == b.name && a.kind == b.kind &&
         a.key_fifths == b.key_fifths && a.key_minor == b.key_minor;
}

bool chord_equal(const arrangement::ChordSymbol& a, const arrangement::ChordSymbol& b) {
  return a.start_ppq == b.start_ppq && a.end_ppq == b.end_ppq && a.root_pc == b.root_pc &&
         a.quality == b.quality && a.extensions == b.extensions &&
         a.slash_bass_pc == b.slash_bass_pc && a.roman_numeral == b.roman_numeral &&
         a.modulation_boundary == b.modulation_boundary;
}

bool key_equal(const arrangement::KeySegment& a, const arrangement::KeySegment& b) {
  return a.start_ppq == b.start_ppq && a.end_ppq == b.end_ppq && a.tonic_pc == b.tonic_pc &&
         a.mode == b.mode;
}

bool section_equal(const arrangement::SectionSegment& a, const arrangement::SectionSegment& b) {
  return a.start_ppq == b.start_ppq && a.end_ppq == b.end_ppq && a.label == b.label;
}

bool onset_equal(const arrangement::OnsetMarker& a, const arrangement::OnsetMarker& b) {
  return a.ppq == b.ppq && a.confidence == b.confidence;
}

template <typename T, typename Equal>
bool vector_equal(const std::vector<T>& a, const std::vector<T>& b, Equal equal) {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (!equal(a[index], b[index])) return false;
  }
  return true;
}

bool harmonic_timeline_equal(const arrangement::HarmonicTimeline& a,
                             const arrangement::HarmonicTimeline& b) {
  return vector_equal(a.keys, b.keys, key_equal) && vector_equal(a.chords, b.chords, chord_equal);
}

bool annotation_equal(const arrangement::ProjectAnnotation& a,
                      const arrangement::ProjectAnnotation& b) {
  return harmonic_timeline_equal(a.harmony(), b.harmony()) &&
         a.tempo_confidence == b.tempo_confidence &&
         vector_equal(a.sections, b.sections, section_equal) &&
         vector_equal(a.onsets, b.onsets, onset_equal);
}

bool insert_equal(const mixing::api::Insert& a, const mixing::api::Insert& b) {
  // Insert is constructor-bearing, so the aggregate probe cannot count it.
  // Keep this four-field census aligned with scene_schema_paths() and the
  // serializer key census (slot, processor, params, sidechainKey): slot,
  // processor_name, params_json, and sidechain_key.
  return a.slot == b.slot && a.processor_name == b.processor_name &&
         a.params_json == b.params_json && a.sidechain_key == b.sidechain_key;
}

bool send_equal(const mixing::api::Send& a, const mixing::api::Send& b) {
  return a.id == b.id && a.destination_bus_id == b.destination_bus_id && a.send_db == b.send_db &&
         a.timing == b.timing;
}

bool surround_pan_equal(const mixing::api::SurroundPan& a, const mixing::api::SurroundPan& b) {
  return a.azimuth == b.azimuth && a.elevation == b.elevation && a.divergence == b.divergence &&
         a.lfe == b.lfe && a.distance == b.distance;
}

bool metering_equal(const mixing::api::StripMetering& a, const mixing::api::StripMetering& b) {
  return a.enabled == b.enabled && a.lufs == b.lufs && a.true_peak == b.true_peak &&
         a.true_peak_oversample == b.true_peak_oversample;
}

bool strip_eq_equal(const mixing::api::StripEq& a, const mixing::api::StripEq& b) {
  return a.enabled == b.enabled && a.bands == b.bands;
}

bool strip_equal(const mixing::api::Strip& a, const mixing::api::Strip& b) {
  return a.id == b.id && a.input_trim_db == b.input_trim_db && a.fader_db == b.fader_db &&
         a.vca_offset_db == b.vca_offset_db && a.pan == b.pan && a.width == b.width &&
         a.muted == b.muted && a.soloed == b.soloed && a.solo_safe == b.solo_safe &&
         a.pan_mode == b.pan_mode && a.dual_pan_left == b.dual_pan_left &&
         a.dual_pan_right == b.dual_pan_right && a.polarity_invert_left == b.polarity_invert_left &&
         a.polarity_invert_right == b.polarity_invert_right && a.pan_law == b.pan_law &&
         a.channel_delay_samples == b.channel_delay_samples && a.source_layout == b.source_layout &&
         surround_pan_equal(a.surround_pan, b.surround_pan) &&
         metering_equal(a.metering, b.metering) &&
         vector_equal(a.inserts, b.inserts, insert_equal) &&
         vector_equal(a.sends, b.sends, send_equal) && strip_eq_equal(a.eq, b.eq);
}

bool bus_equal(const mixing::api::Bus& a, const mixing::api::Bus& b) {
  // Bus is constructor-bearing, so the aggregate probe cannot count it. The
  // serializer key census covers id, role, layout, inputTrimDb, width,
  // polarityInvertLeft/Right, pan, panMode, dualPanLeft/Right, panLaw,
  // inserts, and eq: fourteen value fields in this comparator.
  return a.id == b.id && a.role == b.role && a.layout == b.layout &&
         a.input_trim_db == b.input_trim_db && a.width == b.width &&
         a.polarity_invert_left == b.polarity_invert_left &&
         a.polarity_invert_right == b.polarity_invert_right && a.pan == b.pan &&
         a.pan_mode == b.pan_mode && a.dual_pan_left == b.dual_pan_left &&
         a.dual_pan_right == b.dual_pan_right && a.pan_law == b.pan_law &&
         vector_equal(a.inserts, b.inserts, insert_equal) && strip_eq_equal(a.eq, b.eq);
}

bool vca_group_equal(const mixing::api::VcaGroup& a, const mixing::api::VcaGroup& b) {
  return a.id == b.id && a.gain_db == b.gain_db && a.members == b.members;
}

bool connection_equal(const mixing::api::Connection& a, const mixing::api::Connection& b) {
  return a.source == b.source && a.destination == b.destination;
}

bool scene_equal(const mixing::api::Scene& a, const mixing::api::Scene& b) {
  return a.version == b.version && vector_equal(a.strips, b.strips, strip_equal) &&
         vector_equal(a.buses, b.buses, bus_equal) &&
         vector_equal(a.vca_groups, b.vca_groups, vca_group_equal) &&
         vector_equal(a.connections, b.connections, connection_equal);
}

bool part_rig_equal(const arrangement::ProjectPartRig& a, const arrangement::ProjectPartRig& b) {
  if (a.destination_id != b.destination_id || a.part != b.part || a.rig.mode != b.rig.mode ||
      a.rig.stages.size() != b.rig.stages.size()) {
    return false;
  }
  for (std::size_t index = 0; index < a.rig.stages.size(); ++index) {
    if (a.rig.stages[index].processor != b.rig.stages[index].processor ||
        a.rig.stages[index].params_json != b.rig.stages[index].params_json) {
      return false;
    }
  }
  return true;
}

bool project_globals_equal(const arrangement::Project& a, const arrangement::Project& b) {
  return a.project_version() == b.project_version() && a.sample_rate() == b.sample_rate() &&
         a.overlap_policy() == b.overlap_policy() &&
         vector_equal(a.tempo_segments(), b.tempo_segments(), tempo_segment_equal) &&
         vector_equal(a.time_signatures(), b.time_signatures(), time_signature_equal) &&
         vector_equal(a.markers(), b.markers(), marker_equal) &&
         annotation_equal(a.annotation(), b.annotation()) && scene_equal(a.scene(), b.scene()) &&
         vector_equal(a.part_rigs(), b.part_rigs(), part_rig_equal);
}

static_assert(field_count<arrangement::ProjectMarker>() == 6,
              "ProjectMarker gained or lost a field: update marker_equal");
static_assert(field_count<transport::TempoSegment>() == 5,
              "TempoSegment gained or lost a field: update tempo_segment_equal");
static_assert(field_count<transport::TimeSignatureSegment>() == 4,
              "TimeSignatureSegment gained or lost a field: update time_signature_equal");
static_assert(field_count<transport::TimeSignature>() == 2,
              "TimeSignature gained or lost a field: update time_signature_equal");
static_assert(field_count<arrangement::SectionSegment>() == 3,
              "SectionSegment gained or lost a field: update section_equal");
static_assert(field_count<arrangement::OnsetMarker>() == 2,
              "OnsetMarker gained or lost a field: update onset_equal");
static_assert(field_count<arrangement::HarmonicTimeline>() == 2,
              "HarmonicTimeline gained or lost a field: update harmonic_timeline_equal");
static_assert(field_count<arrangement::ProjectAnnotation>() == 4,
              "ProjectAnnotation gained or lost a field: update annotation_equal");
static_assert(field_count<arrangement::ChordSymbol>() == 8,
              "ChordSymbol gained or lost a field: update chord_equal");
static_assert(field_count<arrangement::KeySegment>() == 4,
              "KeySegment gained or lost a field: update key_equal");
static_assert(field_count<mixing::api::Send>() == 4,
              "Send gained or lost a field: update send_equal");
static_assert(field_count<mixing::api::Strip>() == 22,
              "Strip gained or lost a field: update strip_equal");
static_assert(field_count<mixing::api::SurroundPan>() == 5,
              "SurroundPan gained or lost a field: update surround_pan_equal");
static_assert(field_count<mixing::api::StripMetering>() == 4,
              "StripMetering gained or lost a field: update metering_equal");
static_assert(field_count<mixing::api::Scene>() == 5,
              "Scene gained or lost a field: update scene_equal");
static_assert(field_count<mixing::api::StripEq>() == 2,
              "StripEq gained or lost a field: update strip_eq_equal");
static_assert(field_count<mixing::api::VcaGroup>() == 3,
              "VcaGroup gained or lost a field: update vca_group_equal");
static_assert(field_count<mixing::api::Connection>() == 2,
              "Connection gained or lost a field: update connection_equal");
static_assert(field_count<midi::PartRigStage>() == 2,
              "PartRigStage gained or lost a field: update part_rig_equal");
static_assert(field_count<midi::PartRig>() == 2,
              "PartRig gained or lost a field: update part_rig_equal");
static_assert(field_count<arrangement::ProjectPartRig>() == 3,
              "ProjectPartRig gained or lost a field: update part_rig_equal");

bool source_equal(const arrangement::ClipSource& a, const arrangement::ClipSource& b) {
  if (a.index() != b.index()) return false;
  if (const auto* audio = std::get_if<arrangement::AudioSourceRef>(&a)) {
    const auto& other = std::get<arrangement::AudioSourceRef>(b);
    return audio->id == other.id && audio->uri == other.uri &&
           audio->channel_count == other.channel_count &&
           audio->sample_rate_hint == other.sample_rate_hint &&
           audio->storage_handle_id == other.storage_handle_id &&
           audio->content_hash == other.content_hash &&
           audio->external_stem_role == other.external_stem_role;
  }
  const auto& midi = std::get<arrangement::MidiSourceRef>(a);
  const auto& other = std::get<arrangement::MidiSourceRef>(b);
  return midi.id == other.id && midi.name == other.name && midi.channel_hint == other.channel_hint;
}

struct ProtectedDependencies {
  std::vector<arrangement::SourceId> source_ids;
  std::vector<arrangement::WarpRefId> warp_ids;
  std::vector<uint32_t> sysex_handles;

  void normalize() {
    auto normalize_ids = [](auto* ids) {
      std::sort(ids->begin(), ids->end());
      ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    };
    normalize_ids(&source_ids);
    normalize_ids(&warp_ids);
    normalize_ids(&sysex_handles);
  }
};

void collect_outside_dependencies(const arrangement::Project& project,
                                  const arrangement::MidiContentStore& store,
                                  const AssistScope& scope, ProtectedDependencies* out) {
  for (const auto& clip : project.clips()) {
    if (scope.covers_track(clip.track_id)) continue;
    if (clip.source_id != 0) out->source_ids.push_back(clip.source_id);
    for (const auto& take : clip.takes) {
      const arrangement::SourceId source_id = take.source_id == 0 ? clip.source_id : take.source_id;
      if (source_id != 0) out->source_ids.push_back(source_id);
    }
    if (clip.warp_ref_id != 0) out->warp_ids.push_back(clip.warp_ref_id);
    const auto events = store.events.find(clip.id);
    if (events == store.events.end()) continue;
    for (const auto& event : events->second) {
      if (event.sysex_handle != 0) out->sysex_handles.push_back(event.sysex_handle);
    }
  }
}

bool protected_dependencies_equal(const arrangement::Project& before,
                                  const arrangement::MidiContentStore& before_store,
                                  const arrangement::Project& after,
                                  const arrangement::MidiContentStore& after_store,
                                  const ProtectedDependencies& dependencies) {
  for (arrangement::SourceId id : dependencies.source_ids) {
    const auto* before_source = before.find_source(id);
    const auto* after_source = after.find_source(id);
    if ((before_source == nullptr) != (after_source == nullptr)) return false;
    if (before_source != nullptr && !source_equal(*before_source, *after_source)) return false;
  }
  for (arrangement::WarpRefId id : dependencies.warp_ids) {
    const auto* before_warp = before.find_warp_map(id);
    const auto* after_warp = after.find_warp_map(id);
    if ((before_warp == nullptr) != (after_warp == nullptr)) return false;
    if (before_warp != nullptr && *before_warp != *after_warp) return false;
  }
  for (uint32_t handle : dependencies.sysex_handles) {
    const auto before_payload = before_store.sysex_payloads.find(handle);
    const auto after_payload = after_store.sysex_payloads.find(handle);
    if ((before_payload == before_store.sysex_payloads.end()) !=
        (after_payload == after_store.sysex_payloads.end())) {
      return false;
    }
    if (before_payload != before_store.sysex_payloads.end() &&
        before_payload->second != after_payload->second) {
      return false;
    }
  }
  return true;
}

bool outside_clip_events_equal(const arrangement::Project& before,
                               const arrangement::MidiContentStore& before_store,
                               const arrangement::Project& after,
                               const arrangement::MidiContentStore& after_store,
                               const AssistScope& scope) {
  std::vector<arrangement::ClipId> outside_clip_ids;
  outside_clip_ids.reserve(before.clips().size() + after.clips().size());
  for (const auto& clip : before.clips()) {
    if (!scope.covers_track(clip.track_id)) outside_clip_ids.push_back(clip.id);
  }
  for (const auto& clip : after.clips()) {
    if (!scope.covers_track(clip.track_id)) outside_clip_ids.push_back(clip.id);
  }
  std::sort(outside_clip_ids.begin(), outside_clip_ids.end());
  outside_clip_ids.erase(std::unique(outside_clip_ids.begin(), outside_clip_ids.end()),
                         outside_clip_ids.end());
  for (arrangement::ClipId id : outside_clip_ids) {
    const auto before_events = before_store.events.find(id);
    const auto after_events = after_store.events.find(id);
    const bool before_has = before_events != before_store.events.end();
    const bool after_has = after_events != after_store.events.end();
    if (before_has != after_has) return false;
    if (before_has && before_events->second != after_events->second) return false;
  }
  return true;
}

bool has_outside_track(const arrangement::Project& project, const AssistScope& scope) {
  for (const auto& track : project.tracks()) {
    if (!scope.covers_track(track.id)) return true;
  }
  return false;
}

bool command_external_effects_allowed(const arrangement::EditCommand& command,
                                      const ProtectedDependencies& dependencies) {
  auto* rollback = dynamic_cast<const arrangement::EditCommandRollback*>(&command);
  if (rollback == nullptr) return true;
  const auto* effects = dynamic_cast<const arrangement::EditCommandExternalEffects*>(rollback);
  if (effects == nullptr) return false;
  for (arrangement::SourceId source_id : dependencies.source_ids) {
    if (effects->affects_audio_source(source_id)) return false;
  }
  for (arrangement::WarpRefId warp_id : dependencies.warp_ids) {
    if (effects->affects_warp_source(warp_id)) return false;
  }
  return true;
}

class PreviewRollbackGuard final {
 public:
  PreviewRollbackGuard(arrangement::Project& project, arrangement::MidiContentStore& store,
                       std::size_t command_count)
      : project_(project), store_(store) {
    hooks_.reserve(command_count);
  }

  void arm(arrangement::EditCommandRollback* hook) { hooks_.push_back(hook); }

  void note_success() noexcept { successful_hook_apply_ = true; }

  bool has_successful_hook_apply() const noexcept { return successful_hook_apply_; }

  void rollback_now() noexcept {
    if (rolled_back_) return;
    for (auto it = hooks_.rbegin(); it != hooks_.rend(); ++it) {
      if (*it != nullptr) (*it)->rollback_apply(project_, store_);
    }
    rolled_back_ = true;
  }

  ~PreviewRollbackGuard() { rollback_now(); }

 private:
  arrangement::Project& project_;
  arrangement::MidiContentStore& store_;
  std::vector<arrangement::EditCommandRollback*> hooks_;
  bool successful_hook_apply_ = false;
  bool rolled_back_ = false;
};

bool scoped_tracks_unchanged(const arrangement::Project& before,
                             const arrangement::MidiContentStore& before_store,
                             const arrangement::Project& after,
                             const arrangement::MidiContentStore& after_store,
                             const AssistScope& scope) {
  if (scope.track_ids.empty()) return true;

  const bool partial_scope = has_outside_track(before, scope) || has_outside_track(after, scope);

  for (const auto& track : after.tracks()) {
    const arrangement::Track* before_track = before.find_track(track.id);
    if (before_track == nullptr) {
      return false;
    }
    if (!scope.covers_track(track.id) && !track_equal(*before_track, track)) {
      return false;
    }
  }
  for (const auto& track : before.tracks()) {
    if (after.find_track(track.id) == nullptr && !scope.covers_track(track.id)) {
      return false;
    }
  }

  for (const auto& clip : after.clips()) {
    const arrangement::EditClip* before_clip = before.find_clip(clip.id);
    if (before_clip == nullptr) {
      if (!scope.covers_track(clip.track_id)) return false;
      continue;
    }
    const bool before_in_scope = scope.covers_track(before_clip->track_id);
    const bool after_in_scope = scope.covers_track(clip.track_id);
    if (!(before_in_scope && after_in_scope) && !clip_equal(*before_clip, clip)) {
      return false;
    }
  }

  for (const auto& clip : before.clips()) {
    if (scope.covers_track(clip.track_id)) continue;
    if (after.find_clip(clip.id) == nullptr) return false;
  }

  if (!partial_scope) return true;

  ProtectedDependencies dependencies;
  dependencies.source_ids.reserve(before.clips().size() + after.clips().size());
  dependencies.warp_ids.reserve(before.clips().size() + after.clips().size());
  dependencies.sysex_handles.reserve(before_store.events.size() + after_store.events.size());
  collect_outside_dependencies(before, before_store, scope, &dependencies);
  collect_outside_dependencies(after, after_store, scope, &dependencies);
  dependencies.normalize();
  if (!protected_dependencies_equal(before, before_store, after, after_store, dependencies)) {
    return false;
  }
  if (!outside_clip_events_equal(before, before_store, after, after_store, scope)) return false;
  if (!project_globals_equal(before, after)) return false;

  return true;
}

bool commands_apply_cleanly(arrangement::Project* scratch,
                            arrangement::MidiContentStore* scratch_store,
                            const std::vector<arrangement::EditCommandPtr>& commands,
                            const AssistScope& scope) {
  // An empty sequence changes nothing, so the candidate copies would only be swapped back.
  if (commands.empty()) return true;
  try {
    arrangement::Project candidate = *scratch;
    arrangement::MidiContentStore candidate_store = *scratch_store;

    const bool partial_scope = !scope.track_ids.empty() && has_outside_track(*scratch, scope);
    ProtectedDependencies dependencies;
    if (partial_scope) {
      dependencies.source_ids.reserve(scratch->clips().size());
      dependencies.warp_ids.reserve(scratch->clips().size());
      dependencies.sysex_handles.reserve(scratch_store->events.size());
      collect_outside_dependencies(*scratch, *scratch_store, scope, &dependencies);
      dependencies.normalize();
    }

    // The guard sits inside the try so a throwing dry run unwinds its hooks before the catch.
    PreviewRollbackGuard rollback(candidate, candidate_store, commands.size());
    for (const auto& cmd : commands) {
      if (cmd == nullptr) return false;
      if (partial_scope && !command_external_effects_allowed(*cmd, dependencies)) return false;
      auto* hook = dynamic_cast<arrangement::EditCommandRollback*>(cmd.get());
      if (hook != nullptr) rollback.arm(hook);
      if (!cmd->apply(candidate, candidate_store)) return false;
      if (hook != nullptr) rollback.note_success();
    }
    if (!scoped_tracks_unchanged(*scratch, *scratch_store, candidate, candidate_store, scope)) {
      return false;
    }

    if (!rollback.has_successful_hook_apply()) {
      static_assert(noexcept(std::swap(std::declval<arrangement::Project&>(),
                                       std::declval<arrangement::Project&>())),
                    "Project swap must remain noexcept for preview commit");
      static_assert(noexcept(std::swap(std::declval<arrangement::MidiContentStore&>(),
                                       std::declval<arrangement::MidiContentStore&>())),
                    "MIDI store swap must remain noexcept for preview commit");
      std::swap(*scratch, candidate);
      std::swap(*scratch_store, candidate_store);
      return true;
    }

    // Copy the accepted stores before rollback, since a hook may restore either candidate.
    arrangement::Project accepted_project = candidate;
    arrangement::MidiContentStore accepted_store = candidate_store;
    rollback.rollback_now();
    static_assert(noexcept(std::swap(std::declval<arrangement::Project&>(),
                                     std::declval<arrangement::Project&>())),
                  "Project swap must remain noexcept for preview commit");
    static_assert(noexcept(std::swap(std::declval<arrangement::MidiContentStore&>(),
                                     std::declval<arrangement::MidiContentStore&>())),
                  "MIDI store swap must remain noexcept for preview commit");
    std::swap(*scratch, accepted_project);
    std::swap(*scratch_store, accepted_store);
    return true;
  } catch (...) {
    return false;
  }
}

/// @brief Runs one generative slot guarded by the error + budget contract.
///
/// Reported work is recovered before command validation. This keeps a module's
/// actual consumption authoritative even when its patch is later discarded.
/// A non-zero assigned iteration budget is an atomic slot allowance: an
/// overreport consumes the remaining allowance, but its commands and payload
/// are not adopted.
bool contains_reason(const std::string& collected, const std::string& reason) {
  std::size_t start = 0;
  while (start <= collected.size()) {
    const std::size_t end = collected.find('\n', start);
    const std::size_t stop = end == std::string::npos ? collected.size() : end;
    if (collected.compare(start, stop - start, reason) == 0) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

/// @param out_reasons Collects each module's own `diagnostics.reason`, newline
///        joined. Without this a module that comes back empty ON PURPOSE -- a
///        request naming no target clip, a malformed parameter blob -- reports
///        nothing at all to the caller, which is indistinguishable from a module
///        that simply had nothing to add.
/// @param out_module_rejected Set when a module REFUSED the request outright.
/// @param out_module_truncated Set when a module reports its OWN budget
///        truncation. The driver's gate only fires between slots, so a single
///        registered module that stops early has no other way to say so.
template <typename RunFn>
void dispatch_slot(arrangement::Project* scratch, arrangement::MidiContentStore* scratch_store,
                   const AssistScope& scope, uint32_t assigned_iterations, bool has_iteration_cap,
                   RunFn&& run_module, AssistResult* out, bool* discarded, std::string* out_reasons,
                   bool* out_module_truncated, bool* out_module_rejected,
                   bool* out_budget_overrun) {
  *discarded = false;
  AssistResult module_result;
  try {
    module_result = run_module();
  } catch (const std::exception&) {
    *discarded = true;
    return;
  } catch (...) {
    *discarded = true;
    return;
  }

  const uint32_t reported_iterations = module_result.diagnostics.iterations_consumed;
  if (!module_result.diagnostics.reason.empty() &&
      !contains_reason(*out_reasons, module_result.diagnostics.reason)) {
    if (!out_reasons->empty()) out_reasons->push_back('\n');
    *out_reasons += module_result.diagnostics.reason;
  }
  if (module_result.diagnostics.status == AssistStatus::kBudgetTruncated) {
    *out_module_truncated = true;
  }
  if (module_result.diagnostics.status == AssistStatus::kRejected) {
    *out_module_rejected = true;
  }

  if (has_iteration_cap && reported_iterations > assigned_iterations) {
    out->diagnostics.iterations_consumed =
        detail::saturating_add_u32(out->diagnostics.iterations_consumed, assigned_iterations);
    *out_budget_overrun = true;
    *discarded = true;
    return;
  }
  out->diagnostics.iterations_consumed =
      detail::saturating_add_u32(out->diagnostics.iterations_consumed, reported_iterations);

  if (module_result.diagnostics.status == AssistStatus::kDiscarded) {
    *discarded = true;
    return;
  }

  if (!commands_apply_cleanly(scratch, scratch_store, module_result.commands, scope)) {
    *discarded = true;
    return;
  }

  if (module_result.candidate_payloads.empty()) {
    if (!module_result.candidate_payload.empty()) {
      module_result.candidate_payloads.push_back(std::move(module_result.candidate_payload));
    }
  }
  for (auto& payload : module_result.candidate_payloads) {
    if (payload.empty()) continue;
    if (!out->candidate_payload.empty()) out->candidate_payload.push_back('\n');
    out->candidate_payload += payload;
    out->candidate_payloads.push_back(std::move(payload));
  }
  for (auto& cmd : module_result.commands) {
    out->commands.push_back(std::move(cmd));
  }
}

}  // namespace

AssistResult CompositionAssist::run(const arrangement::ProjectView& view,
                                    const AssistRequest& request) const {
  AssistResult result;
  result.diagnostics.status = AssistStatus::kEmpty;
  detail::DeadlineState run_deadline = detail::effective_deadline(request);
  detail::DeadlineScope run_deadline_scope(run_deadline);
  AssistRequest local_request = request;
  const uint32_t requested_max_iterations = request.budget.max_iterations;

  // Collected from the modules rather than composed here: a module's own reason
  // is the only account of WHY it produced nothing, and the driver has none.
  std::string module_reasons;
  bool module_truncated = false;
  bool module_rejected = false;
  bool iteration_budget_exhausted = false;

  // Iteration budget: the driver stops dispatching further slots once the total
  // iterations consumed so far (accumulated across dispatched slots into
  // result.diagnostics.iterations_consumed) reaches the cap. A slot receives
  // only the remaining allowance; a normal return reports its consumption even
  // when its patch is subsequently rejected.
  const bool has_iter_cap = requested_max_iterations != 0;
  bool truncated = false;
  bool any_discarded = false;
  bool any_ran = false;
  arrangement::Project scratch = view.project();
  arrangement::MidiContentStore scratch_store = copy_content_store(view);

  const AssistQueryContext queries{registry_.harmony_context(), registry_.dissonance_analyzer(),
                                   registry_.judge()};

  // A budget check helper: returns false (and marks truncated) when dispatching
  // another slot would exceed either run-wide axis. Checking iterations first
  // fixes the documented tie precedence when both axes expire together.
  const auto budget_ok = [&]() -> bool {
    if (has_iter_cap && result.diagnostics.iterations_consumed >= requested_max_iterations) {
      truncated = true;
      iteration_budget_exhausted = true;
      return false;
    }
    if (detail::deadline_expired()) {
      truncated = true;
      return false;
    }
    return true;
  };

  const auto prepare_slot_request = [&]() -> uint32_t {
    if (has_iter_cap) {
      local_request.budget.max_iterations =
          requested_max_iterations - result.diagnostics.iterations_consumed;
    } else {
      local_request.budget.max_iterations = 0;
    }
    local_request.budget.max_time_ms = detail::remaining_time_ms();
    return local_request.budget.max_iterations;
  };

  const auto note_slot_deadline = [&]() noexcept {
    if (detail::deadline_expired()) {
      truncated = true;
      if (has_iter_cap && result.diagnostics.iterations_consumed >= requested_max_iterations) {
        iteration_budget_exhausted = true;
      }
    }
  };

  // `remaining_time_ms() == 0` means either an unlimited run or an already
  // exhausted active deadline. Check the active clock state again after
  // calculating each slot's allowance so a boundary crossing between the
  // pre-slot gate and allowance calculation cannot dispatch a module with an
  // accidental zero/unlimited request.
  const auto slot_ready = [&]() noexcept {
    if (!detail::deadline_expired()) return true;
    truncated = true;
    return false;
  };

  // Fixed dispatch order: generator -> counterpoint -> rhythm. Query slots
  // (harmony/dissonance/judge) are consulted by modules, not by the driver.
  if (auto* gen = registry_.generator(); gen != nullptr && budget_ok()) {
    const uint32_t assigned_iterations = prepare_slot_request();
    if (slot_ready()) {
      any_ran = true;
      bool discarded = false;
      dispatch_slot(
          &scratch, &scratch_store, local_request.scope, assigned_iterations, has_iter_cap,
          [&]() { return gen->generate(view, local_request); }, &result, &discarded,
          &module_reasons, &module_truncated, &module_rejected, &iteration_budget_exhausted);
      truncated = truncated || iteration_budget_exhausted;
      any_discarded = any_discarded || discarded;
      if (discarded) ++result.diagnostics.slots_discarded;
      note_slot_deadline();
    }
  }

  if (auto* cp = registry_.counterpoint(); cp != nullptr && budget_ok()) {
    const uint32_t assigned_iterations = prepare_slot_request();
    if (slot_ready()) {
      any_ran = true;
      bool discarded = false;
      dispatch_slot(
          &scratch, &scratch_store, local_request.scope, assigned_iterations, has_iter_cap,
          [&]() { return cp->derive(view, local_request, {}, queries); }, &result, &discarded,
          &module_reasons, &module_truncated, &module_rejected, &iteration_budget_exhausted);
      truncated = truncated || iteration_budget_exhausted;
      any_discarded = any_discarded || discarded;
      if (discarded) ++result.diagnostics.slots_discarded;
      note_slot_deadline();
    }
  }

  if (auto* rhythm = registry_.rhythm(); rhythm != nullptr && budget_ok()) {
    const uint32_t assigned_iterations = prepare_slot_request();
    if (slot_ready()) {
      any_ran = true;
      bool discarded = false;
      dispatch_slot(
          &scratch, &scratch_store, local_request.scope, assigned_iterations, has_iter_cap,
          [&]() { return rhythm->generate(view, local_request); }, &result, &discarded,
          &module_reasons, &module_truncated, &module_rejected, &iteration_budget_exhausted);
      truncated = truncated || iteration_budget_exhausted;
      any_discarded = any_discarded || discarded;
      if (discarded) ++result.diagnostics.slots_discarded;
      note_slot_deadline();
    }
  }

  // Compose the final status from what happened.
  if (module_rejected) {
    // Ahead of every other branch, and NOT conditioned on the run being empty.
    // A refusal is a verdict on the REQUEST, so one module refusing it settles
    // the whole run: with the `commands.empty()` guard this branch was
    // unreachable for the built-ins (they share one params reader, so a params
    // fault fells all of them) and became reachable the moment a host installed
    // a module of its own -- at which point a faulty request could have come
    // back "ok" with rc 0 and the refusal buried in the reason, which is the
    // same indistinguishability this status exists to end.
    //
    // The commands collected so far are deliberately NOT applied. Nothing is
    // lost by that: preview and apply are separate calls, so a caller fixes the
    // request and runs again.
    result.diagnostics.status = AssistStatus::kRejected;
    result.diagnostics.reason = module_reasons;
    result.commands.clear();
  } else if (truncated) {
    result.diagnostics.status = AssistStatus::kBudgetTruncated;
    result.diagnostics.reason =
        iteration_budget_exhausted ? "iteration budget exhausted" : "time budget exhausted";
  } else if (module_truncated) {
    // The gate above only fires when the driver tries to dispatch the NEXT slot,
    // so a lone module that stopped early is invisible to it. Its own report is
    // the only evidence, and dropping it made a truncated run read as complete.
    result.diagnostics.status = AssistStatus::kBudgetTruncated;
    result.diagnostics.reason =
        module_reasons.empty() ? "module stopped on its own budget" : module_reasons;
  } else if (any_discarded) {
    result.diagnostics.status = AssistStatus::kDiscarded;
    result.diagnostics.reason = "module threw or returned an invalid patch";
  } else if (!any_ran || result.commands.empty()) {
    result.diagnostics.status = AssistStatus::kEmpty;
    result.diagnostics.reason = module_reasons;
  } else {
    result.diagnostics.status = AssistStatus::kOk;
    result.diagnostics.reason = module_reasons;
  }

  return result;
}

}  // namespace sonare::midi::assist
