/// @file scene_delta.cpp
/// @brief Delta composition for the mixing assistant.

#include "mixing/assistant/scene_delta.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace sonare::mixing::assistant {
namespace {

// Accumulated additive contributions for one strip, kept separate from the
// strip itself so the total can be clamped exactly once at the end.
struct GainAccumulator {
  float input_trim_db = 0.0f;
  float fader_db = 0.0f;
  float vca_offset_db = 0.0f;
  bool touched_trim = false;
  bool touched_fader = false;
  bool touched_vca = false;
};

api::Strip* find_strip(api::Scene& scene, const std::string& id) {
  for (auto& strip : scene.strips) {
    if (strip.id == id) return &strip;
  }
  return nullptr;
}

api::Strip& strip_for(api::Scene& scene, const std::string& id) {
  if (api::Strip* existing = find_strip(scene, id)) return *existing;
  api::Strip created;
  created.id = id;
  scene.strips.push_back(std::move(created));
  return scene.strips.back();
}

void note(std::vector<std::string>* sink, std::string text) {
  if (sink != nullptr) sink->push_back(std::move(text));
}

// Clamps a value and reports it when the clamp actually bit. A silently
// truncated total reads downstream as "the levels were matched" when they were
// not, which is exactly the symptom nobody can reproduce.
float clamp_reported(float value, float low, float high, const std::string& what,
                     const std::string& strip_id, std::vector<std::string>* notes) {
  const float clamped = std::clamp(value, low, high);
  if (clamped != value) {
    note(notes, what + " for " + strip_id + " was limited to its allowed range, so the target is " +
                    "not fully reached");
  }
  return clamped;
}

bool insert_already_present(const api::Strip& strip, const api::Insert& candidate) {
  return std::any_of(strip.inserts.begin(), strip.inserts.end(), [&](const api::Insert& existing) {
    return existing.slot == candidate.slot && existing.processor_name == candidate.processor_name;
  });
}

bool send_already_present(const api::Strip& strip, const api::Send& candidate) {
  return std::any_of(strip.sends.begin(), strip.sends.end(), [&](const api::Send& existing) {
    return existing.id == candidate.id ||
           existing.destination_bus_id == candidate.destination_bus_id;
  });
}

}  // namespace

const char* delta_domain_to_string(DeltaDomain domain) noexcept {
  switch (domain) {
    case DeltaDomain::Structure:
      return "structure";
    case DeltaDomain::Gain:
      return "gain";
    case DeltaDomain::Eq:
      return "eq";
    case DeltaDomain::Dynamics:
      return "dynamics";
    case DeltaDomain::Image:
      return "image";
  }
  return "unknown";
}

api::Scene apply_deltas(const api::Scene& base, const std::vector<SceneDelta>& deltas,
                        std::vector<std::string>* notes) {
  api::Scene scene = base;

  // Stable so that the caller's order survives within a domain while the fixed
  // cross-domain order is imposed on top of it.
  std::vector<const SceneDelta*> ordered;
  ordered.reserve(deltas.size());
  for (const auto& delta : deltas) ordered.push_back(&delta);
  std::stable_sort(ordered.begin(), ordered.end(), [](const SceneDelta* a, const SceneDelta* b) {
    return static_cast<int>(a->domain) < static_cast<int>(b->domain);
  });

  std::map<std::string, GainAccumulator> gains;

  for (const SceneDelta* delta : ordered) {
    for (const auto& bus : delta->buses) {
      const bool exists = std::any_of(scene.buses.begin(), scene.buses.end(),
                                      [&](const api::Bus& b) { return b.id == bus.id; });
      if (exists) {
        note(notes, "bus " + bus.id + " was already present, so the duplicate was dropped");
        continue;
      }
      scene.buses.push_back(bus);
    }

    for (const auto& group : delta->vca_groups) {
      const bool exists = std::any_of(scene.vca_groups.begin(), scene.vca_groups.end(),
                                      [&](const api::VcaGroup& g) { return g.id == group.id; });
      if (exists) {
        note(notes, "vca group " + group.id + " was already present, so the duplicate was dropped");
        continue;
      }
      scene.vca_groups.push_back(group);
    }

    for (const auto& connection : delta->connections) {
      const bool exists = std::any_of(
          scene.connections.begin(), scene.connections.end(), [&](const api::Connection& c) {
            return c.source == connection.source && c.destination == connection.destination;
          });
      if (exists) continue;
      scene.connections.push_back(connection);
    }

    if (delta->strip_id.empty()) continue;

    api::Strip& strip = strip_for(scene, delta->strip_id);
    GainAccumulator& gain = gains[delta->strip_id];

    if (delta->input_trim_db) {
      gain.input_trim_db += *delta->input_trim_db;
      gain.touched_trim = true;
    }
    if (delta->fader_db) {
      gain.fader_db += *delta->fader_db;
      gain.touched_fader = true;
    }
    if (delta->vca_offset_db) {
      gain.vca_offset_db += *delta->vca_offset_db;
      gain.touched_vca = true;
    }

    if (delta->pan) strip.pan = *delta->pan;
    if (delta->width) strip.width = *delta->width;
    if (delta->polarity_invert_left) strip.polarity_invert_left = *delta->polarity_invert_left;
    if (delta->polarity_invert_right) strip.polarity_invert_right = *delta->polarity_invert_right;
    if (delta->channel_delay_samples) strip.channel_delay_samples = *delta->channel_delay_samples;

    for (const auto& insert : delta->inserts) {
      if (insert_already_present(strip, insert)) {
        note(notes, insert.processor_name + " was already suggested for " + strip.id +
                        " in the same slot, so the second one was dropped");
        continue;
      }
      strip.inserts.push_back(insert);
    }

    for (const auto& send : delta->sends) {
      if (send_already_present(strip, send)) {
        note(notes, strip.id + " already sends to " + send.destination_bus_id +
                        ", so the duplicate send was dropped");
        continue;
      }
      strip.sends.push_back(send);
    }
  }

  // Clamp once, after every contribution has been summed.
  for (const auto& [strip_id, gain] : gains) {
    api::Strip* strip = find_strip(scene, strip_id);
    if (strip == nullptr) continue;
    if (gain.touched_trim) {
      strip->input_trim_db =
          clamp_reported(strip->input_trim_db + gain.input_trim_db, kMinSuggestedTrimDb,
                         kMaxSuggestedTrimDb, "input trim", strip_id, notes);
    }
    if (gain.touched_fader) {
      strip->fader_db = clamp_reported(strip->fader_db + gain.fader_db, kMinSuggestedFaderDb,
                                       kMaxSuggestedFaderDb, "fader", strip_id, notes);
    }
    if (gain.touched_vca) {
      strip->vca_offset_db =
          clamp_reported(strip->vca_offset_db + gain.vca_offset_db, kMinSuggestedFaderDb,
                         kMaxSuggestedFaderDb, "vca offset", strip_id, notes);
    }
  }

  // Spatial fields are last-writer-wins rather than summed, so they are clamped
  // where they land rather than through an accumulator.
  for (auto& strip : scene.strips) {
    strip.pan = std::clamp(strip.pan, -1.0f, 1.0f);
    strip.width = std::clamp(strip.width, 0.0f, kMaxSuggestedWidth);
    strip.channel_delay_samples =
        std::clamp(strip.channel_delay_samples, 0, kMaxSuggestedDelaySamples);
  }

  return scene;
}

}  // namespace sonare::mixing::assistant
