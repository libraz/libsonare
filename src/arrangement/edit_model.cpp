/// @file edit_model.cpp
/// @brief Implementation of the headless arrangement project model.

#include "arrangement/edit_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

#include "arrangement/edit_command_internal.h"
#include "c_api/instrument_config_domains.h"
#include "c_api/midi_input_domains.h"
#include "midi/builtin_synth.h"
#include "midi/synth/mod_matrix.h"
#include "util/exception.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif

namespace sonare::arrangement {

namespace {

template <typename Id>
Id allocate_entity_id(Id& next) noexcept {
  static_assert(std::is_same_v<Id, uint32_t>);
  // 0 is the public failure/sentinel value and UINT32_MAX is reserved as the
  // exhausted-counter marker. Never increment either value.
  if (next == 0 || next == std::numeric_limits<Id>::max()) return 0;
  return next++;
}

template <typename Id>
void ensure_next_entity_id(Id id, Id& next) noexcept {
  const Id reserved = std::numeric_limits<Id>::max();
  if (id >= next) next = id >= reserved - 1 ? reserved : static_cast<Id>(id + 1);
}

}  // namespace

const mixing::api::Scene& Project::scene() const noexcept {
  static const mixing::api::Scene kDefaultScene;
  return scene_ ? *scene_ : kDefaultScene;
}

bool valid_comp_render_geometry(const std::vector<ClipCompRenderPart>& parts,
                                double clip_length_ppq) noexcept {
  if (!(std::isfinite(clip_length_ppq) && clip_length_ppq > 0.0)) return false;
  double previous_reference_start = 0.0;
  double previous_reference_end = 0.0;
  double covered_until = 0.0;
  bool have_previous_reference = false;
  bool have_active = false;
  for (const ClipCompRenderPart& part : parts) {
    if (!std::isfinite(part.visible_start_ppq) || !std::isfinite(part.visible_end_ppq) ||
        !std::isfinite(part.reference_start_ppq) || !std::isfinite(part.reference_end_ppq) ||
        !(part.reference_end_ppq > part.reference_start_ppq) ||
        !std::isfinite(part.seam_fade_in_ppq) || part.seam_fade_in_ppq < 0.0 ||
        !std::isfinite(part.seam_fade_out_ppq) || part.seam_fade_out_ppq < 0.0 ||
        part.seam_fade_in_ppq > part.reference_end_ppq - part.reference_start_ppq ||
        part.seam_fade_out_ppq > part.reference_end_ppq - part.reference_start_ppq) {
      return false;
    }
    if (have_previous_reference && (part.reference_start_ppq < previous_reference_start ||
                                    part.reference_end_ppq < previous_reference_end)) {
      return false;
    }
    if (part.retained_only) {
      if (part.visible_start_ppq != 0.0 || part.visible_end_ppq != 0.0 ||
          !(part.reference_end_ppq <= 0.0 || part.reference_start_ppq >= clip_length_ppq)) {
        return false;
      }
    } else {
      if (part.visible_start_ppq < 0.0 || !(part.visible_end_ppq > part.visible_start_ppq) ||
          part.visible_end_ppq > clip_length_ppq) {
        return false;
      }
      const double expected_start = std::max(0.0, part.reference_start_ppq);
      const double expected_end = std::min(clip_length_ppq, part.reference_end_ppq);
      if (!(expected_end > expected_start) ||
          !ppq_nearly_equal(part.visible_start_ppq, expected_start) ||
          !ppq_nearly_equal(part.visible_end_ppq, expected_end)) {
        return false;
      }
      if (part.visible_start_ppq > covered_until &&
          !ppq_nearly_equal(part.visible_start_ppq, covered_until)) {
        return false;
      }
      covered_until = std::max(covered_until, part.visible_end_ppq);
      have_active = true;
    }
    previous_reference_start = part.reference_start_ppq;
    previous_reference_end = part.reference_end_ppq;
    have_previous_reference = true;
  }
  return parts.empty() || (have_active && ppq_nearly_equal(covered_until, clip_length_ppq) &&
                           covered_until <= clip_length_ppq);
}

bool valid_clip_render_parts(const EditClip& clip) noexcept {
  if (clip.comp_render_parts.empty()) return true;
  if (!valid_comp_render_geometry(clip.comp_render_parts, clip.length_ppq)) return false;
  const bool takes_exist = std::all_of(
      clip.comp_render_parts.begin(), clip.comp_render_parts.end(), [&clip](const auto& part) {
        return part.take_id == 0 ||
               std::any_of(clip.takes.begin(), clip.takes.end(),
                           [&part](const ClipTake& take) { return take.id == part.take_id; });
      });
  return takes_exist &&
         (clip.loop_mode != LoopMode::kLoop || is_trivial_loop_comp_render_parts(clip));
}

SourceId Project::add_audio_source(AudioSourceRef ref) {
  const SourceId id = allocate_entity_id(next_source_id_);
  if (id == 0) return 0;
  ref.id = id;
  sources_.emplace_back(std::move(ref));
  return id;
}

SourceId Project::add_midi_source(MidiSourceRef ref) {
  const SourceId id = allocate_entity_id(next_source_id_);
  if (id == 0) return 0;
  ref.id = id;
  sources_.emplace_back(std::move(ref));
  return id;
}

const ClipSource* Project::find_source(SourceId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const ClipSource& s : sources_) {
    if (source_id(s) == id) {
      return &s;
    }
  }
  return nullptr;
}

TrackId Project::add_track(Track track) {
  uint32_t max_lane_id = 0;
  for (size_t i = 0; i < track.automation_lanes.size(); ++i) {
    const uint32_t lane_id = track.automation_lanes[i].lane_id();
    if (lane_id == std::numeric_limits<uint32_t>::max()) return 0;
    if (lane_id == 0) continue;
    for (const Track& existing_track : tracks_) {
      for (const automation::AutomationLane& existing_lane : existing_track.automation_lanes) {
        if (existing_lane.lane_id() == lane_id) return 0;
      }
    }
    for (size_t previous = 0; previous < i; ++previous) {
      if (track.automation_lanes[previous].lane_id() == lane_id) return 0;
    }
    max_lane_id = std::max(max_lane_id, lane_id);
  }
  if (max_lane_id != 0) ensure_next_automation_lane_id(max_lane_id);
  for (automation::AutomationLane& lane : track.automation_lanes) {
    if (lane.lane_id() == 0) {
      const uint32_t lane_id = allocate_automation_lane_id();
      if (lane_id == 0) return 0;
      lane.set_lane_id(lane_id);
    }
  }

  const TrackId id = allocate_entity_id(next_track_id_);
  if (id == 0) return 0;
  track.id = id;
  tracks_.emplace_back(std::move(track));
  return id;
}

const Track* Project::find_track(TrackId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const Track& t : tracks_) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

Track* Project::find_track_mutable(TrackId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (Track& t : tracks_) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

bool Project::clip_overlaps(TrackId track_id, double start_ppq, double length_ppq,
                            ClipId ignore_clip_id) const noexcept {
  const double end_ppq = start_ppq + length_ppq;
  for (const EditClip& c : clips_) {
    if (c.track_id != track_id || c.id == ignore_clip_id) {
      continue;
    }
    // Half-open interval overlap; touching endpoints (adjacency) do not count.
    if (start_ppq < c.end_ppq() && c.start_ppq < end_ppq) {
      return true;
    }
  }
  return false;
}

ClipId Project::add_clip(EditClip clip) {
  // Referential integrity.
  if (!has_track(clip.track_id) || !has_source(clip.source_id)) {
    return 0;
  }
  // PPQ range validation.
  if (!numeric::finite_positive(clip.length_ppq) || !numeric::finite_non_negative(clip.start_ppq) ||
      !numeric::finite_non_negative(clip.source_offset_ppq) ||
      (clip.source_offset_seconds.has_value() &&
       !(std::isfinite(*clip.source_offset_seconds) && *clip.source_offset_seconds >= 0.0))) {
    return 0;
  }
  // Loop length is finite in every mode; under LOOP it is non-negative and 0 loops the whole clip.
  if (!numeric::finite(clip.loop_length_ppq) ||
      (clip.loop_mode == LoopMode::kLoop && !numeric::finite_non_negative(clip.loop_length_ppq))) {
    return 0;
  }
  if (clip.loop_anchor.has_value()) {
    const LoopAnchor& anchor = *clip.loop_anchor;
    if (!(std::isfinite(anchor.period_seconds) && anchor.period_seconds > 0.0 &&
          std::isfinite(anchor.phase_seconds))) {
      return 0;
    }
  }
  // Take and comp structure follow the same rule the edit commands and the loader apply.
  if (!detail::valid_clip_take_structure(*this, clip, clip.takes, clip.active_take_id) ||
      !detail::valid_comp_segments(clip.takes, clip.comp_segments, clip.length_ppq) ||
      (detail::clip_is_on_midi_track(*this, clip) && !clip.comp_segments.empty()) ||
      (clip.loop_mode == LoopMode::kLoop &&
       detail::comp_segments_split_clip(clip.comp_segments, clip.length_ppq))) {
    return 0;
  }
  if (!valid_clip_render_parts(clip)) return 0;
  // Overlap policy.
  if (overlap_policy_ == OverlapPolicy::kDisallow &&
      clip_overlaps(clip.track_id, clip.start_ppq, clip.length_ppq)) {
    return 0;
  }

  const ClipId id = allocate_entity_id(next_clip_id_);
  if (id == 0) return 0;
  clip.id = id;
  clips_.emplace_back(std::move(clip));
  return id;
}

const EditClip* Project::find_clip(ClipId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const EditClip& c : clips_) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

EditClip* Project::find_clip_mutable(ClipId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (EditClip& c : clips_) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

const WarpMapRef* Project::find_warp_map(WarpRefId id) const noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (const WarpMapRef& map : warp_maps_) {
    if (map.id == id) {
      return &map;
    }
  }
  return nullptr;
}

WarpMapRef* Project::find_warp_map_mutable(WarpRefId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (WarpMapRef& map : warp_maps_) {
    if (map.id == id) {
      return &map;
    }
  }
  return nullptr;
}

bool Project::set_warp_map(WarpMapRef map) {
  if (map.id == 0 || map.anchors.size() < 2) {
    return false;
  }
  // Anchors must define a well-formed forward mapping: each offset finite and
  // non-negative, and strictly increasing in both the warp and source axes. This
  // is the interpolation contract the C ABI enforces at its boundary; applying it
  // in the setter means every insertion path (edit commands and JSON project
  // load) shares it, so a hand-authored or corrupt project file can no longer
  // install a warp map that plays back mistimed or garbled.
  const WarpAnchorRef* prev = nullptr;
  for (const WarpAnchorRef& a : map.anchors) {
    if (!(std::isfinite(a.warp_sample) && a.warp_sample >= 0.0) ||
        !(std::isfinite(a.source_sample) && a.source_sample >= 0.0)) {
      return false;
    }
    if (prev != nullptr &&
        !(a.warp_sample > prev->warp_sample && a.source_sample > prev->source_sample)) {
      return false;
    }
    prev = &a;
  }
  if (WarpMapRef* existing = find_warp_map_mutable(map.id)) {
    *existing = std::move(map);
    return true;
  }
  warp_maps_.emplace_back(std::move(map));
  return true;
}

std::pair<WarpMapRef, bool> Project::remove_warp_map(WarpRefId id) {
  for (auto it = warp_maps_.begin(); it != warp_maps_.end(); ++it) {
    if (it->id == id) {
      WarpMapRef removed = std::move(*it);
      warp_maps_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {WarpMapRef{}, false};
}

namespace {

// part_rigs_ is kept sorted by (destination_id, part); returns the first entry not below the key.
template <typename Rigs>
auto part_rig_lower_bound(Rigs& rigs, uint32_t destination_id, uint8_t part) noexcept {
  return std::lower_bound(rigs.begin(), rigs.end(), std::pair(destination_id, part),
                          [](const ProjectPartRig& entry, const std::pair<uint32_t, uint8_t>& key) {
                            return std::pair(entry.destination_id, entry.part) < key;
                          });
}

}  // namespace

const ProjectPartRig* Project::find_part_rig(uint32_t destination_id, uint8_t part) const noexcept {
  const auto it = part_rig_lower_bound(part_rigs_, destination_id, part);
  if (it == part_rigs_.end() || it->destination_id != destination_id || it->part != part) {
    return nullptr;
  }
  return &*it;
}

const DestinationInstrumentBinding* Project::find_destination_instrument(
    uint32_t destination_id) const noexcept {
  const auto it = destination_instruments_.find(destination_id);
  return it == destination_instruments_.end() ? nullptr : &it->second;
}

void validate_destination_instrument(const DestinationInstrumentBinding& binding) {
  namespace check = sonare::util::check;
  namespace domains = sonare_c_detail;
  using sonare::midi::synth::ModDestination;
  using sonare::midi::synth::ModSource;
  constexpr auto builtin_waveform_domain =
      check::integer_domain(static_cast<int>(sonare::midi::SynthWaveform::kSine),
                            static_cast<int>(sonare::midi::SynthWaveform::kTriangle));
  constexpr auto mod_source_domain = check::integer_domain(static_cast<int>(ModSource::kAmpEnv),
                                                           static_cast<int>(ModSource::kPitchBend));
  constexpr auto mod_destination_domain =
      check::integer_domain(static_cast<int>(ModDestination::kPitchCents),
                            static_cast<int>(ModDestination::kSpectrumMorph));

  if (binding.bank.has_value()) {
    check::integer_within(binding.bank->msb, domains::kMidiDataByteDomain, "bank.msb");
    check::integer_within(binding.bank->lsb, domains::kMidiDataByteDomain, "bank.lsb");
  }
  if (binding.program.has_value()) {
    check::integer_within(*binding.program, domains::kMidiDataByteDomain, "program");
  }

  if (const auto* builtin = std::get_if<DestinationBuiltinInstrument>(&binding.instrument)) {
    check::integer_within(builtin->waveform, builtin_waveform_domain, "builtin.waveform");
    check::within(builtin->gain, domains::kBuiltinSynthGainDomain, "builtin.gain");
    check::within(builtin->attack_ms, domains::kBuiltinSynthStageMsDomain, "builtin.attack_ms");
    check::within(builtin->decay_ms, domains::kBuiltinSynthStageMsDomain, "builtin.decay_ms");
    check::within(builtin->release_ms, domains::kBuiltinSynthStageMsDomain, "builtin.release_ms");
    check::within(builtin->sustain, domains::kBuiltinSynthSustainDomain, "builtin.sustain");
    check::integer_within(builtin->polyphony, domains::kSynthPolyphonyDomain, "builtin.polyphony");
  } else if (const auto* sf2 = std::get_if<DestinationSf2Instrument>(&binding.instrument)) {
    check::within(sf2->gain, domains::kSf2PlayerGainDomain, "sf2.gain");
    check::integer_within(sf2->polyphony, domains::kSynthPolyphonyDomain, "sf2.polyphony");
    check::integer_within(sf2->prefer_model_for_modeled_families, domains::kSf2PlayerFlagDomain,
                          "sf2.prefer_model_for_modeled_families");
    check::integer_within(sf2->clear_bank_rig, domains::kSf2PlayerFlagDomain, "sf2.clear_bank_rig");
    check::integer_within(sf2->gs_efx_realization, domains::kGsEfxRealizationDomain,
                          "sf2.gs_efx_realization");
    if (sf2->sound_font_uri.has_value() && sf2->sound_font_uri->empty()) {
      check::refuse("sf2.sound_font_uri", "must not be empty");
    }
  } else if (const auto* synth = std::get_if<DestinationSynthInstrument>(&binding.instrument)) {
    check::integer_within(synth->mod_routings.size(), domains::kSynthModRoutingCountDomain,
                          "synth.mod_routings");
    const float depth_limit = domains::synth_patch_mod_depth_limit();
    for (std::size_t i = 0; i < synth->mod_routings.size(); ++i) {
      const auto& route = synth->mod_routings[i];
      const std::string prefix = "synth.mod_routings[" + std::to_string(i) + "].";
      check::integer_within(route.source, mod_source_domain, (prefix + "source").c_str());
      check::integer_within(route.destination, mod_destination_domain,
                            (prefix + "destination").c_str());
      check::in_computed_range(route.depth, -depth_limit, depth_limit, (prefix + "depth").c_str());
    }
  }
}

void Project::set_destination_instrument(uint32_t destination_id,
                                         DestinationInstrumentBinding binding) {
  validate_destination_instrument(binding);
  const bool is_referenced = std::any_of(
      tracks_.begin(), tracks_.end(),
      [destination_id](const Track& track) { return track.midi_destination_id == destination_id; });
  if (!is_referenced) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "destination_id",
                                            "must be referenced by at least one track")
        .as_caller_spelled();
  }
  destination_instruments_.insert_or_assign(destination_id, std::move(binding));
}

bool Project::remove_destination_instrument(uint32_t destination_id) {
  return destination_instruments_.erase(destination_id) != 0;
}

void Project::restore_destination_instrument(uint32_t destination_id,
                                             std::optional<DestinationInstrumentBinding> binding) {
  if (binding.has_value()) {
    validate_destination_instrument(*binding);
    destination_instruments_.insert_or_assign(destination_id, std::move(*binding));
  } else {
    (void)remove_destination_instrument(destination_id);
  }
}

namespace {

[[noreturn]] void refuse_part_rig(const std::string& subject, const std::string& predicate,
                                  ErrorCode code = ErrorCode::InvalidParameter) {
  throw SonareParameterRefusal::predicate(code, subject, predicate).as_caller_spelled();
}

// The refusals midi::validate_part_rig answers with false, each naming its field.
void refuse_malformed_part_rig(uint8_t part, const midi::PartRig& rig) {
  namespace schema = util::json::schema;
  if (part >= 16 && part != midi::kPartRigAllParts) {
    refuse_part_rig("part", "must be 0 to 15, or 255 for the destination default");
  }
  if (rig.mode != midi::PartRigMode::kChain) {
    if (!rig.stages.empty()) refuse_part_rig("inserts", "must be empty unless mode is chain");
    return;
  }
  if (rig.stages.empty() || rig.stages.size() > midi::kMaxPartRigStages) {
    refuse_part_rig("inserts", "must hold 1 to " + std::to_string(midi::kMaxPartRigStages) +
                                   " entries for a chain rig");
  }
  for (std::size_t i = 0; i < rig.stages.size(); ++i) {
    if (rig.stages[i].processor.empty()) {
      refuse_part_rig(schema::child_path(schema::index_path("inserts", i), "processor"),
                      "must not be empty");
    }
  }
}

}  // namespace

void Project::set_part_rig(ProjectPartRig entry) {
  refuse_malformed_part_rig(entry.part, entry.rig);
  if (entry.rig.mode == midi::PartRigMode::kChain) {
#if defined(SONARE_WITH_MASTERING)
    mastering::api::validate_part_rig_chain(entry.rig.stages);
#else
    refuse_part_rig("mode", "must not be chain in a build without insert chains",
                    ErrorCode::NotSupported);
#endif
  }
  auto it = part_rig_lower_bound(part_rigs_, entry.destination_id, entry.part);
  if (it != part_rigs_.end() && it->destination_id == entry.destination_id &&
      it->part == entry.part) {
    *it = std::move(entry);
  } else {
    part_rigs_.insert(it, std::move(entry));
  }
}

bool Project::remove_part_rig(uint32_t destination_id, uint8_t part) {
  const auto it = part_rig_lower_bound(part_rigs_, destination_id, part);
  if (it == part_rigs_.end() || it->destination_id != destination_id || it->part != part) {
    return false;
  }
  part_rigs_.erase(it);
  return true;
}

uint32_t Project::add_marker(double ppq, std::string name, uint8_t kind, int8_t key_fifths,
                             bool key_minor) {
  const uint32_t id = allocate_entity_id(next_marker_id_);
  if (id == 0) return 0;
  ProjectMarker marker;
  marker.ppq = ppq;
  marker.id = id;
  marker.name = std::move(name);
  marker.kind = kind;
  marker.key_fifths = key_fifths;
  marker.key_minor = key_minor;
  markers_.emplace_back(std::move(marker));
  return id;
}

uint32_t Project::allocate_automation_lane_id() noexcept {
  return allocate_entity_id(next_automation_lane_id_);
}

std::pair<Track, bool> Project::remove_track(TrackId id) {
  for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
    if (it->id == id) {
      Track removed = std::move(*it);
      tracks_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {Track{}, false};
}

std::pair<EditClip, bool> Project::remove_clip(ClipId id) {
  // Moving out is safe only because the erase that follows cannot throw.
  static_assert(std::is_nothrow_move_assignable_v<EditClip>);
  for (auto it = clips_.begin(); it != clips_.end(); ++it) {
    if (it->id == id) {
      EditClip removed = std::move(*it);
      clips_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {EditClip{}, false};
}

bool Project::insert_clip_raw(EditClip clip, size_t index, bool known_unique) {
  if (clip.id == 0 || clip.id == std::numeric_limits<ClipId>::max() ||
      (!known_unique && has_clip(clip.id))) {
    return false;
  }
  if (index >= clips_.size()) {
    clips_.emplace_back(std::move(clip));
  } else {
    clips_.insert(clips_.begin() + static_cast<std::ptrdiff_t>(index), std::move(clip));
  }
  return true;
}

bool Project::insert_track_raw(Track track, size_t index, bool known_unique) {
  if (track.id == 0 || track.id == std::numeric_limits<TrackId>::max() ||
      (!known_unique && has_track(track.id))) {
    return false;
  }
  const size_t insertion_index = index >= tracks_.size() ? tracks_.size() : index;
  if (insertion_index == tracks_.size()) {
    tracks_.emplace_back(std::move(track));
  } else {
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                   std::move(track));
  }
  for (const automation::AutomationLane& lane : tracks_[insertion_index].automation_lanes) {
    if (lane.lane_id() != 0) ensure_next_automation_lane_id(lane.lane_id());
  }
  return true;
}

ClipSource* Project::find_source_mutable(SourceId id) noexcept {
  if (id == 0) {
    return nullptr;
  }
  for (ClipSource& s : sources_) {
    if (source_id(s) == id) {
      return &s;
    }
  }
  return nullptr;
}

bool Project::insert_source_raw(ClipSource source, size_t index, bool known_unique) {
  const SourceId id = source_id(source);
  if (id == 0 || id == std::numeric_limits<SourceId>::max() || (!known_unique && has_source(id))) {
    return false;
  }
  if (index >= sources_.size()) {
    sources_.emplace_back(std::move(source));
  } else {
    sources_.insert(sources_.begin() + static_cast<std::ptrdiff_t>(index), std::move(source));
  }
  return true;
}

size_t Project::track_index(TrackId id) const noexcept {
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (tracks_[i].id == id) {
      return i;
    }
  }
  return kAppend;
}

size_t Project::clip_index(ClipId id) const noexcept {
  for (size_t i = 0; i < clips_.size(); ++i) {
    if (clips_[i].id == id) {
      return i;
    }
  }
  return kAppend;
}

size_t Project::source_index(SourceId id) const noexcept {
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (source_id(sources_[i]) == id) {
      return i;
    }
  }
  return kAppend;
}

std::pair<ClipSource, bool> Project::remove_source(SourceId id) {
  for (auto it = sources_.begin(); it != sources_.end(); ++it) {
    if (source_id(*it) == id) {
      ClipSource removed = std::move(*it);
      sources_.erase(it);
      return {std::move(removed), true};
    }
  }
  return {ClipSource{}, false};
}

Project Project::snapshot_members(ProjectMemberMask mask) const {
  using namespace project_member;
  Project copy;
  if (mask & kScalars) {
    copy.sample_rate_ = sample_rate_;
    copy.project_version_ = project_version_;
    copy.overlap_policy_ = overlap_policy_;
  }
  if (mask & kTempo) copy.tempo_segments_ = tempo_segments_;
  if (mask & kTimeSignatures) copy.time_signatures_ = time_signatures_;
  if (mask & kSources) {
    copy.sources_ = sources_;
    copy.next_source_id_ = next_source_id_;
  }
  if (mask & kTracks) {
    copy.tracks_ = tracks_;
    copy.next_track_id_ = next_track_id_;
    copy.next_automation_lane_id_ = next_automation_lane_id_;
  }
  if (mask & kClips) {
    copy.clips_ = clips_;
    copy.next_clip_id_ = next_clip_id_;
  }
  if (mask & kMarkers) {
    copy.markers_ = markers_;
    copy.next_marker_id_ = next_marker_id_;
  }
  if (mask & kAnnotation) copy.annotation_ = annotation_;
  if (mask & kScene) copy.scene_ = scene_;
  if (mask & kAssistSidecars) copy.assist_sidecars_ = assist_sidecars_;
  if (mask & kWarpMaps) copy.warp_maps_ = warp_maps_;
  if (mask & kPartRigs) copy.part_rigs_ = part_rigs_;
  if (mask & kDestinationInstruments) copy.destination_instruments_ = destination_instruments_;
  return copy;
}

void Project::swap_members(Project& other, ProjectMemberMask mask) noexcept {
  using namespace project_member;
  using std::swap;
  if (mask & kScalars) {
    swap(sample_rate_, other.sample_rate_);
    swap(project_version_, other.project_version_);
    swap(overlap_policy_, other.overlap_policy_);
  }
  if (mask & kTempo) swap(tempo_segments_, other.tempo_segments_);
  if (mask & kTimeSignatures) swap(time_signatures_, other.time_signatures_);
  if (mask & kSources) {
    swap(sources_, other.sources_);
    swap(next_source_id_, other.next_source_id_);
  }
  if (mask & kTracks) {
    swap(tracks_, other.tracks_);
    swap(next_track_id_, other.next_track_id_);
    swap(next_automation_lane_id_, other.next_automation_lane_id_);
  }
  if (mask & kClips) {
    swap(clips_, other.clips_);
    swap(next_clip_id_, other.next_clip_id_);
  }
  if (mask & kMarkers) {
    swap(markers_, other.markers_);
    swap(next_marker_id_, other.next_marker_id_);
  }
  if (mask & kAnnotation) swap(annotation_, other.annotation_);
  if (mask & kScene) swap(scene_, other.scene_);
  if (mask & kAssistSidecars) swap(assist_sidecars_, other.assist_sidecars_);
  if (mask & kWarpMaps) swap(warp_maps_, other.warp_maps_);
  if (mask & kPartRigs) swap(part_rigs_, other.part_rigs_);
  if (mask & kDestinationInstruments)
    swap(destination_instruments_, other.destination_instruments_);
}

void Project::ensure_next_source_id(SourceId id) noexcept {
  ensure_next_entity_id(id, next_source_id_);
}

void Project::ensure_next_track_id(TrackId id) noexcept {
  ensure_next_entity_id(id, next_track_id_);
}

void Project::ensure_next_clip_id(ClipId id) noexcept { ensure_next_entity_id(id, next_clip_id_); }

void Project::ensure_next_marker_id(uint32_t id) noexcept {
  ensure_next_entity_id(id, next_marker_id_);
}

void Project::ensure_next_automation_lane_id(uint32_t id) noexcept {
  ensure_next_entity_id(id, next_automation_lane_id_);
}

}  // namespace sonare::arrangement
