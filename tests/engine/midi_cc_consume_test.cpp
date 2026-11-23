#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_WITH_ARRANGEMENT)

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <tuple>
#include <vector>

#include "engine/realtime_engine.h"
#include "host/midi_io.h"
#include "midi/control_value.h"
#include "midi/controller_numbers.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "rt/gain_processor.h"
#include "support/alloc_guard.h"
#include "support/golden_hash.h"
#include "util/constants.h"

namespace {

using namespace sonare;
using namespace sonare::engine;

constexpr int kBlockSize = 256;
constexpr int64_t kCcFrame = 1024;
constexpr int64_t kNoteOffFrame = 3072;
constexpr int64_t kRenderFrames = 4096;
constexpr uint32_t kUnboundParam = 7777;

enum class CcIngress { kScalar, kRawUmp, kInputSource };

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

const char* ingress_name(CcIngress ingress) {
  switch (ingress) {
    case CcIngress::kScalar:
      return "scalar";
    case CcIngress::kRawUmp:
      return "raw-ump";
    case CcIngress::kInputSource:
      return "live-input";
  }
  return "unknown";
}

void push_ump(RealtimeEngine& engine, const midi::Ump& ump, int64_t render_frame) {
  REQUIRE(engine.push_midi_ump(0, ump.words, ump.word_count, render_frame) ==
          MidiUmpPushResult::kQueued);
}

void push_scalar_cc(RealtimeEngine& engine, uint8_t controller, uint8_t value,
                    int64_t render_frame) {
  const uint64_t packed = static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
                          (uint64_t{0} << 16) | (uint64_t{0} << 24);
  rt::Command command{};
  command.type = rt::CommandType::kMidiCcImmediate;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(packed);
  REQUIRE(engine.push_command(command));
}

StereoRender render(CcIngress ingress, bool send_cc, bool bind_cc) {
  midi::synth::Sf2PlayerConfig config;
  config.gain = 1.0f;
  config.synth_fallback = true;
  config.dc_block = false;
#if defined(SONARE_MIDI_WITH_FX)
  config.effects.enable_reverb = false;
  config.effects.enable_chorus = false;
  config.effects.enable_delay = false;
#endif
  midi::synth::Sf2Player player(config);
  host::FixedMidiInputSource<8> input;
  RealtimeEngine engine;
  engine.prepare(constants::kDefaultDawSampleRate, kBlockSize);
  REQUIRE(engine.set_midi_instrument(0, &player));
  if (ingress == CcIngress::kInputSource) engine.set_midi_input_source(&input, 0);
  if (bind_cc) {
    REQUIRE(engine.bind_midi_cc(midi::cc::kChannelVolume, 0, kUnboundParam, 0.0f, 1.0f));
  }

  push_ump(engine, midi::make_midi1_program_change(0, 0, 0), 0);
  push_ump(engine, midi::make_midi1_note_on(0, 0, 60, 100), 0);
  if (send_cc) {
    constexpr uint8_t kQuietVolume = 0;
    const midi::Ump cc =
        midi::make_midi1_control_change(0, 0, midi::cc::kChannelVolume, kQuietVolume);
    switch (ingress) {
      case CcIngress::kScalar:
        push_scalar_cc(engine, midi::cc::kChannelVolume, kQuietVolume, kCcFrame);
        break;
      case CcIngress::kRawUmp:
        push_ump(engine, cc, kCcFrame);
        break;
      case CcIngress::kInputSource:
        REQUIRE(input.push_event_at_render_frame(cc, kCcFrame));
        break;
    }
  }
  push_ump(engine, midi::make_midi1_note_off(0, 0, 60, 0), kNoteOffFrame);

  StereoRender audio{std::vector<float>(kRenderFrames), std::vector<float>(kRenderFrames)};
  float* channels[] = {audio.left.data(), audio.right.data()};
  engine.render_offline(channels, 2, kRenderFrames, kBlockSize);
  return audio;
}

bool identical_pcm(const StereoRender& lhs, const StereoRender& rhs) {
  if (lhs.left.size() != rhs.left.size() || lhs.right.size() != rhs.right.size()) return false;
  const std::size_t bytes = lhs.left.size() * sizeof(float);
  return std::memcmp(lhs.left.data(), rhs.left.data(), bytes) == 0 &&
         std::memcmp(lhs.right.data(), rhs.right.data(), bytes) == 0;
}

bool has_audio(const StereoRender& audio) {
  const auto nonzero = [](float sample) { return sample != 0.0f; };
  return std::any_of(audio.left.begin(), audio.left.end(), nonzero) ||
         std::any_of(audio.right.begin(), audio.right.end(), nonzero);
}

void print_digest(CcIngress ingress, const StereoRender& audio) {
  const uint64_t digest = test::fnv1a_quantized_stereo(audio.left, audio.right);
  std::cout << "midi_cc_consume route=" << ingress_name(ingress) << " consume=false digest=0x"
            << std::hex << digest << std::dec << '\n';
}

}  // namespace

TEST_CASE("MIDI CC consume=false preserves audio on every live ingress",
          "[engine][midi_cc_consume]") {
  const StereoRender no_cc = render(CcIngress::kScalar, false, false);
  REQUIRE(has_audio(no_cc));

  constexpr std::array<CcIngress, 3> kIngresses = {
      CcIngress::kScalar,
      CcIngress::kRawUmp,
      CcIngress::kInputSource,
  };
  for (const CcIngress ingress : kIngresses) {
    const StereoRender unbound = render(ingress, true, false);
    const StereoRender pass_through = render(ingress, true, true);

    CHECK_FALSE(identical_pcm(no_cc, unbound));
    CHECK(identical_pcm(unbound, pass_through));
    print_digest(ingress, pass_through);
  }
}

TEST_CASE("MIDI CC binding snapshots round trip into another engine", "[engine][midi_cc_consume]") {
  RealtimeEngine source;
  RealtimeEngine destination;
  CHECK(source.midi_cc_bindings().empty());

  midi::CcBinding plain;
  plain.cc_number = midi::cc::kChannelVolume;
  plain.param_id = kUnboundParam;
  plain.max_value = 0.75f;
  plain.consume = true;
  REQUIRE(source.bind_midi_cc(plain));

  midi::CcBinding paired;
  paired.kind = midi::CcBindingKind::kControlChange14;
  paired.cc_number = midi::cc::kModulationWheel;
  paired.cc_lsb_number = paired.cc_number + 32;
  paired.channel = 5;
  paired.param_id = kUnboundParam + 1;
  paired.min_value = 0.25f;
  REQUIRE(source.bind_midi_cc(paired));

  midi::CcBinding registered;
  registered.kind = midi::CcBindingKind::kRpn;
  registered.cc_number = midi::cc::kDataEntryMsb;
  registered.channel = 5;
  registered.selector_lsb = 1;
  registered.param_id = kUnboundParam + 2;
  registered.consume = true;
  REQUIRE(source.bind_midi_cc(registered));

  auto snapshot = source.midi_cc_bindings();
  std::array expected{plain, paired, registered};
  REQUIRE(snapshot.size() == expected.size());
  source.clear_midi_cc_bindings();
  CHECK(source.midi_cc_bindings().empty());
  for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
    REQUIRE(destination.bind_midi_cc(*it));
  }
  auto actual = destination.midi_cc_bindings();
  REQUIRE(actual.size() == expected.size());

  const auto key = [](const midi::CcBinding& binding) {
    return std::tie(binding.channel, binding.kind, binding.cc_number, binding.cc_lsb_number,
                    binding.selector_msb, binding.selector_lsb);
  };
  const auto less = [&](const midi::CcBinding& lhs, const midi::CcBinding& rhs) {
    return key(lhs) < key(rhs);
  };
  std::sort(expected.begin(), expected.end(), less);
  std::sort(snapshot.begin(), snapshot.end(), less);
  std::sort(actual.begin(), actual.end(), less);
  const auto check_fields = [&](const midi::CcBinding& binding, const midi::CcBinding& original) {
    CHECK(key(binding) == key(original));
    CHECK(binding.param_id == original.param_id);
    CHECK(binding.min_value == original.min_value);
    CHECK(binding.max_value == original.max_value);
    CHECK(binding.consume == original.consume);
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    check_fields(snapshot[i], expected[i]);
    check_fields(actual[i], expected[i]);
  }
}

namespace {

constexpr uint8_t kFullControllerValue = 127;
constexpr float kInitialParameter = 0.25f;

midi::CcBinding consuming_volume(midi::CcBindingKind kind = midi::CcBindingKind::kControlChange7) {
  midi::CcBinding binding;
  binding.kind = kind;
  binding.channel = 0;
  binding.param_id = kUnboundParam;
  binding.consume = true;
  binding.cc_number = midi::cc::kChannelVolume;
  if (kind == midi::CcBindingKind::kControlChange14) {
    binding.cc_lsb_number = binding.cc_number + 32;
  }
  return binding;
}

midi::CcBinding consuming_registered_controller() {
  auto binding = consuming_volume(midi::CcBindingKind::kRpn);
  binding.cc_number = midi::cc::kDataEntryMsb;
  binding.selector_lsb = 1;
  return binding;
}

midi::Ump cc1(uint8_t controller, uint8_t value) {
  return midi::make_midi1_control_change(0, 0, controller, value);
}

struct ConsumingRender {
  StereoRender audio{std::vector<float>(kRenderFrames), std::vector<float>(kRenderFrames)};
  float parameter = kInitialParameter;
};

ConsumingRender render_consuming(CcIngress ingress, const midi::CcBinding& binding,
                                 std::initializer_list<midi::Ump> messages) {
  midi::synth::Sf2PlayerConfig config;
  config.gain = 1.0f;
  config.synth_fallback = true;
  config.dc_block = false;
#if defined(SONARE_MIDI_WITH_FX)
  config.effects.enable_reverb = false;
  config.effects.enable_chorus = false;
  config.effects.enable_delay = false;
#endif
  midi::synth::Sf2Player player(config);
  rt::GainProcessor parameter(kInitialParameter);
  host::FixedMidiInputSource<8> input;
  RealtimeEngine engine;
  engine.prepare(constants::kDefaultDawSampleRate, kBlockSize);
  REQUIRE(engine.set_midi_instrument(0, &player));
  parameter.set_constructed_parameter_values({{binding.param_id, kInitialParameter}});
  REQUIRE(engine.automation().bind_target(binding.param_id, &parameter));
  REQUIRE(engine.bind_midi_cc(binding));
  if (ingress == CcIngress::kInputSource) engine.set_midi_input_source(&input);

  push_ump(engine, midi::make_midi1_program_change(0, 0, 0), 0);
  push_ump(engine, midi::make_midi1_note_on(0, 0, 60, 100), 0);
  for (const auto& message : messages) {
    switch (ingress) {
      case CcIngress::kScalar:
        REQUIRE(message.message_type() == midi::UmpMessageType::kMidi1ChannelVoice);
        push_scalar_cc(engine, message.note_number(), static_cast<uint8_t>(message.words[0] & 0x7F),
                       kCcFrame);
        break;
      case CcIngress::kRawUmp:
        push_ump(engine, message, kCcFrame);
        break;
      case CcIngress::kInputSource:
        REQUIRE(input.push_event_at_render_frame(message, kCcFrame));
        break;
    }
  }
  push_ump(engine, midi::make_midi1_note_off(0, 0, 60, 0), kNoteOffFrame);
  ConsumingRender result;
  float* channels[] = {result.audio.left.data(), result.audio.right.data()};
  engine.render_offline(channels, 2, kRenderFrames, kBlockSize);
  REQUIRE(engine.current_parameter_value(binding.param_id, &result.parameter));
  return result;
}

}  // namespace

TEST_CASE("Consuming volume CC changes automation without changing instrument audio",
          "[engine][midi_cc_consume]") {
  const auto binding = consuming_volume();
  const auto baseline = render_consuming(CcIngress::kScalar, binding, {});
  REQUIRE(has_audio(baseline.audio));
  CHECK(baseline.parameter == Catch::Approx(kInitialParameter));
  CHECK_FALSE(identical_pcm(baseline.audio, render(CcIngress::kScalar, true, false)));
  for (const auto ingress : {CcIngress::kScalar, CcIngress::kRawUmp, CcIngress::kInputSource}) {
    CAPTURE(ingress_name(ingress));
    const auto actual = render_consuming(ingress, binding, {cc1(binding.cc_number, 0)});
    CHECK(identical_pcm(baseline.audio, actual.audio));
    CHECK(actual.parameter == Catch::Approx(0.0f));
  }
}

TEST_CASE("Consuming paired and registered controllers preserve instrument audio",
          "[engine][midi_cc_consume]") {
  const auto volume = consuming_volume();
  const auto paired = consuming_volume(midi::CcBindingKind::kControlChange14);
  const auto registered = consuming_registered_controller();
  const auto rpn_msb = cc1(midi::cc::kRpnMsb, registered.selector_msb);
  const auto rpn_lsb = cc1(midi::cc::kRpnLsb, registered.selector_lsb);
  constexpr uint8_t kMsb = 48;
  constexpr float kPairedValue = static_cast<float>((kMsb << 7) | kFullControllerValue) /
                                 static_cast<float>(midi::kValue14Max);
  const auto check = [&](const midi::CcBinding& binding, std::initializer_list<midi::Ump> messages,
                         std::initializer_list<midi::Ump> retained_messages, float expected) {
    const bool midi2 = messages.begin()->message_type() == midi::UmpMessageType::kMidi2ChannelVoice;
    const auto baseline = render_consuming(CcIngress::kRawUmp, binding, retained_messages);
    REQUIRE(has_audio(baseline.audio));
    CHECK(baseline.parameter == Catch::Approx(kInitialParameter));
    for (const auto ingress : {CcIngress::kScalar, CcIngress::kRawUmp, CcIngress::kInputSource}) {
      if (midi2 && ingress == CcIngress::kScalar) continue;
      CAPTURE(ingress_name(ingress));
      const auto actual = render_consuming(ingress, binding, messages);
      CHECK(identical_pcm(baseline.audio, actual.audio));
      CHECK(actual.parameter == Catch::Approx(expected));
    }
  };
  SECTION("MIDI 1.0 paired control change") {
    check(paired, {cc1(paired.cc_number, kMsb), cc1(paired.cc_lsb_number, kFullControllerValue)},
          {}, kPairedValue);
  }
  SECTION("MIDI 1.0 Data Entry") {
    check(registered,
          {rpn_msb, rpn_lsb, cc1(midi::cc::kDataEntryMsb, kMsb),
           cc1(midi::cc::kDataEntryLsb, kFullControllerValue)},
          {rpn_msb, rpn_lsb}, kPairedValue);
  }
  SECTION("MIDI 2.0 control change") {
    check(volume, {midi::make_midi2_control_change(0, 0, volume.cc_number, 0)}, {}, 0.0f);
  }
  SECTION("MIDI 2.0 registered controller") {
    check(registered,
          {midi::make_midi2_registered_controller(0, 0, registered.selector_msb,
                                                  registered.selector_lsb, 0)},
          {}, 0.0f);
  }
}

TEST_CASE("Live MIDI capture keeps consumed controllers while selectors still reach output",
          "[engine][midi_cc_consume]") {
  const auto paired = consuming_volume(midi::CcBindingKind::kControlChange14);
  const auto registered = consuming_registered_controller();
  rt::GainProcessor parameter(kInitialParameter);
  host::FixedMidiInputSource<8> input;
  host::FixedMidiOutputSink<8> output;
  RealtimeEngine engine;
  engine.prepare(constants::kDefaultDawSampleRate, kBlockSize);
  parameter.set_constructed_parameter_values({{kUnboundParam, kInitialParameter}});
  REQUIRE(engine.automation().bind_target(kUnboundParam, &parameter));
  REQUIRE(engine.bind_midi_cc(paired));
  REQUIRE(engine.bind_midi_cc(registered));
  engine.set_midi_input_source(&input);
  engine.set_midi_output_sink(&output);
  engine.start_midi_capture();

  const auto lsb_alone = cc1(paired.cc_lsb_number, kFullControllerValue);
  REQUIRE(input.push_event_at_render_frame(lsb_alone, 1));
  size_t allocations = 0;
  {
    test::AllocationGuard guard;
    engine.process(nullptr, 0, kBlockSize);
    allocations = guard.count();
  }
  CHECK(allocations == 0);
  CHECK(output.queued_count() == 0);
  float value = 0.0f;
  REQUIRE(engine.current_parameter_value(kUnboundParam, &value));
  CHECK(value == Catch::Approx(kInitialParameter));

  const auto rpn_msb = cc1(midi::cc::kRpnMsb, registered.selector_msb);
  const auto rpn_lsb = cc1(midi::cc::kRpnLsb, registered.selector_lsb);
  const auto entry = cc1(midi::cc::kDataEntryMsb, kFullControllerValue);
  const auto entry_lsb = cc1(midi::cc::kDataEntryLsb, kFullControllerValue);
  const auto midi2 = midi::make_midi2_control_change(0, 0, paired.cc_number, 0);
  const std::array incoming{rpn_msb, rpn_lsb, entry, entry_lsb, midi2};
  for (size_t i = 0; i < incoming.size(); ++i) {
    REQUIRE(
        input.push_event_at_render_frame(incoming[i], kBlockSize + 1 + static_cast<int64_t>(i)));
  }
  {
    test::AllocationGuard guard;
    engine.process(nullptr, 0, kBlockSize);
    allocations = guard.count();
  }
  CHECK(allocations == 0);
  REQUIRE(engine.current_parameter_value(kUnboundParam, &value));
  CHECK(value == Catch::Approx(0.0f));
  std::array<midi::MidiEvent, 8> delivered{};
  REQUIRE(output.drain_queued(delivered.data(), delivered.size()) == 2);
  CHECK(delivered[0].ump == rpn_msb);
  CHECK(delivered[1].ump == rpn_lsb);

  std::array<CapturedMidiEvent, 8> captured{};
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == incoming.size() + 1);
  CHECK(captured[0].ump == lsb_alone);
  for (size_t i = 0; i < incoming.size(); ++i) CHECK(captured[i + 1].ump == incoming[i]);
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
