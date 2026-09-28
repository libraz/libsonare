#include "engine/track_mixer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>

#include "engine/track_mixer_internal.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mixing/channel_strip_eq.h"
#include "mixing/pan_law.h"
#include "util/constants.h"
#include "util/db.h"

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

bool strip_inserts_equal(const std::vector<mixing::api::Insert>& a,
                         const std::vector<mixing::api::Insert>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].slot != b[i].slot || a[i].processor_name != b[i].processor_name ||
        a[i].params_json != b[i].params_json || a[i].sidechain_key != b[i].sidechain_key) {
      return false;
    }
  }
  return true;
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

}  // namespace

bool TrackMixerRuntime::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  if (!lane_config_valid(lanes)) return false;
  const auto snapshot = std::make_shared<const std::vector<TrackLaneConfig>>(std::move(lanes));
  if (!lanes_.publish(snapshot)) return false;
  clear_lane_insert_automations();
  acquire_lanes();
  prepare_lanes_from_snapshot(*snapshot);
  try {
    configure_lane_sends(*snapshot);
  } catch (...) {
    return false;
  }
  if (!recompute_lane_pdc(*snapshot)) return false;
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
  const size_t binding_count = sidechain_binding_count_.load(std::memory_order_relaxed);
  std::array<bool, kMaxSidechainBindings> drop{};
  std::array<KeyEdge, kMaxSidechainBindings> key_edges{};
  size_t key_edge_count = 0;
  for (size_t i = 0; i < binding_count; ++i) {
    const SidechainBinding& binding = sidechain_bindings_[i];
    const auto target_kind =
        static_cast<SidechainTargetKind>(binding.target_kind.load(std::memory_order_relaxed));
    if (target_kind == SidechainTargetKind::Lane) continue;
    const uint32_t target_id = binding.target_id.load(std::memory_order_relaxed);
    const uint32_t source_id = binding.source_id.load(std::memory_order_relaxed);
    const bool bus_source = binding.source_kind.load(std::memory_order_relaxed) ==
                            static_cast<uint8_t>(SidechainSourceKind::Bus);
    if (bus_source && !declared(source_id)) drop[i] = true;
    if (target_kind == SidechainTargetKind::Bus) {
      const int previous = configured_bus_index(target_id);
      const mixing::FxBus* fx =
          previous >= 0 ? bus_states_[static_cast<size_t>(previous)].bus.get() : nullptr;
      if (!declared(target_id) || fx == nullptr ||
          binding.insert_index.load(std::memory_order_relaxed) >= fx->num_inserts()) {
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
    view.latency_q8[index] = fx != nullptr ? fx->latency_samples_q8() : 0;
  }
  PdcPlan plan;
  if (!plan_pdc(lanes, view, &plan)) return false;
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
  if (any_move) staging = std::make_unique<std::array<BusState, kMaxBusLanes>>();
  for (size_t index = 0; index < bus_states_.size(); ++index) {
    if (kept[index]) continue;
    if (moved_out[index]) {
      (*staging)[index].eq.prepare(sample_rate_, max_block_size_);
      transfer_bus_state(bus_states_[index], (*staging)[index]);
    }
    retire_bus_state(bus_states_[index]);
  }
  bus_configs_ = std::move(buses);
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
      // The delay line held another bus's history; recompute_lane_pdc re-derives its length.
      bus_pdc_delays_[index].reset();
      for (size_t edge = 0; edge < kBusEdgesPerBus; ++edge) {
        bus_edge_delays_[index * kBusEdgesPerBus + edge].reset();
      }
    }
    state.bus_id = bus_configs_[index].bus_id;
    state.gain.prepare(sample_rate_, 5.0f);
    state.gain.reset(db_to_linear(bus_configs_[index].gain_db));
    // Re-prepare the trim/width smoothers for the current rate without
    // disturbing any value a prior set_bus_strip already applied.
    state.input_trim_gain.prepare(sample_rate_, 5.0f);
    if (max_block_size_ > 0) {
      state.width.prepare(sample_rate_, max_block_size_);
    }
    if (!state.bus) {
      state.bus = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
    }
    state.bus->set_channel_layout(bus_configs_[index].layout);
    if (max_block_size_ > 0) {
      state.bus->prepare(sample_rate_, max_block_size_);
    }
  }
  refresh_bus_graph();
  // Control-side snapshot: this is a control-thread structural change, and
  // current() is the audio thread's view (see set_track_channel_delay_samples).
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    try {
      configure_lane_sends(*lanes);
    } catch (...) {
      return false;
    }
    // Declaring or retiring a bus changes which bus latencies feed the master
    // sum, so the bus-stage alignment has to follow the bus list.
    if (!recompute_lane_pdc(*lanes)) return false;
  }
  return true;
}

void TrackMixerRuntime::transfer_bus_state(BusState& from, BusState& to) {
  to.bus_id = from.bus_id;
  to.gain = from.gain;
  to.input_trim_gain = from.input_trim_gain;
  to.width.set_width(from.width.width());
  to.width.reset();
  to.polarity_left.store(from.polarity_left.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
  to.polarity_right.store(from.polarity_right.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
  to.panner.set_pan_mode(from.panner.pan_mode());
  to.panner.set_pan_law(from.panner.pan_law());
  to.panner.set_pan(from.panner.pan());
  to.panner.set_dual_pan(from.panner.dual_pan_left(), from.panner.dual_pan_right());
  to.panner.reset();
  for (size_t band = 0; band < mastering::eq::ParametricEq::kMaxBands; ++band) {
    to.eq.set_band(band, from.eq.band(band));
  }
  to.eq.reset();
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
        configure_lane_sends(*lanes);
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
        configure_lane_sends(*lanes);
      } catch (...) {
        return false;
      }
      if (!recompute_lane_pdc(*lanes)) return false;
    }
    return true;
  }
  return false;
}

bool TrackMixerRuntime::set_track_strip(uint32_t track_id, const mixing::api::Strip& spec) {
  if (track_id == 0 || !strip_eq_acceptable(spec.eq, sample_rate_)) return false;

  // In-place fast path: when a strip already exists for this track and only its
  // smoothable scalars changed (identical insert topology), retarget the existing
  // strip's parameters instead of rebuilding it. A rebuild constructs a fresh
  // strip whose fader/pan/trim smoothers settle straight to the new value, so a
  // live gain/pan edit would jump (an audible click); an in-place update keeps
  // the smoother state so the change ramps. PDC is recomputed in case the channel
  // delay changed; the strip pointer is unchanged so the lane binding stays valid.
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
  const bool rebuild = !strip_inserts_equal(state->spec.inserts, bus.inserts);
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
  for (size_t i = sidechain_binding_count_.load(std::memory_order_relaxed); i > 0; --i) {
    const SidechainBinding& binding = sidechain_bindings_[i - 1];
    if (binding.target_kind.load(std::memory_order_relaxed) ==
            static_cast<uint8_t>(SidechainTargetKind::Bus) &&
        binding.target_id.load(std::memory_order_relaxed) == bus_id &&
        binding.insert_index.load(std::memory_order_relaxed) >= insert_count) {
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
  send_source_scratch_.assign(2u * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
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
    // Same time constant as the stereo pan smoother, so a surround placement
    // glides over the same interval a stereo pan does.
    for (rt::ParamSmoother& plane_gain : lane.surround_gain) {
      plane_gain.prepare(sample_rate_, 5.0f);
      plane_gain.reset(0.0f);
    }
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
    recompute_lane_pdc(*lanes);
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
}

void TrackMixerRuntime::settle_smoothers() noexcept {
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.reset(lane.fader_gain.target());
    lane.pan.reset(lane.pan.target());
    lane.gate.reset(lane.gate.target());
    // The surround scatter gains are smoothers too now, so a pre-roll settle
    // has to quiesce them for the same reason it quiesces the fader: otherwise
    // the first audible block glides into placement instead of opening at it.
    for (rt::ParamSmoother& plane_gain : lane.surround_gain) {
      plane_gain.reset(plane_gain.target());
    }
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

void TrackMixerRuntime::flush_pdc_delays() noexcept {
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : bus_pdc_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : bus_edge_delays_) {
    delay.reset();
  }
  for (mixing::AlignmentDelay& delay : key_edge_delays_) {
    delay.reset();
  }
  master_pdc_delay_.reset();
}

uint64_t TrackMixerRuntime::pdc_storage_generation() const noexcept {
  uint64_t total = master_pdc_delay_.storage_generation();
  for (const mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : bus_pdc_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : bus_edge_delays_) {
    total += delay.storage_generation();
  }
  for (const mixing::AlignmentDelay& delay : key_edge_delays_) {
    total += delay.storage_generation();
  }
  return total;
}

bool TrackMixerRuntime::lane_config_valid(
    const std::vector<TrackLaneConfig>& lanes) const noexcept {
  if (lanes.size() > kMaxTrackLanes) return false;
  for (size_t i = 0; i < lanes.size(); ++i) {
    if (lanes[i].track_id == 0) return false;
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

bool TrackMixerRuntime::validate_bus_graph(const std::vector<TrackBusConfig>& buses,
                                           const KeyEdge* key_edges, size_t key_edge_count,
                                           std::array<size_t, kMaxBusLanes>* order) noexcept {
  const size_t count = buses.size();
  if (count > kMaxBusLanes || order == nullptr) return false;
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  std::array<std::array<int, kMaxBusLanes>, kMaxBusLanes> edges{};
  std::array<int, kMaxBusLanes> indegree{};
  const auto add_edge = [&](int from, int to) {
    if (from < 0 || to < 0 || from == to) return false;
    ++edges[static_cast<size_t>(from)][static_cast<size_t>(to)];
    ++indegree[static_cast<size_t>(to)];
    return true;
  };
  for (size_t i = 0; i < count; ++i) {
    const int from = static_cast<int>(i);
    if (buses[i].output_bus_id != 0 && !add_edge(from, index_of(buses[i].output_bus_id))) {
      return false;
    }
    for (const TrackLaneConfig::Send& send : buses[i].sends) {
      if (!add_edge(from, index_of(send.bus_id))) return false;
    }
  }
  for (size_t i = 0; i < key_edge_count; ++i) {
    if (!add_edge(index_of(key_edges[i].source_bus), index_of(key_edges[i].target_bus))) {
      return false;
    }
  }
  std::array<bool, kMaxBusLanes> placed{};
  for (size_t position = 0; position < count; ++position) {
    size_t next = count;
    for (size_t i = 0; i < count && next == count; ++i) {
      if (!placed[i] && indegree[i] == 0) next = i;
    }
    if (next == count) return false;
    placed[next] = true;
    (*order)[position] = next;
    for (size_t j = 0; j < count; ++j) indegree[j] -= edges[next][j];
  }
  return true;
}

size_t TrackMixerRuntime::collect_key_edges(std::array<KeyEdge, kMaxSidechainBindings>& out,
                                            size_t skip) const noexcept {
  size_t edges = 0;
  const size_t count = sidechain_binding_count_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    const SidechainBinding& binding = sidechain_bindings_[i];
    if (i == skip ||
        binding.target_kind.load(std::memory_order_relaxed) !=
            static_cast<uint8_t>(SidechainTargetKind::Bus) ||
        binding.source_kind.load(std::memory_order_relaxed) !=
            static_cast<uint8_t>(SidechainSourceKind::Bus)) {
      continue;
    }
    out[edges++] = KeyEdge{binding.source_id.load(std::memory_order_relaxed),
                           binding.target_id.load(std::memory_order_relaxed)};
  }
  return edges;
}

void TrackMixerRuntime::build_routes(const std::vector<TrackBusConfig>& buses,
                                     const std::array<bool, kMaxSidechainBindings>& skip,
                                     std::array<BusRoute, kMaxBusLanes>* routes) const noexcept {
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    if (bus_id == 0) return -1;
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  const size_t binding_count = sidechain_binding_count_.load(std::memory_order_relaxed);
  *routes = {};
  for (size_t bus_index = 0; bus_index < buses.size(); ++bus_index) {
    const TrackBusConfig& config = buses[bus_index];
    BusRoute& route = (*routes)[bus_index];
    route.output_index = index_of(config.output_bus_id);
    route.send_count = config.sends.size();
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      route.send_index[send_index] = index_of(config.sends[send_index].bus_id);
      route.any_pre_send =
          route.any_pre_send || config.sends[send_index].timing == mixing::SendTiming::PreFader;
    }
    for (size_t i = 0; i < binding_count; ++i) {
      const SidechainBinding& binding = sidechain_bindings_[i];
      route.key_source =
          route.key_source || (!skip[i] &&
                               binding.source_kind.load(std::memory_order_relaxed) ==
                                   static_cast<uint8_t>(SidechainSourceKind::Bus) &&
                               binding.source_id.load(std::memory_order_relaxed) == config.bus_id);
    }
  }
}

void TrackMixerRuntime::refresh_bus_graph() noexcept {
  std::array<KeyEdge, kMaxSidechainBindings> edges{};
  const size_t edge_count = collect_key_edges(edges, kMaxSidechainBindings);
  std::array<size_t, kMaxBusLanes> order{};
  // Every writer validated this state first, so the order always resolves.
  if (!validate_bus_graph(bus_configs_, edges.data(), edge_count, &order)) return;
  bus_order_ = order;
  build_routes(bus_configs_, {}, &bus_routes_);
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
  const std::array<LaneState, kMaxTrackLanes> previous = lane_states_;
  std::array<LaneState, kMaxTrackLanes> next = lane_states_;
  std::array<bool, kMaxTrackLanes> used_previous{};

  const auto reset_state = [this](LaneState& lane, uint32_t track_id) noexcept {
    lane.track_id = track_id;
    lane.fader_gain.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    for (rt::ParamSmoother& plane_gain : lane.surround_gain) {
      plane_gain.prepare(sample_rate_, 5.0f);
    }
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    lane.monitor_mode = TrackMonitorMode::kOff;
    lane.strip = nullptr;
    for (rt::ParamSmoother& plane_gain : lane.surround_gain) plane_gain.reset(0.0f);
    lane.surround_primed_channels = -1;
  };

  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const uint32_t track_id = lanes[lane_index].track_id;
    const auto previous_state =
        std::find_if(previous.begin(), previous.end(),
                     [track_id](const LaneState& state) { return state.track_id == track_id; });
    if (previous_state != previous.end()) {
      next[lane_index] = *previous_state;
      used_previous[static_cast<size_t>(std::distance(previous.begin(), previous_state))] = true;
      continue;
    }

    reset_state(next[lane_index], track_id);
  }

  size_t inactive_index = lanes.size();
  for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
    if (used_previous[previous_index] || previous[previous_index].track_id == 0) continue;
    if (inactive_index >= next.size()) break;
    next[inactive_index++] = previous[previous_index];
  }

  for (; inactive_index < next.size(); ++inactive_index) {
    reset_state(next[inactive_index], 0);
  }

  lane_states_ = next;
  applied_lane_snapshot_ = &lanes;
}

bool TrackMixerRuntime::recompute_lane_pdc(const std::vector<TrackLaneConfig>& lanes) noexcept {
  PdcPlan plan;
  if (!plan_pdc(lanes, current_bus_graph_view(), &plan)) return false;
  return apply_pdc(plan);
}

TrackMixerRuntime::BusGraphView TrackMixerRuntime::current_bus_graph_view() const noexcept {
  BusGraphView view;
  view.buses = &bus_configs_;
  view.order = bus_order_;
  view.routes = bus_routes_;
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    const mixing::FxBus* bus = bus_states_[bus_index].bus.get();
    view.latency_q8[bus_index] = bus != nullptr ? bus->latency_samples_q8() : 0;
  }
  return view;
}

bool TrackMixerRuntime::plan_pdc(const std::vector<TrackLaneConfig>& lanes,
                                 const BusGraphView& view, PdcPlan* plan) const noexcept {
  // Lane stage: the widest strip latency. Every lane leaves process_lane_strip
  // at this offset, and every path the lane's audio then takes is tapped from
  // there -- the direct master sum, the output-bus routing and the post-fader
  // sends all read the aligned lane buffer, while a pre-fader send reads the
  // strip's earlier tap through lane_pre_send_pdc_delays_ below. One lane
  // timebase, three exits.
  int max_strip_q8 = 0;
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    if (strip != nullptr) {
      max_strip_q8 = std::max(max_strip_q8, strip->latency_samples_q8());
    }
  }
  *plan = PdcPlan{};
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    plan->lane_q8[lane_index] = max_strip_q8 - (strip != nullptr ? strip->latency_samples_q8() : 0);
    // Pre-fader send stage: the same target offset, measured from the earlier
    // tap. A strip's pre-fader latency is what it has accrued by the time the
    // pre tap is taken (channel delay + pre inserts), so this bank carries the
    // rest of the widest strip latency -- including the strip's own
    // post-insert chain, which the lane's output passes through and the pre
    // tap does not.
    plan->lane_pre_q8[lane_index] =
        max_strip_q8 - (strip != nullptr ? strip->pre_fader_latency_samples_q8() : 0);
  }

  // Bus stage along the order: in(b) is the latest arrival over the lane stage
  // and every incoming edge (output, send, bus-sourced key), out(b) adds the
  // bus's own chain latency, and every edge carries the difference.
  const std::vector<TrackBusConfig>& buses = *view.buses;
  const auto index_of = [&buses](uint32_t bus_id) -> int {
    for (size_t i = 0; i < buses.size(); ++i) {
      if (buses[i].bus_id == bus_id) return static_cast<int>(i);
    }
    return -1;
  };
  const size_t bus_count = buses.size();
  std::array<int, kMaxBusLanes> bus_in{};
  std::array<int, kMaxBusLanes> bus_out{};
  bus_in.fill(max_strip_q8);
  int master_in = max_strip_q8;
  const size_t binding_count = sidechain_binding_count_.load(std::memory_order_relaxed);
  const auto live_bus_key = [&](size_t i, uint32_t source_bus) {
    const SidechainBinding& binding = sidechain_bindings_[i];
    return !view.skip_binding[i] &&
           binding.source_kind.load(std::memory_order_relaxed) ==
               static_cast<uint8_t>(SidechainSourceKind::Bus) &&
           binding.source_id.load(std::memory_order_relaxed) == source_bus;
  };
  for (size_t order_index = 0; order_index < bus_count; ++order_index) {
    const size_t bus_index = view.order[order_index];
    bus_out[bus_index] = bus_in[bus_index] + view.latency_q8[bus_index];
    const int out = bus_out[bus_index];
    const BusRoute& route = view.routes[bus_index];
    int& output_in =
        route.output_index < 0 ? master_in : bus_in[static_cast<size_t>(route.output_index)];
    output_in = std::max(output_in, out);
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      if (route.send_index[send_index] < 0) continue;
      int& in = bus_in[static_cast<size_t>(route.send_index[send_index])];
      in = std::max(in, out);
    }
    for (size_t i = 0; i < binding_count; ++i) {
      if (!live_bus_key(i, buses[bus_index].bus_id)) continue;
      const SidechainBinding& binding = sidechain_bindings_[i];
      const auto target_kind = binding.target_kind.load(std::memory_order_relaxed);
      if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Master)) {
        master_in = std::max(master_in, out);
      } else if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus)) {
        const int target = index_of(binding.target_id.load(std::memory_order_relaxed));
        if (target >= 0) {
          int& in = bus_in[static_cast<size_t>(target)];
          in = std::max(in, out);
        }
      }
    }
  }
  const auto input_of = [&](int index) {
    return index < 0 ? master_in : bus_in[static_cast<size_t>(index)];
  };
  for (size_t bus_index = 0; bus_index < bus_count; ++bus_index) {
    plan->bus_in_q8[bus_index] = bus_in[bus_index] - max_strip_q8;
    const BusRoute& route = view.routes[bus_index];
    plan->edge_q8[bus_index * kBusEdgesPerBus] = input_of(route.output_index) - bus_out[bus_index];
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      if (route.send_index[send_index] < 0) continue;
      plan->edge_q8[bus_index * kBusEdgesPerBus + 1 + send_index] =
          input_of(route.send_index[send_index]) - bus_out[bus_index];
    }
  }
  plan->master_q8 = master_in - max_strip_q8;
  // Key edges: in(target) - L from a track, in(target) - out(source) from a bus.
  for (size_t i = 0; i < binding_count; ++i) {
    if (view.skip_binding[i]) continue;
    const SidechainBinding& binding = sidechain_bindings_[i];
    const auto target_kind = binding.target_kind.load(std::memory_order_relaxed);
    if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Lane)) continue;
    int target = -1;
    if (target_kind == static_cast<uint8_t>(SidechainTargetKind::Bus)) {
      target = index_of(binding.target_id.load(std::memory_order_relaxed));
      if (target < 0) continue;
    }
    int source_out = max_strip_q8;
    if (binding.source_kind.load(std::memory_order_relaxed) ==
        static_cast<uint8_t>(SidechainSourceKind::Bus)) {
      const int source = index_of(binding.source_id.load(std::memory_order_relaxed));
      if (source < 0) continue;
      source_out = bus_out[static_cast<size_t>(source)];
    }
    plan->key_q8[binding.key_slot.load(std::memory_order_relaxed)] = input_of(target) - source_out;
  }
  // What the engine advertises to the host: the master input's arrival.
  plan->latency_q8 = master_in;

  // A delay line past its ceiling would clamp silently and misalign the mix,
  // so such a configuration is refused instead.
  constexpr int kCapQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  const auto within = [](const auto& values) {
    return std::all_of(values.begin(), values.end(), [](int v) { return v <= kCapQ8; });
  };
  return within(plan->lane_q8) && within(plan->lane_pre_q8) && within(plan->bus_in_q8) &&
         within(plan->edge_q8) && within(plan->key_q8) && plan->master_q8 <= kCapQ8;
}

bool TrackMixerRuntime::apply_pdc(const PdcPlan& plan) noexcept {
  bool ok = true;
  for (size_t i = 0; i < lane_pdc_delays_.size(); ++i) {
    ok = lane_pdc_delays_[i].try_set_delay_samples_q8(plan.lane_q8[i]) && ok;
    ok = lane_pre_send_pdc_delays_[i].try_set_delay_samples_q8(plan.lane_pre_q8[i]) && ok;
  }
  for (size_t i = 0; i < bus_pdc_delays_.size(); ++i) {
    ok = bus_pdc_delays_[i].try_set_delay_samples_q8(plan.bus_in_q8[i]) && ok;
  }
  for (size_t i = 0; i < bus_edge_delays_.size(); ++i) {
    ok = bus_edge_delays_[i].try_set_delay_samples_q8(plan.edge_q8[i]) && ok;
  }
  for (size_t i = 0; i < key_edge_delays_.size(); ++i) {
    ok = key_edge_delays_[i].try_set_delay_samples_q8(plan.key_q8[i]) && ok;
  }
  ok = master_pdc_delay_.try_set_delay_samples_q8(plan.master_q8) && ok;
  latency_samples_q8_ = plan.latency_q8;
  return ok;
}

void TrackMixerRuntime::configure_lane_sends(const std::vector<TrackLaneConfig>& lanes) {
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const TrackLaneConfig& config = lanes[lane_index];
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

    strip->clear_sends();
    for (const TrackLaneConfig::Send& send : config.sends) {
      if (bus_state_for(send.bus_id) == nullptr) {
        throw std::invalid_argument("track send references an unknown bus");
      }
      strip->add_send(
          mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f});
    }
  }
}

}  // namespace sonare::engine
