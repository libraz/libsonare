#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/golden_hash.h"
#include "support/sf2_builder.h"
#include "util/constants.h"

using sonare::constants::kPi;

namespace {

constexpr int kSourceSampleRate = 48000;
constexpr int kTargetSampleRate = 24000;
constexpr int kFrames = 48000;
constexpr int kBlock = 128;

struct Scenario {
  std::string name;
  int normalize_lufs = 0;
  float target_lufs = -18.0f;
  int dither = 0;
  int dither_bits = 16;
  uint32_t dither_seed = 0;
};

std::pair<std::vector<float>, std::vector<float>> make_clip(const std::string& name) {
  std::vector<float> left(kFrames, 0.0f);
  std::vector<float> right(kFrames, 0.0f);
  for (int i = 0; i < kFrames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSourceSampleRate);
    if (name == "tone") {
      left[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * sonare::constants::kPi * 220.0f * t);
      right[static_cast<size_t>(i)] = 0.18f * std::sin(2.0f * sonare::constants::kPi * 330.0f * t);
    } else {
      const int local = i % 6000;
      const float transient = local < 128 ? 1.0f - static_cast<float>(local) / 128.0f : 0.0f;
      left[static_cast<size_t>(i)] =
          0.12f * std::sin(2.0f * sonare::constants::kPi * 110.0f * t) + 0.32f * transient;
      right[static_cast<size_t>(i)] =
          -0.10f * std::sin(2.0f * sonare::constants::kPi * 165.0f * t) - 0.22f * transient;
    }
  }
  return {left, right};
}

std::string hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << value;
  return out.str();
}

std::vector<Scenario> scenarios() {
  return {
      {"src-only", 0, -18.0f, 0, 16, 0},
      {"src-lufs-tpdf", 1, -18.0f, 2, 16, 0x12345678u},
      {"src-lufs-noiseshaped", 1, -20.0f, 3, 16, 0x87654321u},
  };
}

std::tuple<std::string, int64_t, int, int, float> run_bounce(const std::string& signal,
                                                             const Scenario& scenario) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSourceSampleRate, kBlock, 256, 256) == SONARE_OK);

  auto [left, right] = make_clip(signal);
  const float* clip_channels[] = {left.data(), right.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = kFrames;
  clip.start_ppq = 0.0;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);

  SonareEngineBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = kBlock;
  options.num_channels = 2;
  options.source_sample_rate = kSourceSampleRate;
  options.target_sample_rate = kTargetSampleRate;
  options.normalize_lufs = scenario.normalize_lufs;
  options.target_lufs = scenario.target_lufs;
  options.dither = scenario.dither;
  options.dither_bits = scenario.dither_bits;
  options.dither_seed = scenario.dither_seed;
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.interleaved != nullptr);
  REQUIRE(result.frames == kTargetSampleRate);
  REQUIRE(result.num_channels == 2);
  REQUIRE(result.sample_rate == kTargetSampleRate);
  REQUIRE(result.sample_count == static_cast<size_t>(kTargetSampleRate * 2));
  if (scenario.normalize_lufs) {
    REQUIRE(std::abs(result.integrated_lufs - scenario.target_lufs) < 0.35f);
  } else {
    REQUIRE(std::isfinite(result.integrated_lufs));
  }
  const std::string hash =
      hex64(sonare::test::fnv1a_quantized(result.interleaved, result.sample_count));
  const int64_t frames = result.frames;
  const int channels = result.num_channels;
  const int sample_rate = result.sample_rate;
  const float integrated_lufs = result.integrated_lufs;
  sonare_free_floats(result.interleaved);
  sonare_engine_destroy(engine);
  return {hash, frames, channels, sample_rate, integrated_lufs};
}

// A looped tone driven through Sf2Player's GS system-effect bus (reverb +
// chorus + delay send-returns), the one signal path in this file that touches
// a SoundFont-backed synth rather than a pre-rendered clip.
constexpr double kSf2SampleRate = 48000.0;
constexpr int kSf2Block = 256;
constexpr int kSf2Frames = 24000;  // 0.5 s

std::shared_ptr<sonare::midi::synth::Sf2File> make_sf2_fixture() {
  sonare::test::Sf2Builder builder;
  std::vector<float> tone(64);
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.6f * std::sin(2.0f * sonare::constants::kPi * static_cast<float>(i) /
                              static_cast<float>(tone.size()));
  }
  const int sample_id =
      builder.add_sample("tone", tone, 32000, 60, 0, static_cast<uint32_t>(tone.size()));

  sonare::test::Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});  // loop the whole sample
  zone.target = sample_id;
  const int instrument_id = builder.add_instrument("tone", {zone});

  sonare::test::Sf2Builder::ZoneSpec preset_zone;
  preset_zone.target = instrument_id;
  builder.add_preset("Tone", 0, 0, {preset_zone});

  const auto bytes = builder.build();
  auto sf2 = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

sonare::midi::MidiEvent sf2_event(const sonare::midi::Ump& ump) {
  sonare::midi::MidiEvent e;
  e.ump = ump;
  return e;
}

std::tuple<std::string, int64_t, int, int> run_sf2_gs_effects_bounce() {
  using sonare::midi::synth::Sf2Player;
  using sonare::midi::synth::Sf2PlayerConfig;

  Sf2PlayerConfig config;
  config.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  config.effects.enable_reverb = true;
  config.effects.enable_chorus = true;
  config.effects.enable_delay = true;
#endif
  Sf2Player player(config);
  player.set_soundfont(make_sf2_fixture());
  player.prepare(kSf2SampleRate, kSf2Block);

  player.on_event(0, sf2_event(sonare::midi::make_midi1_control_change(0, 0, 91, 100)));
  player.on_event(0, sf2_event(sonare::midi::make_midi1_control_change(0, 0, 93, 90)));
  player.on_event(0, sf2_event(sonare::midi::make_midi1_control_change(0, 0, 94, 80)));
  player.on_event(0, sf2_event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));

  std::vector<float> left(kSf2Frames, 0.0f);
  std::vector<float> right(kSf2Frames, 0.0f);
  for (int rendered = 0; rendered < kSf2Frames;) {
    const int chunk = std::min(kSf2Block, kSf2Frames - rendered);
    float* block_channels[] = {left.data() + rendered, right.data() + rendered};
    player.process(block_channels, 2, chunk);
    rendered += chunk;
  }

  const std::string hash = hex64(sonare::test::fnv1a_quantized_stereo(left, right));
  return {hash, kSf2Frames, 2, static_cast<int>(kSf2SampleRate)};
}

std::vector<std::tuple<std::string, std::string, std::string, int64_t, int, int>> compute_rows() {
  const std::array<std::string, 2> signals{"tone", "transient"};
  std::vector<std::tuple<std::string, std::string, std::string, int64_t, int, int>> rows;
  for (const auto& scenario : scenarios()) {
    for (const auto& signal : signals) {
      auto [hash, frames, channels, sample_rate, _lufs] = run_bounce(signal, scenario);
      rows.emplace_back(scenario.name, signal, hash, frames, channels, sample_rate);
    }
  }
  {
    auto [hash, frames, channels, sample_rate] = run_sf2_gs_effects_bounce();
    rows.emplace_back("sf2-gs-effects", "reverb-chorus-delay", hash, frames, channels, sample_rate);
  }
  return rows;
}

std::map<std::string, std::tuple<std::string, int64_t, int, int>> load_manifest(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::tuple<std::string, int64_t, int, int>> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string scenario;
    std::string signal;
    std::string hash;
    std::string frames;
    std::string channels;
    std::string sample_rate;
    std::getline(stream, scenario, '\t');
    std::getline(stream, signal, '\t');
    std::getline(stream, hash, '\t');
    std::getline(stream, frames, '\t');
    std::getline(stream, channels, '\t');
    std::getline(stream, sample_rate, '\t');
    out[scenario + "/" + signal] =
        std::make_tuple(hash, std::stoll(frames), std::stoi(channels), std::stoi(sample_rate));
  }
  return out;
}

void write_manifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# scenario\tsignal\tfnv1a_quantized_interleaved_hash\tframes\tchannels\tsample_rate\n";
  for (const auto& [scenario, signal, hash, frames, channels, sample_rate] : compute_rows()) {
    file << scenario << '\t' << signal << '\t' << hash << '\t' << frames << '\t' << channels << '\t'
         << sample_rate << '\n';
  }
}

#if defined(SONARE_WITH_MIXING)
// Automation target id for a lane parameter, the packing
// engine::make_track_lane_param_id applies (1 = fader dB).
constexpr uint32_t engine_lane_param_target(uint32_t lane_index, uint32_t param_kind) {
  return 0x4D580000u | (lane_index << 8u) | param_kind;
}

// Engine with one unity clip on a lane whose fader has been driven to -12 dB
// but never rendered, so the fader smoother still holds its reset value. Any
// offline entry point has to open at the target, not ramp down into it.
struct AttenuatedLaneEngine {
  static constexpr int kBlock = 128;
  static constexpr int kRate = 48000;
  static constexpr int kLength = kBlock * 8;
  static constexpr float kFaderDb = -12.0f;

  std::vector<float> left = std::vector<float>(kLength, 1.0f);
  std::vector<float> right = std::vector<float>(kLength, 1.0f);
  SonareRealtimeEngine* engine = nullptr;

  AttenuatedLaneEngine() {
    REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine, kRate, kBlock, 256, 256) == SONARE_OK);

    const float* clip_channels[] = {left.data(), right.data()};
    SonareEngineClip clip{};
    clip.id = 1;
    clip.track_id = 10;
    clip.channels = clip_channels;
    clip.num_channels = 2;
    clip.num_samples = kLength;
    clip.length_samples = kLength;
    clip.gain = 1.0f;
    REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);

    SonareEngineTrackLane lane[] = {{10, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
    REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
    REQUIRE(sonare_engine_set_parameter(engine, engine_lane_param_target(0, 1), kFaderDb, -1) ==
            SONARE_OK);
    REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
    REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);
  }
  ~AttenuatedLaneEngine() { sonare_engine_destroy(engine); }
};
#endif  // defined(SONARE_WITH_MIXING)

}  // namespace

#if defined(SONARE_WITH_MIXING)
TEST_CASE("every offline entry point opens at settled parameter values",
          "[engine][offline][bounce]") {
  // A bounce and a freeze are one-shot renders, so there is no earlier audio for
  // a fader to ramp in from: the caller asked for the lane as configured. Both
  // entry points therefore pre-roll the engine (drain the queued commands,
  // render one discarded block with the transport held, snap the smoothers) the
  // way the project bounce path does. Without it the opening milliseconds come
  // out up to the full fader travel too loud, which is inaudible as a defect in
  // a long bounce and obvious when a freeze is used as a clip.
  using Engine = AttenuatedLaneEngine;
  const auto steady_state = [](const SonareEngineBounceResult& result) {
    return result.interleaved[result.sample_count - 2];
  };

  SECTION("bounce_offline") {
    Engine fixture;
    SonareEngineBounceOptions options{};
    options.total_frames = Engine::kLength;
    options.block_size = Engine::kBlock;
    options.num_channels = 2;
    options.source_sample_rate = Engine::kRate;
    options.target_sample_rate = Engine::kRate;
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(fixture.engine, &options, &result) == SONARE_OK);
    REQUIRE(result.interleaved != nullptr);
    REQUIRE(result.sample_count == static_cast<size_t>(Engine::kLength) * 2u);

    const float settled = steady_state(result);
    CAPTURE(result.interleaved[0], settled);
    // The fader really is attenuating, so "the first sample equals the settled
    // one" is not satisfied by a bounce that ignored the fader entirely.
    REQUIRE(settled > 0.05f);
    REQUIRE(settled < 0.5f);
    REQUIRE(result.interleaved[0] == Catch::Approx(settled).epsilon(0.01));
    sonare_free_floats(result.interleaved);
  }

  SECTION("freeze_offline") {
    Engine fixture;
    SonareEngineFreezeOptions freeze{};
    freeze.total_frames = Engine::kLength;
    freeze.block_size = Engine::kBlock;
    freeze.num_channels = 2;
    freeze.clip_id = 7;
    freeze.gain = 1.0f;
    SonareEngineFreezeResult frozen{};
    REQUIRE(sonare_engine_freeze_offline(fixture.engine, &freeze, &frozen) == SONARE_OK);
    REQUIRE(frozen.clip_id == 7u);

    // The freeze replaced the engine's clips with the captured audio, and the
    // frozen clip carries no track id, so bouncing it reads the capture back
    // without passing it through the lane a second time.
    REQUIRE(sonare_engine_seek_sample(fixture.engine, 0, -1) == SONARE_OK);
    SonareEngineBounceOptions options{};
    options.total_frames = Engine::kLength;
    options.block_size = Engine::kBlock;
    options.num_channels = 2;
    options.source_sample_rate = Engine::kRate;
    options.target_sample_rate = Engine::kRate;
    SonareEngineBounceResult result{};
    REQUIRE(sonare_engine_bounce_offline(fixture.engine, &options, &result) == SONARE_OK);
    REQUIRE(result.interleaved != nullptr);

    const float settled = steady_state(result);
    CAPTURE(result.interleaved[0], settled);
    REQUIRE(settled > 0.05f);
    REQUIRE(settled < 0.5f);
    REQUIRE(result.interleaved[0] == Catch::Approx(settled).epsilon(0.01));
    sonare_free_floats(result.interleaved);
  }
}
#endif  // defined(SONARE_WITH_MIXING)

TEST_CASE("realtime engine offline bounce golden hashes stay stable",
          "[.][engine][offline][bounce][golden]") {
  const std::filesystem::path manifest = "tests/engine/golden/offline_bounce_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_ENGINE_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == 7);
  REQUIRE(expected.size() == rows.size());

  for (const auto& [scenario, signal, hash, frames, channels, sample_rate] : rows) {
    const std::string key = scenario + "/" + signal;
    CAPTURE(key);
    const auto [expected_hash, expected_frames, expected_channels, expected_sample_rate] =
        expected.at(key);
    REQUIRE(expected_hash == hash);
    REQUIRE(expected_frames == frames);
    REQUIRE(expected_channels == channels);
    REQUIRE(expected_sample_rate == sample_rate);
  }
}
