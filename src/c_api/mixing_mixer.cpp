#include "c_api/mixing_internal.h"

using namespace sonare_c_mixing_detail;

size_t sonare_mixer_strip_count(const SonareMixer* mixer) {
  if (!mixer) {
    return 0;
  }
  return mixer->strips.size();
}

SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count) {
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->strips.size();
  return SONARE_OK;
}

SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role) {
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

SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id) {
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
  // Drop any connection that referenced the removed bus; otherwise the next
  // compile would try to wire an edge to/from a node that no longer exists.
  mixer->connections.erase(std::remove_if(mixer->connections.begin(), mixer->connections.end(),
                                          [&](const sonare::mixing::api::Connection& connection) {
                                            return connection.source == bus_id ||
                                                   connection.destination == bus_id;
                                          }),
                           mixer->connections.end());
  // Strip sends that still target the removed bus are intentionally left in
  // place: compile_graph re-materializes any unknown send destination as an
  // implicit aux bus (default-routed to master), so the send stays audible
  // rather than dangling. ChannelStrip has no remove_send, so dropping the
  // send would require rebuilding the strip and is out of scope here.
  mixer->compiled_dirty = true;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count) {
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->buses.size();
  return SONARE_OK;
}

SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count) {
  if (!mixer || !id || (member_count > 0 && !members)) {
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
    group.members.emplace_back(members[i]);
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

SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id) {
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
  if (!mixer || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = mixer->vca_groups.size();
  return SONARE_OK;
}

SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index) {
  if (!mixer || index >= mixer->strips.size()) {
    return nullptr;
  }
  return mixer->strips[index].get();
}

SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id) {
  if (!mixer || !id) {
    return nullptr;
  }
  for (const auto& strip : mixer->strips) {
    if (strip->id == id) {
      return strip.get();
    }
  }
  return nullptr;
}

SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size) {
  if (!json) {
    return nullptr;
  }
  try {
    const auto scene = sonare::mixing::api::scene_from_json(json);
    std::unique_ptr<SonareMixer> mixer(sonare_mixer_create(sample_rate, max_block_size));
    if (!mixer) {
      return nullptr;
    }
    for (const auto& scene_strip : scene.strips) {
      SonareStrip* strip = sonare_mixer_add_strip(mixer.get(), scene_strip.id.c_str());
      if (!strip) {
        return nullptr;
      }
      strip->scene_strip = scene_strip;
      strip->strip.set_input_trim_db(scene_strip.input_trim_db);
      strip->strip.set_fader_db(scene_strip.fader_db);
      strip->strip.set_pan(scene_strip.pan);
      strip->strip.set_width(scene_strip.width);
      strip->strip.set_muted(scene_strip.muted);
      strip->strip.set_soloed(scene_strip.soloed);
      strip->strip.set_solo_safe(scene_strip.solo_safe);
      strip->strip.set_pan_mode(to_pan_mode(scene_strip.pan_mode));
      strip->strip.set_dual_pan(scene_strip.dual_pan_left, scene_strip.dual_pan_right);
      strip->strip.set_pan_law(to_pan_law(scene_strip.pan_law));
      strip->strip.set_polarity_invert(scene_strip.polarity_invert_left,
                                       scene_strip.polarity_invert_right);
      strip->strip.set_channel_delay_samples(scene_strip.channel_delay_samples);
      for (const auto& insert : scene_strip.inserts) {
        auto processor =
            sonare::mastering::api::make_insert(insert.processor_name, insert.params_json);
        if (!processor) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "unknown insert processor: " + insert.processor_name +
                                            " (strip " + scene_strip.id + ")");
        }
        if (insert.slot == sonare::mixing::api::InsertSlot::PreFader) {
          strip->strip.add_pre_insert(std::move(processor));
        } else {
          strip->strip.add_post_insert(std::move(processor));
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

    // VCA group offsets (control-only). A strip may belong to several VCA
    // groups, so each group's gain accumulates additively onto the strip's
    // VCA-group offset, matching the runtime VcaGroup delta semantics (summing
    // in the dB domain is equivalent to multiplying the linear VCA gains). The
    // group offset is independent of any manual trim a loaded strip carries.
    for (const auto& group : scene.vca_groups) {
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
    return mixer.release();
  } catch (const std::exception& e) {
    sonare_c_detail::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)");
    return nullptr;
  }
}

SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out) {
  if (!mixer || !json_out) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::mixing::api::Scene scene;
  scene.buses = mixer->buses;
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
    scene.strips.push_back(std::move(scene_strip));
  }
  *json_out = sonare_c_detail::copy_string(sonare::mixing::api::scene_to_json(scene));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_compile(SonareMixer* mixer) {
  if (!mixer) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  build_and_compile(mixer);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples) {
  if (!mixer || !output_left || !output_right || (!input_left && input_count > 0) ||
      (!input_right && input_count > 0) || input_count > mixer->strips.size() ||
      num_samples > static_cast<size_t>(mixer->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) {
    return SONARE_OK;
  }

  SONARE_C_TRY
  std::fill(output_left, output_left + num_samples, 0.0f);
  std::fill(output_right, output_right + num_samples, 0.0f);

  // Lazy compile: rebuild the routing graph if topology changed since the last
  // process/compile. Acceptable to allocate here (offline/block convenience entry).
  if (mixer->compiled_dirty) {
    build_and_compile(mixer);
  }

  const int n = static_cast<int>(num_samples);
  mixer->graph.clear_inputs(n);
  const size_t count = std::min(input_count, mixer->strips.size());
  for (size_t index = 0; index < count; ++index) {
    if (!input_left[index] || !input_right[index]) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const std::string& id = mixer->strips[index]->id;
    mixer->graph.set_input(id, 0, input_left[index], n);
    mixer->graph.set_input(id, 1, input_right[index], n);
  }

  mixer->graph.process_block(n);

  const float* master_l = mixer->graph.output(mixer->master_id, 0);
  const float* master_r = mixer->graph.output(mixer->master_id, 1);
  if (master_l != nullptr && master_r != nullptr) {
    std::copy(master_l, master_l + num_samples, output_left);
    std::copy(master_r, master_r + num_samples, output_right);
  }
  return SONARE_OK;
  SONARE_C_CATCH
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

void sonare_mixer_destroy(SonareMixer* mixer) { delete mixer; }
