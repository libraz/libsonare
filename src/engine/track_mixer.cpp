#include "engine/track_mixer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "engine/track_mixer_internal.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mixing/channel_strip_eq.h"
#include "mixing/pan_law.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/json.h"

namespace sonare::engine {

using sonare::constants::kFloorDb;

std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec) {
  auto strip = std::make_unique<mixing::ChannelStrip>(
      mixing::ChannelStripConfig{spec.fader_db, spec.pan, mixing::pan_law_from_index(spec.pan_law),
                                 5.0f, mixing::EqPosition::PreFader, spec.input_trim_db, false});
  strip->set_vca_offset_db(spec.vca_offset_db);
  strip->set_width(spec.width);
  strip->set_muted(spec.muted);
  strip->set_soloed(spec.soloed);
  strip->set_solo_safe(spec.solo_safe);
  strip->set_pan_mode(to_pan_mode(spec.pan_mode));
  strip->set_dual_pan(spec.dual_pan_left, spec.dual_pan_right);
  strip->set_polarity_invert(spec.polarity_invert_left, spec.polarity_invert_right);
  strip->set_channel_delay_samples(spec.channel_delay_samples);
  strip->set_surround_pan_params({spec.surround_pan.azimuth, spec.surround_pan.elevation,
                                  spec.surround_pan.divergence, spec.surround_pan.lfe,
                                  spec.surround_pan.distance});
  mixing::apply_strip_eq(*strip, spec.eq, nullptr);
  for (const auto& insert : spec.inserts) {
    auto processor =
        mastering::api::make_insert(insert.processor_name, insert.params_json, nullptr);
    if (!processor) {
      return nullptr;
    }
    const bool spo = mastering::api::channel_policy(insert.processor_name) ==
                     mastering::api::ChannelPolicy::StereoPairOnly;
    if (insert.slot == mixing::api::InsertSlot::PreFader) {
      strip->add_pre_insert(std::move(processor), spo);
    } else {
      strip->add_post_insert(std::move(processor), spo);
    }
  }
  return strip;
}

const std::string* strip_insert_processor_name_at(const mixing::api::Strip& spec,
                                                  unsigned int insert_index) noexcept {
  unsigned int index = 0;
  for (const auto& insert : spec.inserts) {
    if (insert.slot != mixing::api::InsertSlot::PreFader) continue;
    if (index == insert_index) return &insert.processor_name;
    ++index;
  }
  for (const auto& insert : spec.inserts) {
    if (insert.slot != mixing::api::InsertSlot::PostFader) continue;
    if (index == insert_index) return &insert.processor_name;
    ++index;
  }
  return nullptr;
}

namespace {

util::json::Value canonicalize_json_zeroes(const util::json::Value& value) {
  if (value.is_number()) {
    const double number = value.as_number();
    // The WASM facade's JSON round trip maps -0 to 0; match it so a scene resend compares equal.
    return util::json::Value(number == 0.0 ? 0.0 : number);
  }
  if (value.is_array()) {
    util::json::Array normalized;
    normalized.reserve(value.as_array().size());
    for (const util::json::Value& child : value.as_array()) {
      normalized.push_back(canonicalize_json_zeroes(child));
    }
    return util::json::Value(std::move(normalized));
  }
  if (value.is_object()) {
    util::json::Object normalized;
    for (const auto& [key, child] : value.as_object()) {
      normalized.emplace(key, canonicalize_json_zeroes(child));
    }
    return util::json::Value(std::move(normalized));
  }
  return value;
}

std::string canonical_insert_params(const std::string& params_json) {
  // Empty text (omitted params) stays distinct from `{}`; invalid text keeps its exact bytes.
  if (params_json.empty()) return std::string(1, '\0');
  try {
    return std::string(1, '\1') +
           util::json::dump(canonicalize_json_zeroes(util::json::parse(params_json)));
  } catch (...) {
    return std::string(1, '\2') + params_json;
  }
}

bool inserts_match(const mixing::api::Insert& left, const mixing::api::Insert& right) {
  return left.processor_name == right.processor_name &&
         canonical_insert_params(left.params_json) == canonical_insert_params(right.params_json) &&
         left.sidechain_key == right.sidechain_key;
}

}  // namespace

bool strip_inserts_equal(const std::vector<mixing::api::Insert>& a,
                         const std::vector<mixing::api::Insert>& b, bool canonical_track_order) {
  if (a.size() != b.size()) return false;
  if (!canonical_track_order) {
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].slot != b[i].slot || !inserts_match(a[i], b[i])) return false;
    }
    return true;
  }

  // Track/master strips store pre and post chains separately. Scene JSON may
  // interleave their entries, so compare each stage in its stable within-stage
  // order rather than comparing the raw scene vector positions.
  const auto stage_matches = [&a, &b](mixing::api::InsertSlot slot) {
    size_t left_index = 0;
    size_t right_index = 0;
    while (true) {
      while (left_index < a.size() && a[left_index].slot != slot) ++left_index;
      while (right_index < b.size() && b[right_index].slot != slot) ++right_index;
      if (left_index == a.size() || right_index == b.size()) {
        return left_index == a.size() && right_index == b.size();
      }
      if (!inserts_match(a[left_index], b[right_index])) return false;
      ++left_index;
      ++right_index;
    }
  };
  return stage_matches(mixing::api::InsertSlot::PreFader) &&
         stage_matches(mixing::api::InsertSlot::PostFader);
}

void apply_strip_scalars(mixing::ChannelStrip& strip, const mixing::api::Strip& next,
                         const mixing::api::Strip& previous) {
  strip.set_fader_db(next.fader_db);
  strip.set_pan(next.pan);
  strip.set_pan_law(mixing::pan_law_from_index(next.pan_law));
  strip.set_input_trim_db(next.input_trim_db);
  strip.set_vca_offset_db(next.vca_offset_db);
  strip.set_width(next.width);
  strip.set_muted(next.muted);
  strip.set_soloed(next.soloed);
  strip.set_solo_safe(next.solo_safe);
  strip.set_pan_mode(to_pan_mode(next.pan_mode));
  strip.set_dual_pan(next.dual_pan_left, next.dual_pan_right);
  strip.set_polarity_invert(next.polarity_invert_left, next.polarity_invert_right);
  strip.set_channel_delay_samples(next.channel_delay_samples);
  strip.set_surround_pan_params({next.surround_pan.azimuth, next.surround_pan.elevation,
                                 next.surround_pan.divergence, next.surround_pan.lfe,
                                 next.surround_pan.distance});
  mixing::apply_strip_eq(strip, next.eq, &previous.eq);
}

bool strip_eq_acceptable(const mixing::api::StripEq& eq, double sample_rate) noexcept {
  try {
    mixing::validate_eq(eq);
    for (const mastering::eq::EqBand& band : eq.bands) {
      (void)mastering::eq::design_eq_biquad(band, sample_rate);
    }
  } catch (...) {
    return false;
  }
  return true;
}

void store_eq_band(mixing::api::StripEq& eq, size_t band_index, const mastering::eq::EqBand& band) {
  if (eq.bands.size() <= band_index) eq.bands.resize(band_index + 1);
  eq.bands[band_index] = band;
}

namespace {

bool bus_pan_is_default(const mixing::api::Bus& bus) noexcept {
  const mixing::api::Bus defaults;
  return bus.pan == defaults.pan && bus.pan_mode == defaults.pan_mode &&
         bus.pan_law == defaults.pan_law && bus.dual_pan_left == defaults.dual_pan_left &&
         bus.dual_pan_right == defaults.dual_pan_right;
}

// Pan and width are stereo-image operations, refused on a bus wider than two channels.
bool bus_stereo_image_is_default(const mixing::api::Bus& bus) noexcept {
  return bus_pan_is_default(bus) && bus.width == mixing::api::Bus{}.width;
}

bool layout_wider_than_stereo(ChannelLayout layout) noexcept {
  return channel_count(layout) > TrackMixerRuntime::kMaxLaneChannels;
}

bool lane_sends_equal(const TrackLaneConfig& left, const TrackLaneConfig& right) noexcept {
  if (left.sends.size() != right.sends.size()) return false;
  for (size_t send_index = 0; send_index < left.sends.size(); ++send_index) {
    const TrackLaneConfig::Send& lhs = left.sends[send_index];
    const TrackLaneConfig::Send& rhs = right.sends[send_index];
    if (lhs.bus_id != rhs.bus_id || lhs.level_db != rhs.level_db || lhs.enabled != rhs.enabled ||
        lhs.timing != rhs.timing) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool TrackMixerRuntime::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  if (!lane_config_valid(lanes)) return false;
  constexpr int kMaxLaneClipDelayQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  // Refuse a non-empty lane snapshot when the retained direct delay exceeds the lane bank ceiling.
  if (!lanes.empty() && clip_pdc_delay_q8_ > kMaxLaneClipDelayQ8) return false;
  std::shared_ptr<const std::vector<TrackLaneConfig>> snapshot;
  try {
    snapshot = std::make_shared<const std::vector<TrackLaneConfig>>(std::move(lanes));
    // All control-table growth happens before any semantic mutation.  The
    // commit below only appends into these reserved tables.
    owned_strips_.reserve(owned_strips_.size() + snapshot->size());
    track_strip_bindings_.reserve(track_strip_bindings_.size() + snapshot->size());
  } catch (...) {
    return false;
  }

  std::array<mixing::ChannelStrip*, kMaxTrackLanes> candidate_strips{};
  std::array<std::unique_ptr<mixing::ChannelStrip>, kMaxTrackLanes> staged_owned;
  std::array<mixing::ChannelStrip::PreparedSends, kMaxTrackLanes> staged_sends;
  std::array<bool, kMaxTrackLanes> replace_sends{};
  const std::vector<TrackLaneConfig>* previous_lanes = lanes_.control_current().get();
  size_t staged_owned_count = 0;
  // Owned strips of tracks leaving the snapshot are destroyed at commit, so only the rest count.
  size_t retained_owned_count = 0;
  for (const OwnedStrip& owned : owned_strips_) {
    for (const TrackLaneConfig& config : *snapshot) {
      if (config.track_id == owned.track_id) {
        ++retained_owned_count;
        break;
      }
    }
  }
  try {
    for (size_t lane_index = 0; lane_index < snapshot->size(); ++lane_index) {
      const TrackLaneConfig& config = (*snapshot)[lane_index];
      candidate_strips[lane_index] = bound_strip_for(config.track_id);
      if (candidate_strips[lane_index] == nullptr) {
        candidate_strips[lane_index] = owned_strip_for(config.track_id);
      }
      if (candidate_strips[lane_index] == nullptr && !config.sends.empty()) {
        if (retained_owned_count + staged_owned_count >= kMaxTrackLanes) return false;
        auto strip = std::make_unique<mixing::ChannelStrip>(
            mixing::ChannelStripConfig{0.0f, 0.0f, mixing::PanLaw::Linear0dB, 5.0f,
                                       mixing::EqPosition::PreFader, 0.0f, false});
        if (max_block_size_ > 0) strip->prepare(sample_rate_, max_block_size_);
        candidate_strips[lane_index] = strip.get();
        staged_owned[lane_index] = std::move(strip);
        ++staged_owned_count;
      }
      if (candidate_strips[lane_index] == nullptr) continue;

      // Keep the retained send processor and its in-flight ramp when identity and config match.
      bool retain_sends = false;
      if (previous_lanes != nullptr && staged_owned[lane_index] == nullptr) {
        for (size_t previous_index = 0; previous_index < previous_lanes->size(); ++previous_index) {
          if ((*previous_lanes)[previous_index].track_id != (*snapshot)[lane_index].track_id) {
            continue;
          }
          if (previous_index < lane_states_.size() &&
              lane_states_[previous_index].track_id == (*snapshot)[lane_index].track_id &&
              lane_states_[previous_index].strip == candidate_strips[lane_index] &&
              candidate_strips[lane_index]->num_sends() == (*snapshot)[lane_index].sends.size() &&
              lane_sends_equal((*previous_lanes)[previous_index], (*snapshot)[lane_index])) {
            retain_sends = true;
          }
          break;
        }
      }
      if (retain_sends) continue;
      replace_sends[lane_index] = true;
      std::array<mixing::SendConfig, mixing::ChannelStrip::kMaxSends> sends{};
      for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
        const TrackLaneConfig::Send& send = config.sends[send_index];
        sends[send_index] =
            mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f};
      }
      if (!candidate_strips[lane_index]->prepare_sends(sends.data(), config.sends.size(),
                                                       staged_sends[lane_index])) {
        return false;
      }
    }
  } catch (...) {
    return false;
  }

  PdcPlan plan;
  const BusGraphView view = current_bus_graph_view();
  if (!plan_pdc(*snapshot, view, &plan, &candidate_strips)) return false;
  std::array<int, kMaxTrackLanes> lane_sources{};
  std::array<bool, kMaxTrackLanes> lane_reset{};
  make_lane_pdc_sources(*snapshot, &lane_sources, &lane_reset);
  PreparedPdc prepared_pdc;
  if (!prepare_pdc_updates(plan, lane_sources, lane_reset, &prepared_pdc)) return false;

  // From here on every vector has capacity and every processor is prepared, so nothing can fail.
  // Insert ramps stay keyed by track identity; only a track leaving the snapshot is retired.
  settle_insert_automations(*snapshot, true);
  remap_lane_insert_automations(*snapshot);
  for (size_t lane_index = 0; lane_index < snapshot->size(); ++lane_index) {
    if (staged_owned[lane_index] == nullptr) continue;
    mixing::ChannelStrip* raw = staged_owned[lane_index].get();
    owned_strips_.push_back(
        OwnedStrip{(*snapshot)[lane_index].track_id, std::move(staged_owned[lane_index]), {}});
    record_track_strip_binding((*snapshot)[lane_index].track_id, raw);
  }
  for (size_t lane_index = 0; lane_index < snapshot->size(); ++lane_index) {
    if (candidate_strips[lane_index] != nullptr && replace_sends[lane_index]) {
      candidate_strips[lane_index]->commit_sends(staged_sends[lane_index]);
    }
  }
  commit_pdc_updates(prepared_pdc);
  // snapshot is non-null and publish() has no other rejection path.  Treat
  // this as the publication point so a hypothetical future failure cannot
  // report false after the no-fail commit has already begun.
  (void)lanes_.publish(snapshot);
  lanes_.acquire_control_quiescent();
  sidechains_reader_.try_load_into(&audio_sidechains_);
  prepare_lanes_from_snapshot(*snapshot, &candidate_strips);
  for (size_t index = owned_strips_.size(); index > 0; --index) {
    const uint32_t track_id = owned_strips_[index - 1].track_id;
    const bool present =
        std::any_of(snapshot->begin(), snapshot->end(),
                    [track_id](const TrackLaneConfig& lane) { return lane.track_id == track_id; });
    if (present) continue;
    erase_owned_strip(index - 1);
    prune_lane_sidechains(track_id, 0);
  }
  return true;
}

bool TrackMixerRuntime::set_buses(std::vector<TrackBusConfig> buses) {
  if (!bus_config_valid(buses)) return false;
  // Bus state follows the bus id, not the slot: source[i] is the slot new bus i
  // held before, or -1 for a bus declared now.
  std::array<int, kMaxBusLanes> source{};
  source.fill(-1);
  std::array<bool, kMaxBusLanes> kept{};
  std::array<bool, kMaxBusLanes> moved_out{};
  std::array<ChannelLayout, kMaxBusLanes> previous_layout{};
  std::array<float, kMaxBusLanes> previous_gain_db{};
  for (size_t index = 0; index < bus_configs_.size() && index < previous_layout.size(); ++index) {
    previous_layout[index] = bus_configs_[index].layout;
    previous_gain_db[index] = bus_configs_[index].gain_db;
  }
  bool any_move = false;
  for (size_t index = 0; index < buses.size(); ++index) {
    const int previous = configured_bus_index(buses[index].bus_id);
    if (previous < 0) continue;
    // Pan and width are refused on a wider bus rather than silently reset.
    if (layout_wider_than_stereo(buses[index].layout) &&
        !bus_stereo_image_is_default(bus_states_[static_cast<size_t>(previous)].spec)) {
      return false;
    }
    source[index] = previous;
    if (previous == static_cast<int>(index)) {
      kept[index] = true;
    } else {
      moved_out[static_cast<size_t>(previous)] = true;
      any_move = true;
    }
  }
  // Bus and master keys whose bus (source or target) is not in the new list, or
  // whose insert the target no longer has, are dropped; the rest stay edges of
  // the graph the new list has to keep acyclic.
  const auto declared = [&buses](uint32_t bus_id) {
    return std::any_of(buses.begin(), buses.end(),
                       [bus_id](const TrackBusConfig& bus) { return bus.bus_id == bus_id; });
  };
  const size_t binding_count = sidechains_.count;
  std::array<bool, kMaxSidechainBindings> drop{};
  std::array<KeyEdge, kMaxSidechainBindings> key_edges{};
  size_t key_edge_count = 0;
  for (size_t i = 0; i < binding_count; ++i) {
    const SidechainBinding& binding = sidechains_.bindings[i];
    const auto target_kind = static_cast<SidechainTargetKind>(binding.target_kind);
    if (target_kind == SidechainTargetKind::Lane) continue;
    const uint32_t target_id = binding.target_id;
    const uint32_t source_id = binding.source_id;
    const bool bus_source = binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus);
    if (bus_source && !declared(source_id)) drop[i] = true;
    if (target_kind == SidechainTargetKind::Bus) {
      const int previous = configured_bus_index(target_id);
      const mixing::FxBus* fx =
          previous >= 0 ? bus_states_[static_cast<size_t>(previous)].bus.get() : nullptr;
      if (!declared(target_id) || fx == nullptr || binding.insert_index >= fx->num_inserts()) {
        drop[i] = true;
      }
      if (!drop[i] && bus_source) key_edges[key_edge_count++] = KeyEdge{source_id, target_id};
    }
  }
  std::array<size_t, kMaxBusLanes> order{};
  if (!validate_bus_graph(buses, key_edges.data(), key_edge_count, &order)) return false;
  // Every lane output and lane send has to land on a bus the new list keeps.
  static const std::vector<TrackLaneConfig> kNoLanes;
  const std::vector<TrackLaneConfig>* current_lanes = lanes_.control_current().get();
  const std::vector<TrackLaneConfig>& lanes = current_lanes ? *current_lanes : kNoLanes;
  for (const TrackLaneConfig& lane : lanes) {
    if (lane.output_bus_id != 0 && !declared(lane.output_bus_id)) return false;
    for (const TrackLaneConfig::Send& send : lane.sends) {
      if (!declared(send.bus_id)) return false;
    }
  }
  // The alignment the new graph needs has to fit the delay lines.
  BusGraphView view;
  view.buses = &buses;
  view.order = order;
  view.skip_binding = drop;
  build_routes(buses, drop, &view.routes);
  for (size_t index = 0; index < buses.size(); ++index) {
    const mixing::FxBus* fx =
        source[index] >= 0 ? bus_states_[static_cast<size_t>(source[index])].bus.get() : nullptr;
    view.bus[index] = fx;
    view.latency_q8[index] = fx != nullptr ? fx->latency_samples_q8() : 0;
  }
  PdcPlan plan;
  if (!plan_pdc(lanes, view, &plan)) return false;
  std::array<int, kMaxBusLanes> bank_sources{};
  bank_sources.fill(-1);
  std::array<bool, kMaxBusLanes> bank_used{};
  for (size_t index = 0; index < kMaxBusLanes; ++index) {
    int previous = index < buses.size() ? source[index] : -1;
    if (previous < 0 || previous >= static_cast<int>(kMaxBusLanes) ||
        bank_used[static_cast<size_t>(previous)]) {
      previous = -1;
      for (size_t candidate = 0; candidate < kMaxBusLanes; ++candidate) {
        if (!bank_used[candidate]) {
          previous = static_cast<int>(candidate);
          break;
        }
      }
    }
    if (previous < 0) continue;
    bank_sources[index] = previous;
    bank_used[static_cast<size_t>(previous)] = true;
  }
  // Stage every lane send table before the bus table changes, resolving strips by
  // track identity so a lane reorder cannot make the staging positional.
  std::array<mixing::ChannelStrip*, kMaxTrackLanes> lane_send_strips{};
  std::array<std::unique_ptr<mixing::ChannelStrip>, kMaxTrackLanes> staged_lane_owned;
  std::array<mixing::ChannelStrip::PreparedSends, kMaxTrackLanes> prepared_lane_sends;
  std::array<bool, kMaxTrackLanes> replace_lane_sends{};
  size_t staged_lane_owned_count = 0;
  try {
    for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
      const TrackLaneConfig& config = lanes[lane_index];
      mixing::ChannelStrip* strip = bound_strip_for(config.track_id);
      if (strip == nullptr) strip = owned_strip_for(config.track_id);
      if (strip == nullptr) {
        for (const LaneState& state : lane_states_) {
          if (state.track_id == config.track_id) {
            strip = state.strip;
            break;
          }
        }
      }
      if (strip == nullptr && !config.sends.empty()) {
        if (owned_strips_.size() + staged_lane_owned_count >= kMaxTrackLanes) return false;
        auto owned = std::make_unique<mixing::ChannelStrip>(
            mixing::ChannelStripConfig{0.0f, 0.0f, mixing::PanLaw::Linear0dB, 5.0f,
                                       mixing::EqPosition::PreFader, 0.0f, false});
        if (max_block_size_ > 0) owned->prepare(sample_rate_, max_block_size_);
        strip = owned.get();
        staged_lane_owned[lane_index] = std::move(owned);
        ++staged_lane_owned_count;
      }
      lane_send_strips[lane_index] = strip;
      if (strip == nullptr) continue;
      // set_buses() leaves the lane snapshot and its strip identity intact.
      // Preserve the existing send processor in that case, since replacing it
      // would clear queued send automation even though no lane send changed.
      const bool retain_sends = staged_lane_owned[lane_index] == nullptr &&
                                lane_states_[lane_index].track_id == config.track_id &&
                                lane_states_[lane_index].strip == strip &&
                                strip->num_sends() == config.sends.size();
      if (retain_sends) continue;
      replace_lane_sends[lane_index] = true;
      std::array<mixing::SendConfig, mixing::ChannelStrip::kMaxSends> configs{};
      for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
        const TrackLaneConfig::Send& send = config.sends[send_index];
        configs[send_index] =
            mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f};
      }
      if (!strip->prepare_sends(configs.data(), config.sends.size(),
                                prepared_lane_sends[lane_index])) {
        return false;
      }
    }
    owned_strips_.reserve(owned_strips_.size() + staged_lane_owned_count);
    track_strip_bindings_.reserve(track_strip_bindings_.size() + staged_lane_owned_count);
  } catch (...) {
    return false;
  }

  // Prepare new FxBus objects up front so the commit only moves pointers and sets scalars.
  std::array<std::unique_ptr<mixing::FxBus>, kMaxBusLanes> staged_buses;
  try {
    for (size_t index = 0; index < buses.size(); ++index) {
      const int previous = source[index];
      const bool missing =
          previous < 0 || bus_states_[static_cast<size_t>(previous)].bus == nullptr;
      if (!missing) continue;
      auto bus = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
      bus->set_channel_layout(buses[index].layout);
      if (max_block_size_ > 0) bus->prepare(sample_rate_, max_block_size_);
      staged_buses[index] = std::move(bus);
    }
  } catch (...) {
    return false;
  }

  // Stage PDC replacement storage now; commit_pdc_updates() applies scalar plans after the remap.
  std::array<int, kMaxTrackLanes> pdc_sources{};
  std::array<bool, kMaxTrackLanes> pdc_reset{};
  for (size_t index = 0; index < kMaxTrackLanes; ++index) {
    pdc_sources[index] = static_cast<int>(index);
    pdc_reset[index] = false;
  }
  PreparedPdc prepared_pdc;
  if (!prepare_pdc_updates(plan, pdc_sources, pdc_reset, &prepared_pdc, &bank_sources)) {
    return false;
  }

  // Reserve every selector entry the new bus identities may need (a stale tombstone counts as new).
  size_t selectors_needed = 0;
  for (const TrackBusConfig& config : buses) {
    bool active = false;
    for (const BusInsertSelectorBinding& binding : bus_insert_selectors_) {
      active = active || (binding.active && binding.bus_id == config.bus_id);
    }
    if (!active) ++selectors_needed;
  }
  if (bus_insert_selectors_.size() + selectors_needed > kMaxBusInsertSelectors) return false;
  try {
    bus_insert_selectors_.reserve(bus_insert_selectors_.size() + selectors_needed);
  } catch (...) {
    return false;
  }

  decltype(bus_sends_) sends{};
  try {
    for (size_t index = 0; index < buses.size(); ++index) {
      for (size_t send_index = 0; send_index < buses[index].sends.size(); ++send_index) {
        const TrackLaneConfig::Send& send = buses[index].sends[send_index];
        auto processor = std::make_unique<mixing::SendProcessor>(
            mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f});
        if (max_block_size_ > 0) processor->prepare(sample_rate_, max_block_size_);
        sends[index][send_index] = std::move(processor);
      }
    }
  } catch (...) {
    return false;
  }
  std::unique_ptr<std::array<BusState, kMaxBusLanes>> staging;
  try {
    if (any_move) staging = std::make_unique<std::array<BusState, kMaxBusLanes>>();
    for (size_t index = 0; index < bus_states_.size(); ++index) {
      if (kept[index]) continue;
      if (moved_out[index]) {
        (*staging)[index].eq.prepare(sample_rate_, max_block_size_);
        transfer_bus_state(bus_states_[index], (*staging)[index]);
      }
      // No semantic mutation occurs in this loop; it only stages moved state.
    }
  } catch (...) {
    return false;
  }

  // Alignment banks move with their bus id; edge histories survive only while routes are unchanged.
  const auto resolves_to_bus = [](const std::vector<TrackBusConfig>& configs, uint32_t bus_id) {
    return std::any_of(configs.begin(), configs.end(),
                       [bus_id](const TrackBusConfig& config) { return config.bus_id == bus_id; });
  };
  const auto same_destination = [&](uint32_t old_id, uint32_t next_id) {
    const bool old_bus = old_id != 0 && resolves_to_bus(bus_configs_, old_id);
    const bool next_bus = next_id != 0 && resolves_to_bus(buses, next_id);
    return old_bus == next_bus && (!old_bus || old_id == next_id);
  };
  const auto routes_unchanged = [&](size_t index) {
    const int previous = source[index];
    if (previous < 0) return false;
    const TrackBusConfig& old = bus_configs_[static_cast<size_t>(previous)];
    const TrackBusConfig& next = buses[index];
    if (old.layout != next.layout || old.sends.size() != next.sends.size() ||
        !same_destination(old.output_bus_id, next.output_bus_id)) {
      return false;
    }
    for (size_t send = 0; send < old.sends.size(); ++send) {
      if (old.sends[send].timing != next.sends[send].timing ||
          !same_destination(old.sends[send].bus_id, next.sends[send].bus_id)) {
        return false;
      }
    }
    return true;
  };

  std::array<mixing::AlignmentDelay, kMaxBusLanes> old_bus_pdc = std::move(bus_pdc_delays_);
  std::array<mixing::AlignmentDelay, kMaxBusLanes> next_bus_pdc;
  std::array<mixing::AlignmentDelay, kMaxBusLanes * kBusEdgesPerBus> old_edges =
      std::move(bus_edge_delays_);
  std::array<mixing::AlignmentDelay, kMaxBusLanes * kBusEdgesPerBus> next_edges;
  for (size_t index = 0; index < kMaxBusLanes; ++index) {
    const int previous = bank_sources[index];
    if (previous < 0) continue;
    const bool retained_bus = index < buses.size() && source[index] >= 0;
    const bool retained_edges = retained_bus && routes_unchanged(index);
    next_bus_pdc[index] = std::move(old_bus_pdc[static_cast<size_t>(previous)]);
    if (!retained_bus) next_bus_pdc[index].reset();
    for (size_t edge = 0; edge < kBusEdgesPerBus; ++edge) {
      next_edges[index * kBusEdgesPerBus + edge] =
          std::move(old_edges[static_cast<size_t>(previous) * kBusEdgesPerBus + edge]);
      if (!retained_edges) next_edges[index * kBusEdgesPerBus + edge].reset();
    }
  }
  bus_pdc_delays_ = std::move(next_bus_pdc);
  bus_edge_delays_ = std::move(next_edges);
  for (size_t index = 0; index < bus_states_.size(); ++index) {
    if (!kept[index]) retire_bus_state(bus_states_[index]);
  }
  bus_configs_ = std::move(buses);
  for (InsertGainReductionBoard& board : bus_insert_gr_boards_) board.clear();
  // Retire selectors for buses that disappeared. Keeping the mapping entry as
  // an inactive tombstone makes an old queued id a no-op; if the same numeric
  // id is later reused, it receives a fresh selector instead of reviving the
  // stale command.
  for (BusInsertSelectorBinding& binding : bus_insert_selectors_) {
    if (configured_bus_index(binding.bus_id) < 0) {
      binding.active = false;
    }
  }
  for (const TrackBusConfig& config : bus_configs_) {
    // Reserve each active bus's selector by identity. Retired selectors remain
    // tombstoned in bus_insert_selectors_ so stale commands cannot retarget a
    // later bus after a reorder or removal.
    (void)bus_insert_automation_selector(config.bus_id);
  }
  bus_sends_ = std::move(sends);
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    if (staged_lane_owned[lane_index] == nullptr) continue;
    mixing::ChannelStrip* raw = staged_lane_owned[lane_index].get();
    owned_strips_.push_back(
        OwnedStrip{lanes[lane_index].track_id, std::move(staged_lane_owned[lane_index]), {}});
    record_track_strip_binding(lanes[lane_index].track_id, raw);
  }
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    if (lane_send_strips[lane_index] != nullptr && replace_lane_sends[lane_index]) {
      lane_send_strips[lane_index]->commit_sends(prepared_lane_sends[lane_index]);
    }
  }
  for (size_t i = binding_count; i > 0; --i) {
    if (drop[i - 1]) remove_sidechain_binding(i - 1);
  }
  // Keep a live bus smoother across a positional reorder. Only buses that no
  // longer exist are retired; the slot is keyed by bus_id and resolves its
  // current render index on each advance.
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.assigned || !slot.is_bus) continue;
    if (configured_bus_index(slot.bus_id) < 0) {
      slot.active = false;
      slot.assigned = false;
    }
  }
  for (size_t index = 0; index < bus_configs_.size(); ++index) {
    BusState& state = bus_states_[index];
    if (!kept[index]) {
      if (source[index] >= 0) {
        transfer_bus_state((*staging)[static_cast<size_t>(source[index])], state);
      }
    }
    state.bus_id = bus_configs_[index].bus_id;
    if (source[index] >= 0) {
      // A retained bus may have active gain/trim/width ramps. Keep their
      // current and target values through a same-order republish or an
      // identity reorder; only a changed TrackBusConfig gain is a new target.
      if (previous_gain_db[static_cast<size_t>(source[index])] != bus_configs_[index].gain_db) {
        state.gain.set_target(db_to_linear(bus_configs_[index].gain_db));
      }
    } else {
      state.gain.prepare(sample_rate_, 5.0f);
      state.gain.reset(db_to_linear(bus_configs_[index].gain_db));
      state.input_trim_gain.prepare(sample_rate_, 5.0f);
      if (max_block_size_ > 0) {
        state.width.prepare(sample_rate_, max_block_size_);
      }
    }
    if (!state.bus) {
      state.bus = std::move(staged_buses[index]);
    }
    // Dedicated EQ state is per output plane. A retained bus can change width
    // under the same identity, so a plane skipped while the bus was narrower
    // must not resume with its old filter history when that plane returns.
    // Reorders keep the history because the bus state follows its id.
    if (source[index] >= 0 &&
        previous_layout[static_cast<size_t>(source[index])] != bus_configs_[index].layout) {
      state.eq.reset();
    }
    state.bus->set_channel_layout(bus_configs_[index].layout);
    // Existing FxBus objects were prepared before this transaction. Newly
    // declared slots were prepared into staged_buses[] above.
  }
  refresh_bus_graph();
  commit_pdc_updates(prepared_pdc);
  return true;
}

void TrackMixerRuntime::transfer_bus_state(BusState& from, BusState& to) {
  to.bus_id = from.bus_id;
  to.gain = from.gain;
  to.input_trim_gain = from.input_trim_gain;
  to.width.copy_state_from(from.width);
  to.polarity_left.store(from.polarity_left.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
  to.polarity_right.store(from.polarity_right.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
  to.panner.copy_state_from(from.panner);
  // The state belongs to the bus identity, so a positional move must carry it
  // across with the rest of the retained bus state. set_buses() resets it after
  // this transfer only when the destination layout changes.
  to.eq = from.eq;
  to.eq_enabled.store(from.eq_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
  to.eq_active.store(from.eq_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
  to.bus = std::move(from.bus);
  to.spec = std::move(from.spec);
}

void TrackMixerRuntime::retire_bus_state(BusState& state) {
  state.bus_id = 0;
  state.gain.reset(1.0f);
  state.input_trim_gain.reset(1.0f);
  state.width.set_width(1.0f);
  state.width.reset();
  state.polarity_left.store(1.0f, std::memory_order_relaxed);
  state.polarity_right.store(1.0f, std::memory_order_relaxed);
  state.spec = mixing::api::Bus{};
  apply_bus_pan(state, state.spec);
  state.panner.reset();
  state.eq.clear();
  state.eq.reset();
  state.eq_enabled.store(true, std::memory_order_relaxed);
  state.eq_active.store(false, std::memory_order_relaxed);
  state.bus.reset();
}

void TrackMixerRuntime::apply_bus_pan(BusState& state, const mixing::api::Bus& bus) noexcept {
  state.panner.set_pan_mode(to_pan_mode(bus.pan_mode));
  state.panner.set_pan_law(mixing::pan_law_from_index(bus.pan_law));
  state.panner.set_pan(bus.pan);
  state.panner.set_dual_pan(bus.dual_pan_left, bus.dual_pan_right);
}

void TrackMixerRuntime::refresh_bus_eq_active(BusState& state) noexcept {
  bool active = false;
  for (size_t band = 0; band < mastering::eq::ParametricEq::kMaxBands; ++band) {
    active = active || state.eq.band(band).enabled;
  }
  state.eq_active.store(active, std::memory_order_relaxed);
}

bool TrackMixerRuntime::active() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  return lanes && !lanes->empty();
}

size_t TrackMixerRuntime::lane_count() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  return lanes ? lanes->size() : 0;
}

size_t TrackMixerRuntime::copy_lane_track_ids(uint32_t* out, size_t capacity) const noexcept {
  if (out == nullptr || capacity == 0) return 0;
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes == nullptr) return 0;
  size_t count = 0;
  for (const TrackLaneConfig& lane : *lanes) {
    if (lane.track_id == 0) continue;
    bool seen = false;
    for (size_t i = 0; i < count; ++i) seen = seen || out[i] == lane.track_id;
    if (seen || count >= capacity) continue;
    out[count++] = lane.track_id;
  }
  return count;
}

bool TrackMixerRuntime::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  if (track_id == 0) return false;
  acquire_lanes();
  // Control-side snapshot throughout: binding a strip is a control-thread
  // structural change, and current() is the audio thread's view.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id) continue;
    const size_t lane_index = static_cast<size_t>(&lane - lane_states_.data());
    clear_insert_automation_for_lane(lane_index);
    lane.strip = strip;
    record_track_strip_binding(track_id, strip);
    if (strip && max_block_size_ > 0) {
      strip->prepare(sample_rate_, max_block_size_);
    }
    if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
      try {
        configure_lane_sends(*lanes, track_id);
      } catch (...) {
        return false;
      }
      if (!recompute_lane_pdc(*lanes)) return false;
    }
    return true;
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != 0) continue;
    const size_t lane_index = static_cast<size_t>(&lane - lane_states_.data());
    clear_insert_automation_for_lane(lane_index);
    lane.track_id = track_id;
    lane.strip = strip;
    record_track_strip_binding(track_id, strip);
    if (strip && max_block_size_ > 0) {
      strip->prepare(sample_rate_, max_block_size_);
    }
    if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
      try {
        configure_lane_sends(*lanes, track_id);
      } catch (...) {
        return false;
      }
      if (!recompute_lane_pdc(*lanes)) return false;
    }
    return true;
  }
  return false;
}

bool TrackMixerRuntime::release_track_strip(uint32_t track_id) {
  if (track_id == 0) return false;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id) continue;
    clear_insert_automation_for_lane(static_cast<size_t>(&lane - lane_states_.data()));
    lane.strip = nullptr;
  }
  for (size_t index = 0; index < owned_strips_.size(); ++index) {
    if (owned_strips_[index].track_id != track_id) continue;
    erase_owned_strip(index);
    break;
  }
  track_strip_bindings_.erase(
      std::remove_if(track_strip_bindings_.begin(), track_strip_bindings_.end(),
                     [track_id](const TrackStripBinding& b) { return b.track_id == track_id; }),
      track_strip_bindings_.end());
  prune_lane_sidechains(track_id, 0);
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    try {
      configure_lane_sends(*lanes, track_id);
    } catch (...) {
      return false;
    }
    if (!recompute_lane_pdc(*lanes)) return false;
  }
  return true;
}

void TrackMixerRuntime::erase_owned_strip(size_t index) noexcept {
  const mixing::ChannelStrip* raw = owned_strips_[index].strip.get();
  for (LaneState& lane : lane_states_) {
    if (lane.strip == raw) lane.strip = nullptr;
  }
  track_strip_bindings_.erase(
      std::remove_if(track_strip_bindings_.begin(), track_strip_bindings_.end(),
                     [raw](const TrackStripBinding& b) { return b.strip == raw; }),
      track_strip_bindings_.end());
  owned_strips_.erase(owned_strips_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool TrackMixerRuntime::set_track_strip(uint32_t track_id, const mixing::api::Strip& spec) {
  if (track_id == 0 || !strip_eq_acceptable(spec.eq, sample_rate_)) return false;

  // In-place fast path: when a strip already exists for this track and only its
  // smoothable scalars changed (identical insert topology), retarget the existing
  // strip's parameters instead of rebuilding it. A rebuild constructs a fresh
  // strip whose fader/pan/trim smoothers settle straight to the new value, so a
  // live gain/pan edit would jump (an audible click); an in-place update keeps
  // the smoother state so the change ramps. PDC is recomputed in case the EQ
  // latency changed; the strip pointer is unchanged so the lane binding stays valid.
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id && owned.strip &&
        strip_inserts_equal(owned.spec.inserts, spec.inserts)) {
      apply_strip_scalars(*owned.strip, spec, owned.spec);
      owned.spec = spec;
      if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
        if (!recompute_lane_pdc(*lanes)) return false;
      }
      prune_lane_sidechains(track_id, spec.inserts.size());
      return true;
    }
  }

  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(spec);
  } catch (...) {
    return false;
  }
  if (!strip) return false;
  if (max_block_size_ > 0) {
    strip->prepare(sample_rate_, max_block_size_);
  }

  mixing::ChannelStrip* raw = strip.get();
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id) {
      // Control-thread-only, not concurrent with process() (see RealtimeEngine's
      // thread-safety contract). This std::move destroys the previously bound
      // strip immediately -- there is no deferred reclaim -- before rebinding the
      // raw pointer the audio thread reads, so a concurrent render would use
      // freed memory.
      owned.strip = std::move(strip);
      owned.spec = spec;
      const bool bound = bind_track_strip(track_id, raw);
      if (bound) prune_lane_sidechains(track_id, spec.inserts.size());
      return bound;
    }
  }
  if (owned_strips_.size() >= kMaxTrackLanes) {
    return false;
  }
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip), spec});
  const bool bound = bind_track_strip(track_id, raw);
  if (bound) prune_lane_sidechains(track_id, spec.inserts.size());
  return bound;
}

bool TrackMixerRuntime::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  const int found = configured_bus_index(bus_id);
  if (found < 0) return false;
  const size_t bus_index = static_cast<size_t>(found);
  BusState* state = &bus_states_[bus_index];
  // Validate everything before applying anything. The width check reads the
  // engine's own layout, not the spec's, which may be omitted.
  if (!std::isfinite(bus.pan) || !std::isfinite(bus.dual_pan_left) ||
      !std::isfinite(bus.dual_pan_right) ||
      (layout_wider_than_stereo(bus_configs_[bus_index].layout) &&
       !bus_stereo_image_is_default(bus)) ||
      !strip_eq_acceptable(bus.eq, sample_rate_)) {
    return false;
  }
  std::unique_ptr<mixing::FxBus> fx;
  const bool rebuild = !strip_inserts_equal(state->spec.inserts, bus.inserts,
                                            /*canonical_track_order=*/false);
  if (rebuild) {
    fx = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
    fx->set_channel_layout(bus_configs_[bus_index].layout);
    try {
      for (const auto& insert : bus.inserts) {
        auto processor =
            mastering::api::make_insert(insert.processor_name, insert.params_json, nullptr);
        if (!processor) return false;
        const bool spo = mastering::api::channel_policy(insert.processor_name) ==
                         mastering::api::ChannelPolicy::StereoPairOnly;
        fx->add_insert(std::move(processor), spo);
      }
    } catch (...) {
      return false;
    }
    if (max_block_size_ > 0) {
      fx->prepare(sample_rate_, max_block_size_);
    }
  }
  state->input_trim_gain.set_target(db_to_linear(bus.input_trim_db));
  state->width.set_width(bus.width);
  state->polarity_left.store(bus.polarity_invert_left ? -1.0f : 1.0f, std::memory_order_relaxed);
  state->polarity_right.store(bus.polarity_invert_right ? -1.0f : 1.0f, std::memory_order_relaxed);
  apply_bus_pan(*state, bus);
  mixing::apply_eq(state->eq, state->eq_enabled, bus.eq, &state->spec.eq);
  refresh_bus_eq_active(*state);
  state->spec = bus;
  if (!rebuild) return true;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && slot.is_bus && slot.bus_id == bus_id) {
      slot.active = false;
      slot.assigned = false;
    }
  }
  state->bus = std::move(fx);
  // Keys stay on (bus, insert index); an index the new chain lacks is dropped.
  const size_t insert_count = state->bus->num_inserts();
  for (size_t i = sidechains_.count; i > 0; --i) {
    const SidechainBinding& binding = sidechains_.bindings[i - 1];
    if (binding.target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus) &&
        binding.target_id == bus_id && binding.insert_index >= insert_count) {
      remove_sidechain_binding(i - 1);
    }
  }
  refresh_bus_graph();
  // A bus insert chain's latency joins the mixer's end-to-end PDC, so
  // installing one has to re-derive the alignment banks. Without this call
  // FxBus::latency_samples_q8() has no reader in the engine at all and a
  // latent bus insert silently offsets its whole parallel path.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    if (!recompute_lane_pdc(*lanes)) return false;
  }
  return true;
}

void TrackMixerRuntime::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = std::max(max_block_size, 1);
  scratch_.assign(kMaxTrackLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  bus_scratch_.assign(kMaxBusLanes * kMaxBusChannels * static_cast<size_t>(max_block_size_), 0.0f);
  key_scratch_.assign(kMaxTrackLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_),
                      0.0f);
  // No lane has snapshotted yet, so every key plane reads as silence until one
  // does rather than as whatever the previous prepare() left.
  key_frames_.fill(0);
  // Rests at unity so a lane whose gain ramp has not been advanced yet (a strip
  // rendered outside the finish_block sequence) contributes its dry signal
  // rather than silence.
  lane_gain_scratch_.assign(kMaxTrackLanes * static_cast<size_t>(max_block_size_), 1.0f);
  lane_gate_scratch_.assign(kMaxTrackLanes * static_cast<size_t>(max_block_size_), 1.0f);
  send_source_scratch_.assign(2u * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  direct_scratch_.assign(kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  // Edge scratch, pre-fader tap and downmix fold for the bus stage.
  bus_edge_scratch_.assign(3u * kMaxBusChannels * static_cast<size_t>(max_block_size_), 0.0f);
  bus_key_scratch_.assign(kMaxBusLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_),
                          0.0f);
  bus_key_frames_.fill(0);
  keyed_input_scratch_.assign(
      kMaxSidechainBindings * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  master_key_frames_.fill(0);
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    lane.clip_pdc_delay.set_prepared_channels(kMaxLaneChannels);
    lane.clip_pdc_delay.prepare(sample_rate_, max_block_size_);
    // Same time constant as the stereo pan smoother, so a surround placement
    // glides over the same interval a stereo pan does.
    lane.surround_glide.prepare(sample_rate_, 5.0f);
    lane.surround_primed_channels = -1;
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    lane.monitor_mode = TrackMonitorMode::kOff;
    if (lane.strip) {
      lane.strip->prepare(sample_rate_, max_block_size_);
    }
  }
  for (BusState& bus : bus_states_) {
    bus.gain.prepare(sample_rate_, 5.0f);
    bus.input_trim_gain.prepare(sample_rate_, 5.0f);
    bus.width.prepare(sample_rate_, max_block_size_);
    // Every slot, configured or not, so the render never meets an unprepared
    // stage and a surround bus finds state for all of its planes.
    bus.panner.prepare(sample_rate_, max_block_size_);
    bus.eq.prepare(sample_rate_, max_block_size_);
    bus.eq.prepare_channels(kMaxBusChannels);
    if (bus.bus) {
      bus.bus->prepare(sample_rate_, max_block_size_);
    }
  }
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  direct_pdc_delay_.set_prepared_channels(kMaxLaneChannels);
  direct_pdc_delay_.prepare(sample_rate_, max_block_size_);
  for (mixing::AlignmentDelay& delay : lane_in_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  // The bus stage runs on bus and master buffers, which are as wide as the
  // widest layout the mixer renders, not the ≤2-wide lane buffers.
  for (mixing::AlignmentDelay& delay : bus_pdc_delays_) {
    delay.set_prepared_channels(kMaxBusChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  master_pdc_delay_.set_prepared_channels(kMaxBusChannels);
  master_pdc_delay_.prepare(sample_rate_, max_block_size_);
  for (mixing::AlignmentDelay& delay : bus_edge_delays_) {
    delay.set_prepared_channels(kMaxBusChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (mixing::AlignmentDelay& delay : key_edge_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (auto& sends : bus_sends_) {
    for (auto& send : sends) {
      if (send) send->prepare(sample_rate_, max_block_size_);
    }
  }
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    slot.smoother.prepare(sample_rate_, 5.0f);
    slot.smoother.reset(0.0f);
    slot.active = false;
    slot.assigned = false;
    slot.is_bus = false;
    slot.index = 0;
    slot.insert_index = 0;
    slot.param_id = 0;
  }
  insert_automation_overflow_count_ = 0;
  // Control-side snapshot: prepare() is control-thread only, and current() is
  // the audio thread's view.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    prepare_lanes_from_snapshot(*lanes);
    try {
      configure_lane_sends(*lanes);
    } catch (...) {
    }
    if (!recompute_lane_pdc(*lanes)) {
      throw SonareException(ErrorCode::InvalidState, "track mixer master PDC preparation failed");
    }
  }
}

void TrackMixerRuntime::process(float* const* channels, int num_channels, int num_samples) {
  (void)channels;
  (void)num_channels;
  (void)num_samples;
}

void TrackMixerRuntime::reset() {
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
  }
  flush_pdc_delays();
  flush_clip_pdc_delays();
}

bool TrackMixerRuntime::set_clip_pdc_delay_q8(int delay_samples_q8) noexcept {
  ClipPdcPlan prepared;
  if (!prepare_clip_pdc_delay_q8(delay_samples_q8, prepared)) return false;
  commit_clip_pdc_delay(prepared);
  return true;
}

bool TrackMixerRuntime::prepare_clip_pdc_delay_q8(int delay_samples_q8,
                                                  ClipPdcPlan& out) const noexcept {
  constexpr int kMaxDelayQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  if (delay_samples_q8 < 0) return false;
  // With no lanes there is no clip bank, so a direct-only latency above the lane ceiling is valid.
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if ((lanes == nullptr || lanes->empty()) && delay_samples_q8 > kMaxDelayQ8) {
    try {
      for (size_t lane_index = 0; lane_index < lane_states_.size(); ++lane_index) {
        out.banks[lane_index] = lane_states_[lane_index].clip_pdc_delay;
      }
      out.delay_samples_q8 = delay_samples_q8;
      return true;
    } catch (...) {
      return false;
    }
  }
  // Never clamp only the lane side: lane clips would sit on a different timebase than direct clips.
  if (delay_samples_q8 > kMaxDelayQ8) return false;
  const int bounded = delay_samples_q8;
  // Stage on copies of the live banks so a failure leaves lanes untouched and history survives.
  try {
    for (size_t lane_index = 0; lane_index < lane_states_.size(); ++lane_index) {
      out.banks[lane_index] = lane_states_[lane_index].clip_pdc_delay;
      // Keep the bank wide enough for the lane scratch buffers even when this
      // plan is prepared before the first runtime prepare() call.
      if (out.banks[lane_index].prepared_channels() < kMaxLaneChannels) {
        out.banks[lane_index].set_prepared_channels(kMaxLaneChannels);
      }
      if (!out.banks[lane_index].try_set_delay_samples_q8(bounded)) return false;
    }
    out.delay_samples_q8 = bounded;
    return true;
  } catch (...) {
    return false;
  }
}

void TrackMixerRuntime::commit_clip_pdc_delay(ClipPdcPlan& prepared) noexcept {
  for (size_t lane_index = 0; lane_index < lane_states_.size(); ++lane_index) {
    using std::swap;
    swap(lane_states_[lane_index].clip_pdc_delay, prepared.banks[lane_index]);
  }
  clip_pdc_delay_q8_ = prepared.delay_samples_q8;
}

void TrackMixerRuntime::flush_clip_pdc_delays() noexcept {
  for (LaneState& lane : lane_states_) lane.clip_pdc_delay.reset();
}

void TrackMixerRuntime::drain_clip_pdc_delays(int num_channels, int num_samples) noexcept {
  if (num_channels <= 0 || num_samples <= 0) return;
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes == nullptr || lanes->empty()) return;
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  if (render_channels <= 0) return;
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    // A stopped block still runs every lane so tails advance; raw clip delay drains here.
    source_mix_lane_active_[lane_index] = true;
    for (int ch = 0; ch < render_channels; ++ch) {
      float* lane = lane_channel(lane_index, ch);
      std::fill(lane, lane + num_samples, 0.0f);
      lane_channel_ptrs_[static_cast<size_t>(ch)] = lane;
    }
    lane_states_[lane_index].clip_pdc_delay.process(lane_channel_ptrs_.data(), render_channels,
                                                    num_samples);
  }
}

void TrackMixerRuntime::settle_smoothers() noexcept {
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.reset(lane.fader_gain.target());
    lane.pan.reset(lane.pan.target());
    lane.gate.reset(lane.gate.target());
    // The surround placement glides too, so a pre-roll settle quiesces it for
    // the same reason it quiesces the fader: otherwise the first audible block
    // glides into placement instead of opening at it.
    lane.surround_glide.settle();
    // Quiesce the lane's channel-strip gain stages too so the first rendered
    // block opens without an insert/fader ramp-in.
    if (lane.strip != nullptr) lane.strip->settle();
  }
  for (BusState& bus : bus_states_) {
    bus.gain.reset(bus.gain.target());
    bus.input_trim_gain.reset(bus.input_trim_gain.target());
    // Seed the width smoother from its target too; set_width() only stores the
    // target, so without this an offline pre-roll glides width from 1.0 over the
    // first audible block instead of opening at the configured width.
    bus.width.reset();
    bus.panner.reset();
  }
  for (auto& sends : bus_sends_) {
    for (auto& send : sends) {
      if (send) send->reset();
    }
  }
  settle_insert_automations();
}

void TrackMixerRuntime::settle_insert_automations() noexcept {
  // Structural replay restores insert targets without changing unrelated
  // fader and pan ramps in flight.
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active) continue;
    const float target = slot.smoother.target();
    slot.smoother.reset(target);
    if (slot.is_bus) {
      const int bus_index = configured_bus_index(slot.bus_id);
      if (bus_index < 0) {
        slot.active = false;
        slot.assigned = false;
        continue;
      }
      slot.index = static_cast<size_t>(bus_index);
      apply_bus_insert_parameter(slot.index, slot.insert_index, slot.param_id, target);
    } else {
      apply_lane_insert_parameter(slot.index, slot.insert_index, slot.param_id, target);
    }
    slot.active = false;
  }
}

void TrackMixerRuntime::settle_insert_automations(const std::vector<TrackLaneConfig>& next_lanes,
                                                  bool preserve_surviving_lanes) noexcept {
  if (!preserve_surviving_lanes) {
    settle_insert_automations();
    return;
  }
  // Surviving lane ramps are identity-keyed and stay unsettled; bus slots are never settled here.
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active || slot.is_bus) continue;
    const uint32_t track_id =
        slot.index < lane_states_.size() ? lane_states_[slot.index].track_id : 0;
    const bool survives = track_id != 0 && std::any_of(next_lanes.begin(), next_lanes.end(),
                                                       [track_id](const TrackLaneConfig& lane) {
                                                         return lane.track_id == track_id;
                                                       });
    if (survives) continue;
    // The strip is leaving the published graph. Do not apply its pending
    // target to an object that no longer renders; retire only this slot.
    slot.active = false;
    slot.assigned = false;
  }
}

bool TrackMixerRuntime::lane_config_valid(
    const std::vector<TrackLaneConfig>& lanes) const noexcept {
  if (lanes.size() > kMaxTrackLanes) return false;
  for (size_t i = 0; i < lanes.size(); ++i) {
    if (lanes[i].track_id == 0) return false;
    if (lanes[i].source_layout != ChannelLayout::Stereo) return false;
    if (lanes[i].output_bus_id != 0 && bus_state_for(lanes[i].output_bus_id) == nullptr) {
      return false;
    }
    if (lanes[i].sends.size() > mixing::ChannelStrip::kMaxSends) return false;
    for (size_t send_index = 0; send_index < lanes[i].sends.size(); ++send_index) {
      const TrackLaneConfig::Send& send = lanes[i].sends[send_index];
      if (send.bus_id == 0 || !std::isfinite(send.level_db) || send.level_db < kFloorDb ||
          send.level_db > kMaxGainDb || bus_state_for(send.bus_id) == nullptr) {
        return false;
      }
      for (size_t other = send_index + 1; other < lanes[i].sends.size(); ++other) {
        if (send.bus_id == lanes[i].sends[other].bus_id) return false;
      }
    }
    for (size_t j = i + 1; j < lanes.size(); ++j) {
      if (lanes[i].track_id == lanes[j].track_id) return false;
    }
  }
  return true;
}

bool TrackMixerRuntime::bus_config_valid(const std::vector<TrackBusConfig>& buses) const noexcept {
  if (buses.size() > kMaxBusLanes) return false;
  for (size_t i = 0; i < buses.size(); ++i) {
    if (buses[i].bus_id == 0 || !std::isfinite(buses[i].gain_db) || buses[i].gain_db < kFloorDb ||
        buses[i].gain_db > kMaxGainDb) {
      return false;
    }
    for (size_t j = i + 1; j < buses.size(); ++j) {
      if (buses[i].bus_id == buses[j].bus_id) return false;
    }
    // Same value rules as a lane send; targets are checked by validate_bus_graph.
    const std::vector<TrackLaneConfig::Send>& sends = buses[i].sends;
    if (sends.size() > mixing::ChannelStrip::kMaxSends) return false;
    for (size_t send_index = 0; send_index < sends.size(); ++send_index) {
      const TrackLaneConfig::Send& send = sends[send_index];
      if (send.bus_id == 0 || !std::isfinite(send.level_db) || send.level_db < kFloorDb ||
          send.level_db > kMaxGainDb) {
        return false;
      }
      for (size_t other = send_index + 1; other < sends.size(); ++other) {
        if (send.bus_id == sends[other].bus_id) return false;
      }
    }
  }
  return true;
}

mixing::ChannelStrip* TrackMixerRuntime::owned_strip_for(uint32_t track_id) noexcept {
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id) {
      return owned.strip.get();
    }
  }
  return nullptr;
}

mixing::ChannelStrip* TrackMixerRuntime::bound_strip_for(uint32_t track_id) const noexcept {
  for (const TrackStripBinding& binding : track_strip_bindings_) {
    if (binding.track_id == track_id) {
      return binding.strip;
    }
  }
  return nullptr;
}

bool TrackMixerRuntime::track_insert_processor_name(size_t lane_index, unsigned int insert_index,
                                                    std::string* out_name) const noexcept {
  if (out_name == nullptr) return false;
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr || lane_index >= lanes->size()) return false;
  const uint32_t track_id = (*lanes)[lane_index].track_id;
  if (track_id == 0) return false;
  mixing::ChannelStrip* strip = bound_strip_for(track_id);
  if (strip == nullptr) return false;
  for (const OwnedStrip& owned : owned_strips_) {
    if (owned.track_id != track_id || owned.strip.get() != strip) continue;
    const std::string* name = strip_insert_processor_name_at(owned.spec, insert_index);
    if (name == nullptr) return false;
    *out_name = *name;
    return true;
  }
  // No matching owned entry: the bound strip is externally bound
  // (bind_track_strip), which carries no retained spec to read a name from.
  return false;
}

bool TrackMixerRuntime::bus_insert_processor_name(size_t bus_index, unsigned int insert_index,
                                                  std::string* out_name) const noexcept {
  if (out_name == nullptr || bus_index >= bus_states_.size() ||
      bus_states_[bus_index].bus == nullptr) {
    return false;
  }
  if (insert_index >= bus_states_[bus_index].spec.inserts.size()) return false;
  *out_name = bus_states_[bus_index].spec.inserts[insert_index].processor_name;
  return true;
}

bool TrackMixerRuntime::bus_insert_processor_name_by_id(uint32_t bus_id, unsigned int insert_index,
                                                        std::string* out_name) const noexcept {
  const int bus_index = configured_bus_index(bus_id);
  return bus_index >= 0 &&
         bus_insert_processor_name(static_cast<size_t>(bus_index), insert_index, out_name);
}

bool TrackMixerRuntime::track_insert_automation_selector(uint32_t track_id,
                                                         uint32_t* out_selector) const noexcept {
  if (track_id == 0 || out_selector == nullptr) return false;
  const std::shared_ptr<const std::vector<TrackLaneConfig>> lanes = lanes_.control_current();
  if (lanes == nullptr) return false;
  for (size_t index = 0; index < lanes->size() && index < kMaxTrackLanes; ++index) {
    if ((*lanes)[index].track_id != track_id) continue;
    *out_selector = static_cast<uint32_t>(index);
    return true;
  }
  return false;
}

void TrackMixerRuntime::record_track_strip_binding(uint32_t track_id, mixing::ChannelStrip* strip) {
  if (track_id == 0) return;
  for (TrackStripBinding& binding : track_strip_bindings_) {
    if (binding.track_id == track_id) {
      binding.strip = strip;
      return;
    }
  }
  track_strip_bindings_.push_back(TrackStripBinding{track_id, strip});
}

mixing::ChannelStrip* TrackMixerRuntime::ensure_owned_strip_for(uint32_t track_id) {
  if (mixing::ChannelStrip* strip = owned_strip_for(track_id)) {
    return strip;
  }
  if (owned_strips_.size() >= kMaxTrackLanes) {
    return nullptr;
  }
  auto strip = std::make_unique<mixing::ChannelStrip>(mixing::ChannelStripConfig{
      0.0f, 0.0f, mixing::PanLaw::Linear0dB, 5.0f, mixing::EqPosition::PreFader, 0.0f, false});
  if (max_block_size_ > 0) {
    strip->prepare(sample_rate_, max_block_size_);
  }
  mixing::ChannelStrip* raw = strip.get();
  // Automation-seeded strip: no spec applied yet, so leave spec default (a later
  // set_track_strip will see a differing insert topology and rebuild).
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip), {}});
  return raw;
}

TrackMixerRuntime::BusState* TrackMixerRuntime::bus_state_for(uint32_t bus_id) noexcept {
  for (BusState& state : bus_states_) {
    if (state.bus_id == bus_id) return &state;
  }
  return nullptr;
}

const TrackMixerRuntime::BusState* TrackMixerRuntime::bus_state_for(
    uint32_t bus_id) const noexcept {
  for (const BusState& state : bus_states_) {
    if (state.bus_id == bus_id) return &state;
  }
  return nullptr;
}

int TrackMixerRuntime::configured_bus_index(uint32_t bus_id) const noexcept {
  if (bus_id == 0) return -1;
  for (size_t index = 0; index < bus_configs_.size(); ++index) {
    if (bus_configs_[index].bus_id == bus_id) return static_cast<int>(index);
  }
  return -1;
}

uint32_t TrackMixerRuntime::bus_insert_automation_selector(uint32_t bus_id) noexcept {
  if (bus_id == 0) return 0;
  for (size_t index = 0; index < bus_insert_selectors_.size(); ++index) {
    const BusInsertSelectorBinding& binding = bus_insert_selectors_[index];
    if (binding.active && binding.bus_id == bus_id) {
      return kInsertStripBusBase - static_cast<uint32_t>(index);
    }
  }
  if (bus_insert_selectors_.size() >= kMaxBusInsertSelectors) return 0;
  try {
    bus_insert_selectors_.push_back(BusInsertSelectorBinding{bus_id, true});
  } catch (...) {
    return 0;
  }
  return kInsertStripBusBase - static_cast<uint32_t>(bus_insert_selectors_.size() - 1);
}

uint32_t TrackMixerRuntime::bus_insert_automation_selector_for_clear(
    uint32_t bus_id) const noexcept {
  if (bus_id == 0) return 0;
  uint32_t tombstoned_selector = 0;
  for (size_t index = 0; index < bus_insert_selectors_.size(); ++index) {
    const BusInsertSelectorBinding& binding = bus_insert_selectors_[index];
    if (binding.bus_id != bus_id) continue;
    const uint32_t selector = kInsertStripBusBase - static_cast<uint32_t>(index);
    if (binding.active) {
      return selector;
    }
    if (tombstoned_selector == 0) {
      tombstoned_selector = selector;
    }
  }
  return tombstoned_selector;
}

uint32_t TrackMixerRuntime::bus_id_for_insert_automation_selector_for_clear(
    uint32_t selector) const noexcept {
  if (selector < kInsertStripBusMin || selector > kInsertStripBusBase) return 0;
  const size_t index = static_cast<size_t>(kInsertStripBusBase - selector);
  if (index >= bus_insert_selectors_.size()) return 0;
  return bus_insert_selectors_[index].bus_id;
}

uint32_t TrackMixerRuntime::bus_id_for_insert_automation_selector(
    uint32_t selector) const noexcept {
  if (selector < kInsertStripBusMin || selector > kInsertStripBusBase) return 0;
  const size_t index = static_cast<size_t>(kInsertStripBusBase - selector);
  if (index >= bus_insert_selectors_.size() || !bus_insert_selectors_[index].active) return 0;
  return bus_insert_selectors_[index].bus_id;
}

TrackMixerRuntime::BusState* TrackMixerRuntime::pannable_bus_state_for(uint32_t bus_id) noexcept {
  const int index = configured_bus_index(bus_id);
  if (index < 0 || layout_wider_than_stereo(bus_configs_[static_cast<size_t>(index)].layout)) {
    return nullptr;
  }
  return &bus_states_[static_cast<size_t>(index)];
}

void TrackMixerRuntime::prepare_lanes_from_snapshot(
    const std::vector<TrackLaneConfig>& lanes) noexcept {
  prepare_lanes_from_snapshot(lanes, nullptr);
}

void TrackMixerRuntime::prepare_lanes_from_snapshot(
    const std::vector<TrackLaneConfig>& lanes,
    const std::array<mixing::ChannelStrip*, kMaxTrackLanes>* candidate_strips) noexcept {
  // Audio thread, noexcept: LaneState holds AlignmentDelay vectors, so move banks, never copy.
  // Slots are staged in a fixed array and moved by identity; new identities reset delay history.
  std::array<LaneState, kMaxTrackLanes> previous = std::move(lane_states_);
  std::array<LaneState, kMaxTrackLanes> next;
  std::array<int, kMaxTrackLanes> matching_previous{};
  std::array<bool, kMaxTrackLanes> reserved_previous{};
  matching_previous.fill(-1);
  std::array<bool, kMaxTrackLanes> moved_previous{};
  const size_t previous_active_count =
      std::min(applied_lane_count_, static_cast<size_t>(kMaxTrackLanes));

  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const uint32_t track_id = lanes[lane_index].track_id;
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!reserved_previous[previous_index] && previous[previous_index].track_id == track_id) {
        matching_previous[lane_index] = static_cast<int>(previous_index);
        reserved_previous[previous_index] = true;
        break;
      }
    }
  }

  const auto reset_state = [this](LaneState& lane, uint32_t track_id) noexcept {
    lane.track_id = track_id;
    lane.fader_gain.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    lane.surround_glide.prepare(sample_rate_, 5.0f);
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    lane.monitor_mode = TrackMonitorMode::kOff;
    lane.strip = nullptr;
    // A new track identity must not inherit the previous occupant's clip delay audio.
    lane.clip_pdc_delay.reset();
    lane.surround_primed_channels = -1;
  };

  // Move all surviving identities first. Reserving their old slots before the
  // fallback pass prevents a new lane at an earlier position from stealing a
  // bank that a later lane still owns (old [A,B] -> new [C,A]).
  for (size_t next_index = 0; next_index < lanes.size(); ++next_index) {
    const int source_index = matching_previous[next_index];
    if (source_index < 0) continue;
    const size_t source = static_cast<size_t>(source_index);
    next[next_index] = std::move(previous[source]);
    moved_previous[source] = true;
    // A matching id in the inactive tail is a re-add. Keep its control state
    // (solo/mute/fader/strip) for compatibility, but never resurrect delayed
    // audio that was in flight when the track was removed.
    if (source >= previous_active_count) next[next_index].clip_pdc_delay.reset();
  }

  // Fill new configured slots from blank old banks first, preserving retired
  // control metadata in the inactive tail whenever capacity permits. If all
  // banks are occupied, a retired state is evicted for the genuinely new id.
  for (size_t next_index = 0; next_index < lanes.size(); ++next_index) {
    if (matching_previous[next_index] >= 0) continue;
    int source_index = -1;
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!moved_previous[previous_index] && previous[previous_index].track_id == 0) {
        source_index = static_cast<int>(previous_index);
        break;
      }
    }
    if (source_index < 0) {
      for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
        if (!moved_previous[previous_index]) {
          source_index = static_cast<int>(previous_index);
          break;
        }
      }
    }
    if (source_index >= 0) {
      const size_t source = static_cast<size_t>(source_index);
      next[next_index] = std::move(previous[source]);
      moved_previous[source] = true;
    }
    reset_state(next[next_index], lanes[next_index].track_id);
  }

  // Retain retired identities in the inactive tail so their public mixer state
  // follows a temporary removal. Their raw clip bank is explicitly flushed.
  size_t next_index = lanes.size();
  for (; next_index < next.size(); ++next_index) {
    int source_index = -1;
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!moved_previous[previous_index] && previous[previous_index].track_id != 0) {
        source_index = static_cast<int>(previous_index);
        break;
      }
    }
    if (source_index >= 0) {
      const size_t source = static_cast<size_t>(source_index);
      next[next_index] = std::move(previous[source]);
      moved_previous[source] = true;
      next[next_index].clip_pdc_delay.reset();
      continue;
    }
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!moved_previous[previous_index]) {
        source_index = static_cast<int>(previous_index);
        break;
      }
    }
    if (source_index >= 0) {
      const size_t source = static_cast<size_t>(source_index);
      next[next_index] = std::move(previous[source]);
      moved_previous[source] = true;
    }
    reset_state(next[next_index], 0);
  }

  lane_states_ = std::move(next);
  if (candidate_strips != nullptr) {
    for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
      lane_states_[lane_index].strip = (*candidate_strips)[lane_index];
    }
  }
  applied_lane_snapshot_ = &lanes;
  applied_lane_count_ = lanes.size();
  // Slot identities were restaged, so no published reading still belongs to its slot.
  for (InsertGainReductionBoard& board : lane_insert_gr_boards_) board.clear();
}

void TrackMixerRuntime::remap_lane_insert_automations(
    const std::vector<TrackLaneConfig>& lanes) noexcept {
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.assigned || slot.is_bus) continue;
    const uint32_t old_track_id =
        slot.index < lane_states_.size() ? lane_states_[slot.index].track_id : 0;
    size_t next_index = 0;
    while (next_index < lanes.size() && lanes[next_index].track_id != old_track_id) {
      ++next_index;
    }
    if (old_track_id == 0 || next_index == lanes.size()) {
      slot.active = false;
      slot.assigned = false;
      continue;
    }
    slot.index = next_index;
    // The current/target pair and active state belong to the track identity,
    // so leave them intact across a lane reorder or republish. An explicit
    // settle_smoothers() call is the opt-in path for snapping to target.
  }
}

void TrackMixerRuntime::configure_lane_sends(const std::vector<TrackLaneConfig>& lanes,
                                             uint32_t only_track_id) {
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const TrackLaneConfig& config = lanes[lane_index];
    if (only_track_id != 0 && config.track_id != only_track_id) continue;
    mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    if (!strip && !config.sends.empty()) {
      strip = ensure_owned_strip_for(config.track_id);
      lane_states_[lane_index].strip = strip;
      if (strip != nullptr) {
        // Send-seeded strips bypass bind_track_strip, so mirror them into the
        // control-thread binding table here.
        record_track_strip_binding(config.track_id, strip);
      }
    }
    if (!strip) continue;

    if (config.sends.size() > mixing::ChannelStrip::kMaxSends) {
      throw std::invalid_argument("track send count exceeds the strip send cap");
    }
    std::array<mixing::SendConfig, mixing::ChannelStrip::kMaxSends> configs{};
    for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
      const TrackLaneConfig::Send& send = config.sends[send_index];
      if (bus_state_for(send.bus_id) == nullptr) {
        throw std::invalid_argument("track send references an unknown bus");
      }
      configs[send_index] =
          mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f};
    }
    mixing::ChannelStrip::PreparedSends prepared;
    if (!strip->prepare_sends(configs.data(), config.sends.size(), prepared)) {
      throw std::bad_alloc();
    }
    strip->commit_sends(prepared);
  }
}

void TrackMixerRuntime::reset_processing() noexcept {
  // Adopt the published lanes first so a strip bound since the last block is reset too.
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes != nullptr && !scratch_.empty() && lanes != applied_lane_snapshot_) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.strip != nullptr) lane.strip->reset_processing();
  }
  for (BusState& bus : bus_states_) {
    bus.eq.reset();
    if (bus.bus) bus.bus->reset_processing();
  }
  for (auto& sends : bus_sends_) {
    for (auto& send : sends) {
      if (send) send->reset();
    }
  }
  // Also clears key_edge_delays_ and every lane/bus alignment bank.
  flush_pdc_delays();
  flush_clip_pdc_delays();
}

}  // namespace sonare::engine
