/// @file binding_project_part_rig_test.cpp
/// @brief Part rigs across the C ABI: the project's entries reach a bounce, never
///        reach an exported SMF, and sonare_engine_set_part_rig drives the live
///        instruments that have rigs.

#include <cstring>
#include <vector>

#include "binding_project_parity_test_helpers.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

constexpr uint32_t kDestination = 3;
constexpr const char* kSoftClipChain = R"([{"processor":"saturation.softClipper","params":"{}"}])";

/// One MIDI track routed to kDestination: program 30 (electric guitar, a program
/// the default rig covers) on channel 0, then a held note.
SonareProject* make_guitar_project() {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[3] = {};
  events[0].ppq = 0.0;
  events[0].data0 = 0x20C01E00u;  // program change, channel 0, program 30
  events[1].ppq = 0.0;
  events[1].data0 = 0x20900000u | (52u << 8) | 0x60u;  // note-on 52, velocity 96
  events[2].ppq = 2.0;
  events[2].data0 = 0x20800000u | (52u << 8);  // note-off 52
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 3) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
  return project;
}

std::vector<float> bounce_gm_synth(SonareProject* project) {
  SonareProjectBounceOptions options{};
  options.total_frames = 24000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.use_gm_programs = 1;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  return result;
}

float rms_of(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float s : samples) sum += static_cast<double>(s) * s;
  return samples.empty() ? 0.0f : static_cast<float>(std::sqrt(sum / samples.size()));
}

std::vector<uint8_t> export_smf(const SonareProject* project) {
  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  std::vector<uint8_t> result(bytes, bytes + len);
  sonare_free_bytes(bytes);
  return result;
}

}  // namespace

TEST_CASE("a synth bounce of GM program 30 sounds through the default rig",
          "[bindings][part_rig]") {
  SonareProject* project = make_guitar_project();
  const std::vector<float> with_default_rig = bounce_gm_synth(project);
  REQUIRE(rms_of(with_default_rig) > 0.0f);

  REQUIRE(sonare_project_set_part_rig(project, kDestination, SONARE_PART_RIG_ALL_PARTS,
                                      SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  const std::vector<float> di = bounce_gm_synth(project);
  REQUIRE(rms_of(di) > 0.0f);
  REQUIRE(di != with_default_rig);

  // An explicit bank entry is the default again, and a part entry outranks the
  // destination default.
  REQUIRE(sonare_project_set_part_rig(project, kDestination, 0, SONARE_PART_RIG_BANK, nullptr) ==
          SONARE_OK);
  REQUIRE(bounce_gm_synth(project) == with_default_rig);

  // A chain differs from the default rig and from DI, and an entry for another
  // destination changes nothing.
  REQUIRE(sonare_project_set_part_rig(project, kDestination, 0, SONARE_PART_RIG_CHAIN,
                                      kSoftClipChain) == SONARE_OK);
  const std::vector<float> chained = bounce_gm_synth(project);
  REQUIRE(chained != with_default_rig);
  REQUIRE(chained != di);

  REQUIRE(sonare_project_clear_part_rig(project, kDestination, 0) == SONARE_OK);
  REQUIRE(sonare_project_set_part_rig(project, kDestination + 1, SONARE_PART_RIG_ALL_PARTS,
                                      SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  REQUIRE(sonare_project_clear_part_rig(project, kDestination, SONARE_PART_RIG_ALL_PARTS) ==
          SONARE_OK);
  REQUIRE(bounce_gm_synth(project) == with_default_rig);

  sonare_project_destroy(project);
}

TEST_CASE("a SoundFont-player bounce honours part rigs", "[bindings][part_rig]") {
  SonareProject* project = make_guitar_project();
  SonareProjectBounceOptions options{};
  options.total_frames = 24000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareSf2InstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config.struct_version = 4;

  const auto bounce = [&] {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
    std::vector<float> result(out, out + out_len);
    sonare_free_floats(out);
    return result;
  };
  const std::vector<float> with_default_rig = bounce();
  REQUIRE(rms_of(with_default_rig) > 0.0f);
  REQUIRE(sonare_project_set_part_rig(project, kDestination, SONARE_PART_RIG_ALL_PARTS,
                                      SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  REQUIRE(bounce() != with_default_rig);

  sonare_project_destroy(project);
}

TEST_CASE("part rigs never reach an exported SMF", "[bindings][part_rig]") {
  SonareProject* project = make_guitar_project();
  const std::vector<uint8_t> before = export_smf(project);
  REQUIRE(!before.empty());
  REQUIRE(sonare_project_set_part_rig(project, kDestination, 0, SONARE_PART_RIG_CHAIN,
                                      kSoftClipChain) == SONARE_OK);
  REQUIRE(sonare_project_set_part_rig(project, kDestination, SONARE_PART_RIG_ALL_PARTS,
                                      SONARE_PART_RIG_NONE, nullptr) == SONARE_OK);
  REQUIRE(export_smf(project) == before);
  sonare_project_destroy(project);
}

TEST_CASE("sonare_engine_set_part_rig reaches NativeSynth and Sf2Player, not the built-in synth",
          "[bindings][part_rig][engine]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);

  SonareSynthInstrumentBinding synth{};
  synth.destination_id = 1;
  synth.use_gm_programs = 1;
  REQUIRE(sonare_engine_set_synth_instrument_binding(engine, &synth) == SONARE_OK);
  SonareEngineSf2InstrumentConfig sf2{};
  sf2.struct_version = 4;
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 2, &sf2) == SONARE_OK);
  SonareEngineBuiltinSynthConfig builtin{};
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 3, &builtin) == SONARE_OK);

  for (const uint32_t destination : {1u, 2u}) {
    INFO(destination);
    REQUIRE(sonare_engine_set_part_rig(engine, destination, 0, SONARE_PART_RIG_NONE, nullptr) ==
            SONARE_OK);
    REQUIRE(sonare_engine_set_part_rig(engine, destination, SONARE_PART_RIG_ALL_PARTS,
                                       SONARE_PART_RIG_BANK, nullptr) == SONARE_OK);
    REQUIRE(sonare_engine_set_part_rig(engine, destination, 5, SONARE_PART_RIG_CHAIN,
                                       kSoftClipChain) == SONARE_OK);
  }
  REQUIRE(sonare_engine_set_part_rig(engine, 3, 0, SONARE_PART_RIG_NONE, nullptr) ==
          SONARE_ERROR_NOT_SUPPORTED);
  sonare_engine_destroy(engine);
}

#endif  // SONARE_WITH_ARRANGEMENT
