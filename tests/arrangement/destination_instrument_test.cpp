#include "arrangement/destination_instrument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"
#include "midi/controller_numbers.h"
#include "midi/sequencer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "support/sf2_builder.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare::arrangement;

namespace {

DestinationInstrumentBinding synth_binding(std::string preset) {
  DestinationSynthInstrument synth;
  synth.preset = std::move(preset);
  synth.engine_mode = "subtractive";
  synth.fields.emplace("gain", 0.75);
  synth.engine_params.emplace("filter.cutoffHz", 1200.0);
  synth.mod_routings.push_back({1, 2, 0.5f});
  return DestinationInstrumentBinding{std::move(synth), DestinationMidiBank{1, 2}, 3};
}

Track track_for_destination(uint32_t destination_id) {
  Track track;
  track.midi_destination_id = destination_id;
  return track;
}

}  // namespace

TEST_CASE("destination instrument bindings are sorted and retained after rerouting",
          "[arrangement][destination_instrument]") {
  Project project;
  project.add_track(track_for_destination(9));
  project.add_track(track_for_destination(2));

  project.set_destination_instrument(9, synth_binding("nine"));
  project.set_destination_instrument(2, synth_binding("two"));

  REQUIRE(project.destination_instruments().size() == 2);
  auto it = project.destination_instruments().begin();
  CHECK(it->first == 2);
  CHECK(std::get<DestinationSynthInstrument>(it->second.instrument).preset == "two");
  ++it;
  CHECK(it->first == 9);

  const TrackId routed_track = project.tracks().front().id;
  project.find_track_mutable(routed_track)->midi_destination_id = 4;
  CHECK(project.find_destination_instrument(9) != nullptr);
}

TEST_CASE("destination instrument binding requires a track reference",
          "[arrangement][destination_instrument]") {
  Project project;
  try {
    project.set_destination_instrument(7, synth_binding("unused"));
    FAIL("expected an invalid destination refusal");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    CHECK(std::string(error.what()).find("destination_id") != std::string::npos);
  }

  project.add_track(track_for_destination(0));
  CHECK_NOTHROW(project.set_destination_instrument(0, synth_binding("default")));
}

TEST_CASE("destination instrument bank, program and SoundFont URI are checked",
          "[arrangement][destination_instrument]") {
  Project project;
  project.add_track(track_for_destination(3));

  auto binding = synth_binding("valid");
  binding.bank->msb = 128;
  CHECK_THROWS_AS(project.set_destination_instrument(3, binding), sonare::SonareException);

  binding = synth_binding("valid");
  binding.program = -1;
  CHECK_THROWS_AS(project.set_destination_instrument(3, binding), sonare::SonareException);

  DestinationSf2Instrument sf2;
  sf2.sound_font_uri = std::string();
  binding.instrument = sf2;
  binding.bank.reset();
  binding.program.reset();
  CHECK_THROWS_AS(project.set_destination_instrument(3, binding), sonare::SonareException);
}

TEST_CASE("destination instrument edits restore prior bindings and clear missing bindings",
          "[arrangement][destination_instrument]") {
  Project project;
  project.add_track(track_for_destination(12));
  const auto before_binding = synth_binding("before");
  project.set_destination_instrument(12, before_binding);

  SetDestinationInstrument command(12, synth_binding("after"));
  CHECK(command.retained_bytes() > sizeof(command));
  MidiContentStore store;
  EditCommandPtr inverse = command.invert(project, store);
  REQUIRE(command.apply(project, store));
  REQUIRE(project.find_destination_instrument(12) != nullptr);
  CHECK(std::get<DestinationSynthInstrument>(project.find_destination_instrument(12)->instrument)
            .preset == "after");

  project.find_track_mutable(project.tracks().front().id)->midi_destination_id = 13;
  REQUIRE(inverse->apply(project, store));
  REQUIRE(project.find_destination_instrument(12) != nullptr);
  CHECK(project.find_destination_instrument(12)->instrument == before_binding.instrument);
  CHECK(project.find_destination_instrument(12)->bank == before_binding.bank);
  CHECK(project.find_destination_instrument(12)->program == before_binding.program);

  SetDestinationInstrument clear(99, std::nullopt);
  CHECK(clear.apply(project, store));
  CHECK(project.find_destination_instrument(99) == nullptr);
  CHECK(clear.retained_bytes() >= sizeof(clear));
}

TEST_CASE("destination instrument snapshots and retained bytes include bindings",
          "[arrangement][destination_instrument]") {
  Project project;
  project.add_track(track_for_destination(5));
  project.set_destination_instrument(5, synth_binding(std::string(80, 'p')));

  Project snapshot = project.snapshot_members(project_member::kDestinationInstruments);
  REQUIRE(snapshot.destination_instruments().size() == 1);
  CHECK(snapshot.destination_instruments().begin()->first == 5);

  Project restored;
  restored.swap_members(snapshot, project_member::kDestinationInstruments);
  CHECK(restored.destination_instruments() == project.destination_instruments());

  const std::size_t project_bytes = retained::dynamic_bytes(project);
  CHECK(project_bytes > 80);
}

TEST_CASE("destination bank and program precede the first authored program",
          "[arrangement][destination_instrument]") {
  namespace midi = sonare::midi;
  constexpr uint32_t kDestination = 9;
  constexpr uint8_t kGroup = 3;
  constexpr uint8_t kChannel = 6;
  Project project;
  Track track = track_for_destination(kDestination);
  track.kind = Track::Kind::kMidi;
  const auto track_id = project.add_track(std::move(track));
  const auto source_id = project.add_midi_source(MidiSourceRef{});
  EditClip clip;
  clip.track_id = track_id;
  clip.source_id = source_id;
  clip.length_ppq = 2;
  const auto clip_id = project.add_clip(clip);
  REQUIRE(clip_id != 0);
  project.set_destination_instrument(kDestination, synth_binding("keys"));
  const auto authored = midi::make_midi1_program_change(kGroup, kChannel, 27);
  MidiContentStore midi_store;
  midi_store.events[clip_id].push_back({0, authored.words[0]});

  const auto compiled = compile(project, midi_store, {});
  REQUIRE(compiled.timeline);
  const auto& schedules = compiled.timeline->midi_clips;
  REQUIRE(schedules.size() == 2);
  const auto& setup = schedules.front();
  CHECK(setup.destination_id == kDestination);
  CHECK(setup.track_id == track_id);
  REQUIRE(setup.events.size() == 3);
  for (const auto& event : setup.events) CHECK(event.render_frame == 0);
  CHECK(setup.events[0].ump ==
        midi::make_midi1_control_change(kGroup, kChannel, midi::cc::kBankSelectMsb, 1));
  CHECK(setup.events[1].ump ==
        midi::make_midi1_control_change(kGroup, kChannel, midi::cc::kBankSelectLsb, 2));
  CHECK(setup.events[2].ump == midi::make_midi1_program_change(kGroup, kChannel, 3));
  REQUIRE(schedules.back().events.size() == 1);
  CHECK(schedules.back().id == clip_id);
  CHECK(schedules.back().events.front().ump == authored);
  CHECK(schedules.back().events.front().render_frame == 0);

  const auto second_track = project.add_track(project.tracks().front());
  clip.track_id = second_track;
  const auto second_clip = project.add_clip(clip);
  REQUIRE(second_clip != 0);
  midi_store.events[second_clip].push_back({0, authored.words[0]});
  const auto combined = compile(project, midi_store, {});
  REQUIRE(combined.timeline);
  const auto selected = combined.timeline->select_clips(
      [second_track](TrackId candidate) { return candidate == second_track; }, false, true);
  REQUIRE(selected.midi_clips.size() == 2);
  CHECK(selected.midi_clips.front().track_id == second_track);
  CHECK(selected.midi_clips.front().events.size() == 3);
  CHECK(selected.midi_clips.back().id == second_clip);
}

TEST_CASE("a dangling destination instrument produces a compile warning",
          "[arrangement][destination_instrument]") {
  Project project;
  const auto track_id = project.add_track(track_for_destination(9));
  project.set_destination_instrument(9, synth_binding("keys"));
  project.find_track_mutable(track_id)->midi_destination_id = 2;
  const auto compiled = compile(project, {}, {});
  REQUIRE(compiled.timeline);
  REQUIRE(compiled.diagnostics.size() == 1);
  CHECK(compiled.diagnostics.front().severity == Diagnostic::Severity::kWarning);
  CHECK(compiled.diagnostics.front().message.find("instrument destination 9") != std::string::npos);
  CHECK(project.find_destination_instrument(9) != nullptr);
}

TEST_CASE("Frame-zero destination setup precedes all authored events",
          "[arrangement][destination_instrument]") {
  namespace midi = sonare::midi;
  constexpr uint32_t kDestination = 9;
  constexpr uint8_t kGroup = 3;
  constexpr uint8_t kChannel = 6;
  const auto bank = [](uint8_t controller, uint8_t value) {
    return midi::make_midi1_control_change(kGroup, kChannel, controller, value);
  };
  const auto program = [](uint8_t value) {
    return midi::make_midi1_program_change(kGroup, kChannel, value);
  };

  Project project;
  Track track = track_for_destination(kDestination);
  track.kind = Track::Kind::kMidi;
  EditClip clip;
  clip.track_id = project.add_track(std::move(track));
  clip.source_id = project.add_midi_source(MidiSourceRef{});
  clip.length_ppq = 2;
  const auto clip_id = project.add_clip(clip);
  REQUIRE(clip_id != 0);
  const auto authored_msb = bank(midi::cc::kBankSelectMsb, 8);
  const auto authored_lsb = bank(midi::cc::kBankSelectLsb, 9);
  const auto authored_program = program(27);
  const auto authored_note_off = midi::make_midi1_note_off(kGroup, kChannel, 60, 0);
  MidiContentStore midi_store;
  midi_store.events[clip_id] = {{0, authored_note_off.words[0]},
                                {0, authored_msb.words[0]},
                                {0, authored_lsb.words[0]},
                                {0, authored_program.words[0]}};

  const auto ordinary = compile(project, midi_store, {});
  REQUIRE(ordinary.timeline);
  REQUIRE(ordinary.timeline->midi_clips.size() == 1);
  CHECK_FALSE(ordinary.timeline->midi_clips.front().frame_zero_prelude);
  project.set_destination_instrument(kDestination, synth_binding("keys"));
  auto compiled = compile(project, midi_store, {});
  REQUIRE(compiled.timeline);
  REQUIRE(compiled.timeline->midi_clips.size() == 2);
  CHECK(compiled.timeline->midi_clips.front().frame_zero_prelude);
  CHECK(compiled.timeline->midi_clips.back() == ordinary.timeline->midi_clips.front());

  class FrameZeroSink final : public midi::MidiEventSink {
   public:
    void on_event(uint32_t, const midi::MidiEvent& event) noexcept override {
      if (event.render_frame != 0) return;
      if (count < received.size()) received[count] = event.ump;
      ++count;
    }
    std::array<midi::Ump, 7> received{};
    std::size_t count = 0;
  } sink;

  midi::MidiSequencer sequencer;
  sequencer.prepare(sonare::constants::kDefaultDawSampleRate);
  sequencer.set_sink(&sink);
  sequencer.set_midi_clips(std::move(compiled.timeline->midi_clips));
  sequencer.acquire_midi_clips();
  sequencer.dispatch_due(midi::SequencerClock::aligned(0));

  const std::array<midi::Ump, 7> expected = {bank(midi::cc::kBankSelectMsb, 1),
                                             bank(midi::cc::kBankSelectLsb, 2),
                                             program(3),
                                             authored_note_off,
                                             authored_msb,
                                             authored_lsb,
                                             authored_program};
  REQUIRE(sink.count == expected.size());
  CHECK(sink.received == expected);
}

TEST_CASE("Authored program without bank selects plays under the setup bank",
          "[arrangement][destination_instrument]") {
  namespace midi = sonare::midi;
  constexpr uint32_t kDestination = 9;
  constexpr uint8_t kChannel = 6;
  constexpr uint8_t kProgram = 27;
  constexpr uint16_t kSetupBank = 8;
  constexpr uint8_t kNote = 60;
  constexpr int kBlockSize = 64;

  Project project;
  Track track = track_for_destination(kDestination);
  track.kind = Track::Kind::kMidi;
  EditClip clip;
  clip.track_id = project.add_track(std::move(track));
  clip.source_id = project.add_midi_source(MidiSourceRef{});
  clip.length_ppq = 2;
  const auto clip_id = project.add_clip(clip);
  REQUIRE(clip_id != 0);
  auto binding = synth_binding("keys");
  binding.bank = DestinationMidiBank{kSetupBank, 0};
  project.set_destination_instrument(kDestination, std::move(binding));

  MidiContentStore midi_store;
  const auto authored = midi::make_midi1_program_change(0, kChannel, kProgram);
  midi_store.events[clip_id] = {{0, authored.words[0]}};
  auto compiled = compile(project, midi_store, {});
  REQUIRE(compiled.timeline);

  constexpr auto kSampleRate = sonare::constants::kDefaultDawSampleRate;
  sonare::test::Sf2Builder builder;
  sonare::test::Sf2Builder::ZoneSpec sample_zone;
  sample_zone.target = builder.add_sample("tone", std::vector<float>(kBlockSize, 0.25f),
                                          static_cast<uint32_t>(kSampleRate), kNote, 0, kBlockSize);
  sonare::test::Sf2Builder::ZoneSpec instrument_zone;
  instrument_zone.target = builder.add_instrument("tone", {sample_zone});
  builder.add_preset("authored", kSetupBank, kProgram, {instrument_zone});
  const auto bytes = builder.build();
  auto soundfont = std::make_shared<midi::synth::Sf2File>();
  REQUIRE(soundfont->parse(bytes.data(), bytes.size(), nullptr));

  midi::synth::Sf2PlayerConfig config;
  config.synth_fallback = false;
  midi::synth::Sf2Player player(config);
  player.set_soundfont(std::move(soundfont));
  player.prepare(kSampleRate, kBlockSize);
  midi::MidiSequencer sequencer;
  sequencer.prepare(kSampleRate);
  sequencer.set_sink(&player);
  sequencer.set_midi_clips(std::move(compiled.timeline->midi_clips));
  sequencer.acquire_midi_clips();
  sequencer.dispatch_due(midi::SequencerClock::aligned(0));

  const auto note = player.resolve_note_on(kChannel, kNote, midi::Velocity16::from7(100));
  CHECK(note.backend == midi::synth::Sf2Player::NoteBackend::kSoundFont);
  CHECK(note.bank == kSetupBank);
  CHECK(note.program == kProgram);
}
