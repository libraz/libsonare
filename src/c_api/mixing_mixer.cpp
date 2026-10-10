#include <algorithm>
#include <functional>
#include <unordered_map>

#include "c_api/mixing_internal.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mixing/channel_strip_eq.h"
#include "util/json.h"

using namespace sonare_c_mixing_detail;

namespace {

std::vector<std::string> unique_vca_members(const std::vector<std::string>& members) {
  std::vector<std::string> unique;
  unique.reserve(members.size());
  for (const auto& member : members) {
    if (std::find(unique.begin(), unique.end(), member) == unique.end()) {
      unique.push_back(member);
    }
  }
  return unique;
}

// Rejects a non-finite (NaN / Inf) sample in one channel of a process block.
// The routing graph, the inserts, and the meters all propagate a NaN silently,
// so a fabricated mix would otherwise be reported as SONARE_OK. Uniform with the
// non-finite policy of validate_audio_params. The scan is O(num_samples) per
// channel, which this entry point can afford: it lazily compiles and allocates
// (see sonare_mixer_process_stereo) and is therefore not an audio-thread entry.
// The scene bag of @p insert with every realtime parameter @p live has moved
// since construction (consumed automation) written back under its descriptor
// key, so a reload rebuilds the settled state. When a construction-time alias in
// the bag would shadow a written value, the one alias whose removal lets the bag
// reproduce every live value is dropped.
std::string live_insert_params(const sonare::mixing::api::Insert& insert,
                               const sonare::rt::ProcessorBase& live, double build_sample_rate) {
  namespace json = sonare::util::json;
  const std::vector<sonare::rt::ParamDescriptor> descriptors = live.parameter_descriptors();
  std::vector<std::string> moved_keys;
  json::Value bag =
      insert.params_json.empty() ? json::Value(json::Object()) : json::parse(insert.params_json);
  for (const auto& descriptor : descriptors) {
    float constructed = 0.0f;
    float applied = 0.0f;
    if (!live.constructed_parameter_value(descriptor.id, &constructed) ||
        !live.last_applied_parameter_value(descriptor.id, &applied) || applied == constructed) {
      continue;
    }
    bag.as_object()[descriptor.key] = json::Value(static_cast<double>(applied));
    moved_keys.push_back(descriptor.key);
  }
  if (moved_keys.empty()) return insert.params_json;

  const auto reproduces = [&](const json::Value& candidate) {
    const auto rebuilt = sonare::mastering::api::make_insert(
        insert.processor_name, json::dump(candidate), nullptr,
        sonare::resource::kDefaultProjectImportResourceLimits, build_sample_rate);
    if (!rebuilt) return false;
    for (const auto& descriptor : descriptors) {
      float want = 0.0f;
      float got = 0.0f;
      if (!live.last_applied_parameter_value(descriptor.id, &want)) continue;
      if (!rebuilt->constructed_parameter_value(descriptor.id, &got) ||
          std::abs(got - want) > 1.0e-4f * std::max(1.0f, std::abs(want))) {
        return false;
      }
    }
    return true;
  };
  if (!reproduces(bag)) {
    for (const auto& entry : bag.as_object()) {
      if (std::find(moved_keys.begin(), moved_keys.end(), entry.first) != moved_keys.end()) {
        continue;
      }
      json::Value candidate = bag;
      candidate.as_object().erase(entry.first);
      if (reproduces(candidate)) return json::dump(candidate);
    }
  }
  return json::dump(bag);
}

bool block_finite(const float* samples, size_t num_samples) noexcept {
  for (size_t index = 0; index < num_samples; ++index) {
    if (!finite(samples[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace

size_t sonare_mixer_strip_count(const SonareMixer* mixer) {
  if (!mixer) {
    return 0;
  }
  return mixer->strips.size();
}

SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->strips.size();
  return SONARE_OK;
}

SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string bus_id = id;
  for (const auto& bus : mixer->buses) {
    if (bus.id == bus_id) {
      return SONARE_ERROR_INVALID_PARAMETER;  // duplicate bus id
    }
  }
  mixer->buses.emplace_back(bus_id, role != nullptr ? std::string(role) : std::string("aux"));
  mixer->compiled_dirty = true;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_set_output_bus(SonareMixer* mixer, const char* source_id,
                                        const char* bus_id) {
  SONARE_C_API_ENTRY;
  if (!mixer || !source_id || !bus_id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string source = source_id;
  const std::string destination = bus_id;
  const auto refuse = [](const std::string& message) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, message);
  };

  const std::string master = resolve_master_bus_id(mixer->buses);
  const auto is_declared_bus = [&](const std::string& id) {
    return std::any_of(mixer->buses.begin(), mixer->buses.end(),
                       [&](const sonare::mixing::api::Bus& bus) { return bus.id == id; });
  };
  const bool source_is_strip =
      std::any_of(mixer->strips.begin(), mixer->strips.end(),
                  [&](const std::unique_ptr<SonareStrip>& strip) { return strip->id == source; });
  if (source == master) {
    refuse("set_output_bus: the master '" + source + "' cannot be a source");
  }
  if (!source_is_strip && !is_declared_bus(source)) {
    refuse("set_output_bus: unknown source '" + source + "'");
  }
  if (destination != master && !is_declared_bus(destination)) {
    const bool destination_is_strip = std::any_of(
        mixer->strips.begin(), mixer->strips.end(),
        [&](const std::unique_ptr<SonareStrip>& strip) { return strip->id == destination; });
    refuse(destination_is_strip
               ? "set_output_bus: destination '" + destination + "' is a strip, not a bus"
               : "set_output_bus: unknown destination '" + destination + "'");
  }
  if (source == destination) {
    refuse("set_output_bus: '" + source + "' cannot route to itself");
  }

  // Edge set after the edit: every other main connection, the new one, and every
  // send tap (a send is an edge for cycle purposes).
  std::unordered_map<std::string, std::vector<std::string>> edges;
  for (const auto& connection : mixer->connections) {
    if (connection.source != source) {
      edges[connection.source].push_back(connection.destination);
    }
  }
  edges[source].push_back(destination);
  for (const auto& strip : mixer->strips) {
    for (const auto& send : strip->scene_strip.sends) {
      edges[strip->id].push_back(send.destination_bus_id);
    }
  }
  // Any new cycle runs through the new edge, so search for a path back to the
  // source from the destination.
  std::vector<std::string> path{source, destination};
  std::unordered_map<std::string, bool> visited;
  const std::function<bool(const std::string&)> reaches_source = [&](const std::string& node) {
    if (node == source) return true;
    if (visited[node]) return false;
    visited[node] = true;
    const auto it = edges.find(node);
    if (it == edges.end()) return false;
    for (const auto& next : it->second) {
      path.push_back(next);
      if (reaches_source(next)) return true;
      path.pop_back();
    }
    return false;
  };
  if (reaches_source(destination)) {
    std::string text;
    for (const auto& node : path) {
      text += (text.empty() ? "" : " -> ") + node;
    }
    refuse("set_output_bus: routing '" + source + "' to '" + destination +
           "' closes a cycle: " + text);
  }

  mixer->connections.erase(std::remove_if(mixer->connections.begin(), mixer->connections.end(),
                                          [&](const sonare::mixing::api::Connection& connection) {
                                            return connection.source == source;
                                          }),
                           mixer->connections.end());
  mixer->connections.push_back({source, destination});
  mixer->compiled_dirty = true;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string bus_id = id;
  const auto before = mixer->buses.size();
  mixer->buses.erase(
      std::remove_if(mixer->buses.begin(), mixer->buses.end(),
                     [&](const sonare::mixing::api::Bus& bus) { return bus.id == bus_id; }),
      mixer->buses.end());
  if (mixer->buses.size() == before) {
    return SONARE_ERROR_INVALID_PARAMETER;  // no such bus
  }
  // The declaration and its DSP are one unit, so the record goes with it rather
  // than lingering to be re-adopted by a later bus that reuses the id.
  mixer->bus_dsp.erase(
      std::remove_if(mixer->bus_dsp.begin(), mixer->bus_dsp.end(),
                     [&](const std::unique_ptr<SonareBusDsp>& dsp) { return dsp->id == bus_id; }),
      mixer->bus_dsp.end());
  // Drop any connection that referenced the removed bus; otherwise the next
  // compile would try to wire an edge to/from a node that no longer exists.
  mixer->connections.erase(std::remove_if(mixer->connections.begin(), mixer->connections.end(),
                                          [&](const sonare::mixing::api::Connection& connection) {
                                            return connection.source == bus_id ||
                                                   connection.destination == bus_id;
                                          }),
                           mixer->connections.end());
  // Drop any strip send that targeted the removed bus from both the live strip
  // and the scene mirror. Otherwise build_and_compile would re-materialize the
  // orphaned destination as an implicit aux bus default-routed to master, so the
  // (now reverb-less) dry send would silently re-appear at the master and alter
  // the mix. Iterate sends back-to-front so erasing one does not skip the next.
  for (const auto& strip : mixer->strips) {
    auto& sends = strip->scene_strip.sends;
    for (size_t i = sends.size(); i-- > 0;) {
      if (sends[i].destination_bus_id == bus_id) {
        // remove_send keeps the live strip and scene mirror index-parallel.
        strip->strip.remove_send(i);
        sends.erase(sends.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }
  // A key taken from the removed bus would name a node the next compile cannot
  // find; the keyed insert falls back to its own detector, as an unkeyed one does.
  const auto drop_keys = [&](std::vector<sonare::mixing::api::Insert>& inserts) {
    for (auto& insert : inserts) {
      if (insert.sidechain_key == bus_id) insert.sidechain_key.clear();
    }
  };
  for (const auto& strip : mixer->strips) drop_keys(strip->scene_strip.inserts);
  for (auto& bus : mixer->buses) drop_keys(bus.inserts);
  mixer->compiled_dirty = true;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->buses.size();
  return SONARE_OK;
}

SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id || !finite(gain_db) || (member_count > 0 && !members)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string group_id = id;
  for (const auto& group : mixer->vca_groups) {
    if (group.id == group_id) {
      return SONARE_ERROR_INVALID_PARAMETER;  // duplicate group id
    }
  }
  sonare::mixing::api::VcaGroup group;
  group.id = group_id;
  group.gain_db = gain_db;
  group.members.reserve(member_count);
  for (size_t i = 0; i < member_count; ++i) {
    if (!members[i]) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const std::string member = members[i];
    if (std::find(group.members.begin(), group.members.end(), member) == group.members.end()) {
      group.members.push_back(member);
    }
  }
  // Apply the group's gain offset to the live ChannelStrip of each member, the
  // same control-only path scene load uses. A strip may belong to several VCA
  // groups, so this group's gain accumulates additively onto the strip's single
  // offset (matching the runtime VcaGroup delta semantics) rather than
  // overwriting any contribution from other groups.
  for (const auto& member : group.members) {
    for (const auto& strip : mixer->strips) {
      if (strip->id == member) {
        strip->strip.add_vca_group_offset_db(gain_db);
        break;
      }
    }
  }
  mixer->vca_groups.push_back(std::move(group));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_set_vca_group_gain_db(SonareMixer* mixer, const char* id, float gain_db) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id || !finite(gain_db)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string group_id = id;
  const auto it =
      std::find_if(mixer->vca_groups.begin(), mixer->vca_groups.end(),
                   [&](const sonare::mixing::api::VcaGroup& g) { return g.id == group_id; });
  if (it == mixer->vca_groups.end()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const float delta_db = gain_db - it->gain_db;
  if (delta_db != 0.0f) {
    for (const auto& member : it->members) {
      for (const auto& strip : mixer->strips) {
        if (strip->id == member) {
          strip->strip.add_vca_group_offset_db(delta_db);
          break;
        }
      }
    }
  }
  it->gain_db = gain_db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_set_vca_group_members(SonareMixer* mixer, const char* id,
                                               const char* const* members, size_t member_count) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id || (member_count > 0 && !members)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string group_id = id;
  const auto group =
      std::find_if(mixer->vca_groups.begin(), mixer->vca_groups.end(),
                   [&](const sonare::mixing::api::VcaGroup& g) { return g.id == group_id; });
  if (group == mixer->vca_groups.end()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  std::vector<std::string> next_members;
  next_members.reserve(member_count);
  for (size_t i = 0; i < member_count; ++i) {
    if (!members[i]) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const std::string member = members[i];
    if (std::find(next_members.begin(), next_members.end(), member) == next_members.end()) {
      next_members.push_back(member);
    }
  }

  for (const auto& strip : mixer->strips) {
    const bool was_member =
        std::find(group->members.begin(), group->members.end(), strip->id) != group->members.end();
    const bool is_member =
        std::find(next_members.begin(), next_members.end(), strip->id) != next_members.end();
    if (was_member != is_member) {
      strip->strip.add_vca_group_offset_db(is_member ? group->gain_db : -group->gain_db);
    }
  }
  group->members = std::move(next_members);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::string group_id = id;
  const auto it =
      std::find_if(mixer->vca_groups.begin(), mixer->vca_groups.end(),
                   [&](const sonare::mixing::api::VcaGroup& g) { return g.id == group_id; });
  if (it == mixer->vca_groups.end()) {
    return SONARE_ERROR_INVALID_PARAMETER;  // no such group
  }
  // Subtract only this group's contribution from each member's offset,
  // preserving any offset still owed by other VCA groups the strip belongs to
  // (mirrors the runtime VcaGroup::remove_member delta semantics).
  for (const auto& member : it->members) {
    for (const auto& strip : mixer->strips) {
      if (strip->id == member) {
        strip->strip.add_vca_group_offset_db(-it->gain_db);
        break;
      }
    }
  }
  mixer->vca_groups.erase(it);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_vca_group_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->vca_groups.size();
  return SONARE_OK;
}

SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index) {
  SONARE_C_API_ENTRY;
  if (!mixer || index >= mixer->strips.size()) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                                    "mixer: handle is required and index must be in range");
    return nullptr;
  }
  return mixer->strips[index].get();
}

SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  if (!mixer || !id) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                                    "mixer: handle and strip id are required");
    return nullptr;
  }
  SonareStrip* strip = find_strip(mixer, id);
  if (strip == nullptr) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_PARAMETER, "mixer: unknown strip id");
  }
  return strip;
}

SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size) {
  SONARE_C_API_ENTRY;
  if (!json) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                                    "mixer: scene JSON is required");
    return nullptr;
  }
  // A non-fatal channel, separate from last_error: a scene can load fine while still
  // carrying unknown keys or insert params no processor reads. Cleared on entry so a
  // stale warning from an earlier load never leaks into a later, clean one.
  sonare_c_detail::clear_last_warning();
  std::vector<std::string> ignored_param_notes;
  try {
    sonare::mixing::api::Scene scene;
    // A malformed document is InvalidFormat, as on the engine's scene entry point;
    // without this arm it would reach the std::exception tail as Unknown.
    try {
      scene = sonare::mixing::api::scene_from_json(json, &ignored_param_notes);
    } catch (const sonare::util::json::JsonError& e) {
      sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_FORMAT, e.what());
      return nullptr;
    }
    std::unique_ptr<SonareMixer> mixer(sonare_mixer_create(sample_rate, max_block_size));
    if (!mixer) {
      return nullptr;
    }
    for (const auto& scene_strip : scene.strips) {
      // Build with the scene's metering: the meters are sized when the strip is
      // constructed, so this is the one point where a scene can opt out of the
      // full LUFS + true-peak configuration.
      SonareStrip* strip = sonare_mixer_add_strip_ex(
          mixer.get(), scene_strip.id.c_str(), scene_strip.metering.enabled ? 1 : 0,
          scene_strip.metering.lufs ? 1 : 0, scene_strip.metering.true_peak ? 1 : 0,
          scene_strip.metering.true_peak_oversample);
      if (!strip) {
        return nullptr;
      }
      strip->scene_strip = scene_strip;
      strip->strip.set_input_trim_db(scene_strip.input_trim_db);
      strip->strip.set_fader_db(scene_strip.fader_db);
      strip->strip.set_vca_offset_db(scene_strip.vca_offset_db);
      strip->strip.set_pan(scene_strip.pan);
      strip->strip.set_width(scene_strip.width);
      strip->strip.set_muted(scene_strip.muted);
      strip->strip.set_soloed(scene_strip.soloed);
      strip->strip.set_solo_safe(scene_strip.solo_safe);
      strip->strip.set_pan_mode(to_pan_mode(scene_strip.pan_mode));
      strip->strip.set_dual_pan(scene_strip.dual_pan_left, scene_strip.dual_pan_right);
      strip->strip.set_pan_law(to_pan_law(scene_strip.pan_law));
      sonare::mixing::SurroundPanParams surround;
      surround.azimuth = scene_strip.surround_pan.azimuth;
      surround.elevation = scene_strip.surround_pan.elevation;
      surround.divergence = scene_strip.surround_pan.divergence;
      surround.lfe = scene_strip.surround_pan.lfe;
      surround.distance = scene_strip.surround_pan.distance;
      strip->strip.set_surround_pan_params(surround);
      strip->strip.set_polarity_invert(scene_strip.polarity_invert_left,
                                       scene_strip.polarity_invert_right);
      strip->strip.set_channel_delay_samples(scene_strip.channel_delay_samples);
      sonare::mixing::apply_strip_eq(strip->strip, scene_strip.eq, nullptr);
      for (const auto& insert : scene_strip.inserts) {
        std::vector<std::string> unknown_keys;
        auto processor = sonare::mastering::api::make_insert(
            insert.processor_name, insert.params_json, &unknown_keys,
            sonare::resource::kDefaultProjectImportResourceLimits,
            static_cast<double>(mixer->sample_rate));
        if (!processor) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "unknown insert processor: " + insert.processor_name +
                                            " (strip " + scene_strip.id + ")");
        }
        if (!unknown_keys.empty()) {
          std::string note = "insert '" + insert.processor_name + "' on strip '" + scene_strip.id +
                             "' ignored unknown param" + (unknown_keys.size() > 1 ? "s" : "") +
                             ": ";
          for (size_t k = 0; k < unknown_keys.size(); ++k) {
            if (k > 0) note += ", ";
            note += unknown_keys[k];
          }
          ignored_param_notes.push_back(std::move(note));
        }
        const bool spo = sonare::mastering::api::channel_policy(insert.processor_name) ==
                         sonare::mastering::api::ChannelPolicy::StereoPairOnly;
        if (insert.slot == sonare::mixing::api::InsertSlot::PreFader) {
          strip->strip.add_pre_insert(std::move(processor), spo);
        } else {
          strip->strip.add_post_insert(std::move(processor), spo);
        }
      }
      for (const auto& send : scene_strip.sends) {
        sonare::mixing::SendConfig config;
        config.send_db = send.send_db;
        config.timing = to_send_timing(send.timing);
        strip->strip.add_send(config);
      }
    }

    // Store routing topology. If the scene defines no master bus, synthesize one
    // so strips default-route to it.
    mixer->buses = scene.buses;
    mixer->vca_groups = scene.vca_groups;
    for (auto& group : mixer->vca_groups) {
      group.members = unique_vca_members(group.members);
    }
    mixer->connections = scene.connections;
    bool has_master = false;
    for (const auto& bus : mixer->buses) {
      if (bus.role == "master" || bus.id == "master") {
        has_master = true;
        break;
      }
    }
    if (!has_master) {
      mixer->buses.push_back({"master", "master"});
    }

    // Bus inserts are built here, at the same lifecycle stage as strip inserts
    // above, and live in SonareMixer::bus_dsp for the mixer's lifetime. They used
    // to be constructed inside build_and_compile, which runs again on every
    // unrelated topology edit and so discarded each insert's state (reverb tail,
    // delay line, envelope follower) one block after any strip change.
    for (const auto& bus : mixer->buses) {
      auto dsp = std::make_unique<SonareBusDsp>();
      dsp->id = bus.id;
      // Prepare before adding inserts, the same order sonare_mixer_add_strip_ex
      // uses: add_insert prepares each processor as it arrives only once the
      // container itself knows its block size, and BusNode::prepare will not
      // prepare this bus again on a later compile.
      dsp->fx.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
      dsp->panner.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
      dsp->panner.set_pan(bus.pan);
      dsp->panner.set_pan_mode(to_pan_mode(bus.pan_mode));
      dsp->panner.set_pan_law(to_pan_law(bus.pan_law));
      dsp->panner.set_dual_pan(bus.dual_pan_left, bus.dual_pan_right);
      // Settle immediately: an offline graph must not glide in from center on
      // the first rendered block (mirrors ChannelStrip::settle).
      dsp->panner.reset();
      dsp->eq.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
      dsp->eq.prepare_channels(sonare::channel_count(sonare::ChannelLayout::SevenPointOne));
      sonare::mixing::apply_eq(dsp->eq, dsp->eq_enabled, bus.eq, nullptr);
      for (const auto& insert : bus.inserts) {
        std::vector<std::string> unknown_keys;
        auto processor = sonare::mastering::api::make_insert(
            insert.processor_name, insert.params_json, &unknown_keys,
            sonare::resource::kDefaultProjectImportResourceLimits,
            static_cast<double>(mixer->sample_rate));
        if (!processor) {
          throw sonare::SonareException(
              sonare::ErrorCode::InvalidParameter,
              "unknown bus insert processor: " + insert.processor_name + " (bus " + bus.id + ")");
        }
        if (!unknown_keys.empty()) {
          std::string note = "insert '" + insert.processor_name + "' on bus '" + bus.id +
                             "' ignored unknown param" + (unknown_keys.size() > 1 ? "s" : "") +
                             ": ";
          for (size_t k = 0; k < unknown_keys.size(); ++k) {
            if (k > 0) note += ", ";
            note += unknown_keys[k];
          }
          ignored_param_notes.push_back(std::move(note));
        }
        // Same channel policy the engine's bus chain passes, so a pair-only insert
        // on a surround bus touches the front pair alone in both.
        const bool spo = sonare::mastering::api::channel_policy(insert.processor_name) ==
                         sonare::mastering::api::ChannelPolicy::StereoPairOnly;
        dsp->fx.add_insert(std::move(processor), spo);
      }
      mixer->bus_dsp.push_back(std::move(dsp));
    }

    // VCA group offsets (control-only). A strip may belong to several VCA
    // groups, so each group's gain accumulates additively onto the strip's
    // VCA-group offset, matching the runtime VcaGroup delta semantics (summing
    // in the dB domain is equivalent to multiplying the linear VCA gains). The
    // group offset is independent of any manual trim a loaded strip carries.
    for (const auto& group : mixer->vca_groups) {
      for (const auto& member : group.members) {
        for (const auto& strip : mixer->strips) {
          if (strip->id == member) {
            strip->strip.add_vca_group_offset_db(group.gain_db);
            break;
          }
        }
      }
    }

    apply_solo_mutes(mixer.get());
    build_and_compile(mixer.get());
    // Scene loaded successfully; surface unknown scene keys and silently-ignored insert
    // params as a non-fatal warning (one note each, '\n'-joined).
    if (!ignored_param_notes.empty()) {
      std::string warning;
      for (size_t n = 0; n < ignored_param_notes.size(); ++n) {
        if (n > 0) warning += '\n';
        warning += ignored_param_notes[n];
      }
      sonare_c_detail::set_last_warning(warning.c_str());
    }
    return mixer.release();
    SONARE_C_CATCH_RETURN(nullptr)
  }

  SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out) {
    SONARE_C_API_ENTRY;
    if (!mixer || !json_out) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    SONARE_C_TRY
    sonare::mixing::api::Scene scene;
    scene.buses = mixer->buses;
    for (auto& bus : scene.buses) {
      for (const auto& dsp : mixer->bus_dsp) {
        if (dsp->id != bus.id) continue;
        for (size_t i = 0; i < bus.inserts.size(); ++i) {
          const auto* live = dsp->fx.insert_processor(static_cast<unsigned int>(i));
          if (live != nullptr)
            bus.inserts[i].params_json =
                live_insert_params(bus.inserts[i], *live, mixer->sample_rate);
        }
      }
    }
    scene.vca_groups = mixer->vca_groups;
    if (scene.buses.empty()) {
      scene.buses.push_back({"master", "master"});
    }
    scene.connections = mixer->connections;
    for (const auto& strip : mixer->strips) {
      sonare::mixing::api::Strip scene_strip = strip->scene_strip;
      scene_strip.id = strip->id;
      scene_strip.input_trim_db = strip->strip.input_trim_db();
      scene_strip.fader_db = strip->strip.fader_db();
      scene_strip.vca_offset_db = strip->strip.vca_trim_offset_db();
      scene_strip.pan_mode = from_pan_mode(strip->strip.pan_mode());
      // For a DualPan strip the live ChannelStrip pan_ is never updated by
      // set_dual_pan (it drives the L/R pair directly), so reading strip.pan()
      // would export the stale default and disagree with the cached nominal pan
      // that set_dual_pan computed from the clamped L/R. Keep the cached value for
      // DualPan; otherwise mirror the live processor pan.
      scene_strip.pan = scene_strip.pan_mode == SONARE_PAN_MODE_DUAL_PAN ? strip->scene_strip.pan
                                                                         : strip->strip.pan();
      scene_strip.width = strip->strip.width();
      scene_strip.muted = strip->strip.muted();
      scene_strip.soloed = strip->strip.soloed();
      scene_strip.solo_safe = strip->strip.solo_safe();
      scene_strip.pan_law = from_pan_law(strip->strip.pan_law());
      // ChannelStrip exposes no dual-pan getters; reuse the cached scene values.
      scene_strip.dual_pan_left = strip->scene_strip.dual_pan_left;
      scene_strip.dual_pan_right = strip->scene_strip.dual_pan_right;
      scene_strip.polarity_invert_left = strip->strip.polarity_invert_left();
      scene_strip.polarity_invert_right = strip->strip.polarity_invert_right();
      scene_strip.channel_delay_samples = strip->strip.channel_delay_samples();
      // Consumed send and insert automation, so a reload rebuilds the settled mix.
      for (size_t i = 0; i < scene_strip.sends.size() && i < strip->strip.num_sends(); ++i) {
        scene_strip.sends[i].send_db = strip->strip.send_db(i);
      }
      size_t pre_index = 0;
      size_t post_index = 0;
      const size_t pre_count = strip->strip.num_pre_inserts();
      for (auto& insert : scene_strip.inserts) {
        const size_t combined = insert.slot == sonare::mixing::api::InsertSlot::PreFader
                                    ? pre_index++
                                    : pre_count + post_index++;
        const auto* live = strip->strip.insert_processor(static_cast<unsigned int>(combined));
        if (live != nullptr)
          insert.params_json = live_insert_params(insert, *live, mixer->sample_rate);
      }
      scene.strips.push_back(std::move(scene_strip));
    }
    *json_out = sonare_c_detail::copy_string(sonare::mixing::api::scene_to_json(scene));
    return SONARE_OK;
    SONARE_C_CATCH
  }

  SonareError sonare_mixer_compile(SonareMixer * mixer) {
    SONARE_C_API_ENTRY;
    if (!mixer) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    SONARE_C_TRY
    build_and_compile(mixer);
    return SONARE_OK;
    SONARE_C_CATCH
  }

  SonareError sonare_mixer_latency_samples(SonareMixer * mixer, int* out_latency_samples) {
    SONARE_C_API_ENTRY;
    if (!mixer || !out_latency_samples) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    SONARE_C_TRY
    if (mixer->compiled_dirty) {
      build_and_compile(mixer);
    }
    *out_latency_samples = mixer->latency_samples;
    return SONARE_OK;
    SONARE_C_CATCH
  }

  SonareError sonare_mixer_tail_samples(SonareMixer * mixer, int* out_tail_samples) {
    SONARE_C_API_ENTRY;
    if (!mixer || !out_tail_samples) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    SONARE_C_TRY
    *out_tail_samples = tail_samples(mixer);
    return SONARE_OK;
    SONARE_C_CATCH
  }

  SonareError sonare_mixer_process_stereo(
      SonareMixer * mixer, const float* const* input_left, const float* const* input_right,
      size_t input_count, float* output_left, float* output_right, size_t num_samples) {
    SONARE_C_API_ENTRY;
    if (!output_left || !output_right) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    float* output[] = {output_left, output_right};
    return process_planar(mixer, input_left, input_right, input_count, output, 2, num_samples);
  }

  SonareError sonare_mixer_drain_tail_stereo(SonareMixer * mixer, float* output_left,
                                             float* output_right, size_t num_samples) {
    SONARE_C_API_ENTRY;
    return sonare_mixer_process_stereo(mixer, nullptr, nullptr, 0, output_left, output_right,
                                       num_samples);
  }

  SonareError sonare_mixer_flush_meters(SonareMixer * mixer) {
    SONARE_C_API_ENTRY;
    if (!mixer) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    for (auto& strip : mixer->strips) strip->strip.flush_meters();
    for (auto& bus : mixer->bus_dsp) bus->fx.flush_meters();
    return SONARE_OK;
  }

  const char* sonare_mixing_scene_preset_names(void) {
    // thread_local (not plain static): each call reassigns the storage and returns
    // a borrowed pointer into it. A plain static would let a concurrent caller
    // reassign the string and invalidate another thread's returned pointer. Per
    // the C-ABI contract this pointer is borrowed (callers must not free it) and
    // is valid until the next call ON THE SAME THREAD.
    static thread_local std::string storage;
    return sonare_c_detail::join_names(sonare::mixing::api::scene_preset_names(), storage);
  }

  SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out) {
    SONARE_C_API_ENTRY;
    if (!preset_name || !json_out) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    SONARE_C_TRY
    *json_out = nullptr;
    const auto preset = sonare::mixing::api::scene_preset_from_string(preset_name);
    const auto scene = sonare::mixing::api::scene_preset(preset);
    *json_out = sonare_c_detail::copy_string(sonare::mixing::api::scene_to_json(scene));
    return SONARE_OK;
    SONARE_C_CATCH
  }

  void sonare_mixer_destroy(SonareMixer * mixer) { delete mixer; }

  namespace sonare_c_mixing_detail {

  SonareStrip* find_strip(SonareMixer* mixer, const char* id) {
    for (const auto& strip : mixer->strips) {
      if (strip->id == id) return strip.get();
    }
    return nullptr;
  }

  void set_output_channels(SonareMixer* mixer, int channels) {
    if (mixer == nullptr ||
        (channels != 1 && channels != 2 && !sonare::is_surround_channel_count(channels))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer output width must be 1, 2, 6 or 8");
    }
    const int width = std::max(channels, 2);
    if (mixer->output_channels != width) {
      mixer->output_channels = width;
      mixer->compiled_dirty = true;
    }
  }

  SonareError process_planar(SonareMixer* mixer, const float* const* input_left,
                             const float* const* input_right, size_t input_count,
                             float* const* output, int out_channels, size_t num_samples) {
    if (!mixer || !output || out_channels <= 0 || (!input_left && input_count > 0) ||
        (!input_right && input_count > 0) || input_count > mixer->strips.size() ||
        num_samples > static_cast<size_t>(mixer->max_block_size)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    for (int ch = 0; ch < out_channels; ++ch) {
      if (!output[ch]) return SONARE_ERROR_INVALID_PARAMETER;
    }
    // EMPTY-BLOCK POLICY: a zero-frame block is a no-op, not an error. This is a
    // block-processing entry, not an offline analysis (validate_audio_params
    // rejects empty there), and sonare_mixer_drain_tail_stereo delegates here
    // with input_count == 0, so a host that hands over an empty callback block
    // must get SONARE_OK. Rejecting empty *input material* belongs to the
    // caller that knows a mix of nothing is meaningless.
    if (num_samples == 0) {
      return SONARE_OK;
    }
    // Non-finite input is rejected before any mixer state changes, so a NaN can
    // neither reach the mix nor advance the meters / timeline position.
    for (size_t index = 0; index < input_count; ++index) {
      if (input_left[index] && !block_finite(input_left[index], num_samples)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (input_right[index] && !block_finite(input_right[index], num_samples)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
    }

    // Per-strip channel pointers are checked here, with the other rejections,
    // rather than inside the input loop below. Every other exit path returns
    // before writing anything, and the shipped contract for a rejected block is
    // that the caller's buffers are untouched (the non-finite case asserts
    // exactly that). Validating after the zero-fill turned "this block was
    // rejected, reuse the previous one" into a hard dropout.
    const size_t count = std::min(input_count, mixer->strips.size());
    for (size_t index = 0; index < count; ++index) {
      if (!input_left[index] || !input_right[index]) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
    }

    SONARE_C_TRY
    for (int ch = 0; ch < out_channels; ++ch) {
      std::fill(output[ch], output[ch] + num_samples, 0.0f);
    }

    // Lazy compile: rebuild the routing graph if topology changed since the last
    // process/compile. Acceptable to allocate here (offline/block convenience entry).
    if (mixer->compiled_dirty) {
      build_and_compile(mixer);
    }

    const int n = static_cast<int>(num_samples);
    mixer->graph.clear_inputs(n);
    for (size_t index = 0; index < count; ++index) {
      const std::string& id = mixer->input_node_ids[index];
      mixer->graph.set_input(id, 0, input_left[index], n);
      mixer->graph.set_input(id, 1, input_right[index], n);
    }

    mixer->graph.process_block(n);
    mixer->timeline_sample_pos += static_cast<int64_t>(num_samples);

    const int planes = std::min(out_channels, mixer->output_channels);
    for (int ch = 0; ch < planes; ++ch) {
      const float* master = mixer->graph.output(mixer->master_id, ch);
      if (master != nullptr) {
        std::copy(master, master + num_samples, output[ch]);
      }
    }
    return SONARE_OK;
    SONARE_C_CATCH
  }

  }  // namespace sonare_c_mixing_detail
