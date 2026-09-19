/// @file sonare_c_engine_controller_test.cpp
/// @brief The controller-profile C ABI: the preset and enum name tables, the
///        bind/clear/count family, and that a binding made through C actually
///        reaches the sound.
///
/// The last one is the point. Every call here can return SONARE_OK while the
/// profile is dropped on the floor, and the only evidence would be a note that
/// sounds wrong later, so one case renders: a CC bound to the excitation axis
/// must change the audio, and a cleared profile must leave it bit-identical.
/// Exact equality on the cleared side rather than a tolerance, because the CC
/// never reaches a setter there and the two renders are the same arithmetic on
/// the same state.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "c_api/sonare_c_engine_internal.h"
#include "sonare_c_test_helpers.h"

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)

/// Entries in a '\n'-separated name table.
size_t name_count(const char* names) {
  if (names == nullptr || *names == '\0') return 0;
  size_t count = 1;
  for (const char* p = names; *p != '\0'; ++p) {
    if (*p == '\n') ++count;
  }
  return count;
}

bool names_contain(const char* names, const std::string& wanted) {
  const std::string all(names);
  size_t start = 0;
  while (start <= all.size()) {
    const size_t end = all.find('\n', start);
    const std::string entry = all.substr(start, end == std::string::npos ? end : end - start);
    if (entry == wanted) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

/// An engine with a reed synth on destination @p dest: an engine whose force
/// axis a controller can actually reach, so a binding made below has somewhere
/// to land.
SonareRealtimeEngine* make_reed_engine(uint32_t dest) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);
  SonareSynthPatch patch{};
  patch.struct_version = 1;
  patch.engine_mode = SONARE_SYNTH_ENGINE_REED;
  REQUIRE(sonare_engine_set_synth_instrument(engine, dest, &patch) == SONARE_OK);
  return engine;
}

SonareControllerBinding breath_to_excitation() {
  SonareControllerBinding binding{};
  binding.input = SONARE_CONTROLLER_INPUT_CONTROL_CHANGE;
  binding.index = 2;
  binding.axis = SONARE_CONTROLLER_AXIS_EXCITATION;
  binding.lo = 0.0f;
  binding.hi = 1.0f;
  binding.curve = 1.0f;
  return binding;
}

/// Holds a note on @p dest while stepping CC2 across its range, and returns the
/// rendered left channel.
std::vector<float> render_breath_ramp(SonareRealtimeEngine* engine, uint32_t dest) {
  REQUIRE(sonare_engine_push_midi_note_on(engine, dest, 0, 0, 60, 100, -1) == SONARE_OK);
  std::vector<float> out;
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  for (int block = 0; block < 32; ++block) {
    const uint8_t value = static_cast<uint8_t>((block * 127) / 31);
    REQUIRE(sonare_engine_push_midi_cc(engine, dest, 0, 0, 2, value, -1) == SONARE_OK);
    REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
    out.insert(out.end(), left.begin(), left.end());
  }
  return out;
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)

}  // namespace

TEST_CASE("sonare_controller_profile_names lists the presets", "[c_api][controller]") {
  const char* names = sonare_controller_profile_names();
  REQUIRE(names != nullptr);
#if defined(SONARE_WITH_ARRANGEMENT)
  // Each preset is named rather than counted alone: a table that lost an entry
  // and gained an empty one keeps its count.
  REQUIRE(names_contain(names, "gm"));
  REQUIRE(names_contain(names, "breath"));
  REQUIRE(names_contain(names, "breath-aftertouch"));
  REQUIRE(names_contain(names, "mpe"));
  REQUIRE(name_count(names) >= 4);
#else
  REQUIRE(std::string(names).empty());
#endif
}

TEST_CASE("the controller enum name tables match their C counts", "[c_api][controller]") {
#if defined(SONARE_WITH_ARRANGEMENT)
  const char* inputs = sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_INPUT);
  const char* axes = sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_AXIS);
  REQUIRE(name_count(inputs) == SONARE_CONTROLLER_INPUT_COUNT);
  REQUIRE(name_count(axes) == SONARE_CONTROLLER_AXIS_COUNT);
  REQUIRE(names_contain(inputs, "channel-pressure"));
  REQUIRE(names_contain(axes, "excitation"));
  REQUIRE(names_contain(axes, "vibrato-depth"));
#else
  REQUIRE(std::string(sonare_synth_enum_names(SONARE_SYNTH_ENUM_CONTROLLER_INPUT)).empty());
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("a controller profile installs, binds, counts and clears", "[c_api][controller]") {
  SonareRealtimeEngine* engine = make_reed_engine(3);

  size_t count = 0;
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  const size_t gm_bindings = count;

  // A named preset replaces the table wholesale, so the count is the preset's
  // own rather than the sum of the two.
  REQUIRE(sonare_engine_set_controller_profile(engine, 3, "breath") == SONARE_OK);
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  const size_t breath_bindings = count;
  REQUIRE(breath_bindings > 0);

  // An unknown name is refused and changes nothing: a default that silently
  // replaced the device's spelling would still play, just not what was sent.
  REQUIRE(sonare_engine_set_controller_profile(engine, 3, "no-such-device") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  REQUIRE(count == breath_bindings);

  const SonareControllerBinding binding = breath_to_excitation();
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_OK);
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  REQUIRE(count == breath_bindings + 1);

  REQUIRE(sonare_engine_clear_controller_bindings(engine, 3) == SONARE_OK);
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  REQUIRE(count == 0);

  // The gm preset is still reachable after a clear, so clearing empties the
  // table rather than removing the profile. The count it restores has to be
  // non-zero, or this would pass just as well for a set that did nothing.
  REQUIRE(gm_bindings > 0);
  REQUIRE(sonare_engine_set_controller_profile(engine, 3, "gm") == SONARE_OK);
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  REQUIRE(count == gm_bindings);

  sonare_engine_destroy(engine);
}

TEST_CASE("a controller binding that cannot mean anything is refused", "[c_api][controller]") {
  SonareRealtimeEngine* engine = make_reed_engine(3);
  REQUIRE(sonare_engine_clear_controller_bindings(engine, 3) == SONARE_OK);

  SonareControllerBinding binding = breath_to_excitation();
  binding.axis = SONARE_CONTROLLER_AXIS_NONE;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);

  // A per-note value on a channel-level axis: refused rather than widened,
  // because a caller cannot tell a silently widened binding from one that took.
  binding = breath_to_excitation();
  binding.input = SONARE_CONTROLLER_INPUT_POLY_PRESSURE;
  binding.axis = SONARE_CONTROLLER_AXIS_LOUDNESS;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  // The same input on an excitation axis is accepted, so the refusal above is
  // about the axis and not about poly pressure.
  binding.axis = SONARE_CONTROLLER_AXIS_BRIGHTNESS;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_OK);

  // Ordinals past the enum and a CC number past 127. Each names its own field
  // in the last-error message: the code cannot carry the diagnosis, because an
  // unbound destination returns the same INVALID_PARAMETER.
  binding = breath_to_excitation();
  binding.input = SONARE_CONTROLLER_INPUT_COUNT;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("input") != std::string::npos);
  binding = breath_to_excitation();
  binding.axis = SONARE_CONTROLLER_AXIS_COUNT;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("axis") != std::string::npos);
  binding = breath_to_excitation();
  binding.index = 128;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("index") != std::string::npos);

  // A non-finite range or curve would reach the audio thread and stay there.
  binding = breath_to_excitation();
  binding.hi = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  binding = breath_to_excitation();
  binding.curve = 0.0f;
  REQUIRE(sonare_engine_bind_controller(engine, 3, &binding) == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_engine_bind_controller(engine, 3, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  // Nothing above was accepted except the one that was meant to be.
  size_t count = 0;
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  REQUIRE(count == 1);

  sonare_engine_destroy(engine);
}

TEST_CASE("the controller binding table fills rather than growing", "[c_api][controller]") {
  SonareRealtimeEngine* engine = make_reed_engine(3);
  REQUIRE(sonare_engine_clear_controller_bindings(engine, 3) == SONARE_OK);

  // Counted rather than asserted at the limit: an accept count that stops short
  // of the advertised capacity is as much a defect as one that runs past it.
  int accepted = 0;
  for (int i = 0; i < SONARE_MAX_CONTROLLER_BINDINGS + 4; ++i) {
    SonareControllerBinding binding = breath_to_excitation();
    binding.index = static_cast<uint8_t>(i);
    if (sonare_engine_bind_controller(engine, 3, &binding) == SONARE_OK) ++accepted;
  }
  REQUIRE(accepted == SONARE_MAX_CONTROLLER_BINDINGS);
  size_t count = 0;
  REQUIRE(sonare_engine_controller_binding_count(engine, 3, &count) == SONARE_OK);
  REQUIRE(count == static_cast<size_t>(SONARE_MAX_CONTROLLER_BINDINGS));

  sonare_engine_destroy(engine);
}

TEST_CASE("controller velocity meaningfulness round-trips", "[c_api][controller]") {
  SonareRealtimeEngine* engine = make_reed_engine(3);

  int meaningful = -1;
  REQUIRE(sonare_engine_controller_velocity_meaningful(engine, 3, &meaningful) == SONARE_OK);
  REQUIRE(meaningful == 1);
  REQUIRE(sonare_engine_set_controller_velocity_meaningful(engine, 3, 0) == SONARE_OK);
  REQUIRE(sonare_engine_controller_velocity_meaningful(engine, 3, &meaningful) == SONARE_OK);
  REQUIRE(meaningful == 0);
  // Any non-zero means true, so a caller passing a raw flag word is not
  // silently reading as false.
  REQUIRE(sonare_engine_set_controller_velocity_meaningful(engine, 3, 42) == SONARE_OK);
  REQUIRE(sonare_engine_controller_velocity_meaningful(engine, 3, &meaningful) == SONARE_OK);
  REQUIRE(meaningful == 1);

  sonare_engine_destroy(engine);
}

TEST_CASE("controller profile calls name the destination that has no instrument",
          "[c_api][controller]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  size_t count = 123;
  const SonareControllerBinding binding = breath_to_excitation();
  int meaningful = 123;
  REQUIRE(sonare_engine_set_controller_profile(engine, 5, "gm") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_bind_controller(engine, 5, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_clear_controller_bindings(engine, 5) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_controller_binding_count(engine, 5, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_controller_velocity_meaningful(engine, 5, &meaningful) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Both out-parameters are defined on the failure path rather than left at
  // whatever the caller had.
  REQUIRE(count == 0);
  REQUIRE(meaningful == 0);

  sonare_engine_destroy(engine);
}

TEST_CASE("a binding made through the C ABI reaches the sound", "[c_api][controller]") {
  // Bound: CC2 drives the reed's excitation axis.
  SonareRealtimeEngine* bound = make_reed_engine(3);
  REQUIRE(sonare_engine_clear_controller_bindings(bound, 3) == SONARE_OK);
  const SonareControllerBinding binding = breath_to_excitation();
  REQUIRE(sonare_engine_bind_controller(bound, 3, &binding) == SONARE_OK);
  const std::vector<float> with_binding = render_breath_ramp(bound, 3);
  sonare_engine_destroy(bound);

  // Unbound: the same ramp, the same patch, nothing bound.
  SonareRealtimeEngine* unbound = make_reed_engine(3);
  REQUIRE(sonare_engine_clear_controller_bindings(unbound, 3) == SONARE_OK);
  const std::vector<float> without_binding = render_breath_ramp(unbound, 3);
  sonare_engine_destroy(unbound);

  // The renders carry energy, so "they differ" is not two kinds of silence.
  float peak = 0.0f;
  for (const float s : without_binding) peak = std::max(peak, std::abs(s));
  REQUIRE(peak > 0.0f);
  REQUIRE(with_binding.size() == without_binding.size());
  REQUIRE(with_binding != without_binding);

  // And the unbound side is reproducible to the bit, so the difference above is
  // the binding rather than anything free-running in the render.
  SonareRealtimeEngine* again = make_reed_engine(3);
  REQUIRE(sonare_engine_clear_controller_bindings(again, 3) == SONARE_OK);
  REQUIRE(render_breath_ramp(again, 3) == without_binding);
  sonare_engine_destroy(again);
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
