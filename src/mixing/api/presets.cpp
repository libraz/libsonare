#include "mixing/api/presets.h"

#include <stdexcept>

namespace sonare::mixing::api {
namespace {

Scene make_vocal_reverb_send() {
  Scene scene;
  scene.buses.push_back({"master", "master"});
  scene.buses.push_back({"vocal-verb", "aux"});

  Strip vocal;
  vocal.id = "vocal";
  vocal.fader_db = -3.0f;
  vocal.inserts.push_back(
      {InsertSlot::PreFader, "eq.parametric", "{\"highPassHz\":80,\"presenceDb\":2}"});
  vocal.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.compressor", "{\"thresholdDb\":-18,\"ratio\":2.5}"});
  vocal.sends.push_back({"vocal-to-verb", "vocal-verb", -14.0f, SendTiming::PostFader});
  scene.strips.push_back(vocal);

  Strip return_strip;
  return_strip.id = "vocal-verb-return";
  return_strip.fader_db = -10.0f;
  return_strip.width = 1.25f;
  return_strip.inserts.push_back(
      {InsertSlot::PostFader, "effects.reverb.plate", "{\"decaySec\":1.8,\"preDelayMs\":25}"});
  scene.strips.push_back(return_strip);

  scene.connections.push_back({"vocal", "master"});
  scene.connections.push_back({"vocal-verb-return", "master"});
  return scene;
}

Scene make_drum_bus_subgroup() {
  Scene scene;
  scene.buses.push_back({"master", "master"});
  scene.buses.push_back({"drum-bus", "subgroup"});

  for (const char* id : {"kick", "snare", "overheads"}) {
    Strip strip;
    strip.id = id;
    strip.fader_db = id == std::string("overheads") ? -6.0f : -3.0f;
    strip.inserts.push_back({InsertSlot::PreFader, "eq.parametric", "{}"});
    scene.connections.push_back({strip.id, "drum-bus"});
    scene.strips.push_back(strip);
  }

  Strip bus_return;
  bus_return.id = "drum-bus-return";
  bus_return.fader_db = -2.0f;
  bus_return.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.parallelComp", "{\"thresholdDb\":-20,\"mix\":0.35}"});
  bus_return.inserts.push_back({InsertSlot::PostFader, "saturation.tape", "{\"driveDb\":1.5}"});
  scene.strips.push_back(bus_return);
  scene.connections.push_back({"drum-bus-return", "master"});

  scene.vca_groups.push_back({"drums", 0.0f, {"kick", "snare", "overheads", "drum-bus-return"}});
  return scene;
}

Scene make_commentary_ducking() {
  Scene scene;
  scene.buses.push_back({"master", "master"});

  Strip host;
  host.id = "host";
  host.fader_db = -3.0f;
  host.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.deesser", "{\"frequencyHz\":6000,\"thresholdDb\":-24}"});
  host.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.compressor", "{\"thresholdDb\":-20,\"ratio\":3}"});
  scene.strips.push_back(host);

  Strip guest;
  guest.id = "guest";
  guest.fader_db = -4.0f;
  guest.pan = 0.1f;
  guest.inserts.push_back(
      {InsertSlot::PreFader, "dynamics.compressor", "{\"thresholdDb\":-22,\"ratio\":2.5}"});
  scene.strips.push_back(guest);

  Strip bed;
  bed.id = "music-bed";
  bed.fader_db = -18.0f;
  bed.inserts.push_back(
      {InsertSlot::PostFader, "dynamics.sidechainRouter", "{\"key\":\"host\",\"rangeDb\":18}"});
  scene.strips.push_back(bed);

  scene.connections.push_back({"host", "master"});
  scene.connections.push_back({"guest", "master"});
  scene.connections.push_back({"music-bed", "master"});
  scene.vca_groups.push_back({"voices", 0.0f, {"host", "guest"}});
  return scene;
}

}  // namespace

std::vector<std::string> scene_preset_names() {
  return {"vocalReverbSend", "drumBusSubgroup", "commentaryDucking"};
}

ScenePreset scene_preset_from_string(const std::string& name) {
  if (name == "vocalReverbSend") return ScenePreset::VocalReverbSend;
  if (name == "drumBusSubgroup") return ScenePreset::DrumBusSubgroup;
  if (name == "commentaryDucking") return ScenePreset::CommentaryDucking;
  throw std::invalid_argument("unknown mixing scene preset: " + name);
}

const char* scene_preset_to_string(ScenePreset preset) noexcept {
  switch (preset) {
    case ScenePreset::VocalReverbSend:
      return "vocalReverbSend";
    case ScenePreset::DrumBusSubgroup:
      return "drumBusSubgroup";
    case ScenePreset::CommentaryDucking:
      return "commentaryDucking";
  }
  return "unknown";
}

Scene scene_preset(ScenePreset preset) {
  switch (preset) {
    case ScenePreset::VocalReverbSend:
      return make_vocal_reverb_send();
    case ScenePreset::DrumBusSubgroup:
      return make_drum_bus_subgroup();
    case ScenePreset::CommentaryDucking:
      return make_commentary_ducking();
  }
  throw std::invalid_argument("unknown mixing scene preset");
}

}  // namespace sonare::mixing::api
