#include <catch2/catch_test_macros.hpp>
#include <string>

#include "playback/config.h"
#include "playback/renderer.h"
#include "util/exception.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

/// Message of the InvalidParameter a document is refused with; empty when it parses.
std::string refusal(const std::string& json) {
  try {
    parse_renderer_config(json);
  } catch (const sonare::SonareException& e) {
    CHECK(e.code() == sonare::ErrorCode::InvalidParameter);
    return e.what();
  }
  return {};
}

bool mentions(const std::string& message, const std::string& text) {
  return message.find(text) != std::string::npos;
}

RendererConfig speakers_config(ChannelLayout layout) {
  RendererConfig config;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = layout;
  return config;
}

}  // namespace

TEST_CASE("an empty document takes the schema defaults and round-trips", "[playback][config]") {
  RendererConfig config;
  REQUIRE_NOTHROW(config = parse_renderer_config("{}"));
  CHECK(config.prepare.input_layout == InputLayout::Auto);
  CHECK_FALSE(config.prepare.has_channel_map);
  CHECK(config.prepare.target_kind == TargetKind::Headphones);
  CHECK(config.prepare.room_preset == RoomPreset::LivingRoom);
  CHECK_FALSE(config.prepare.bass_management.enabled);
  CHECK(config.prepare.bass_management.crossover_hz == 80.0f);
  CHECK(config.prepare.bass_management.subwoofer);
  for (const SpeakerPrepare& speaker : config.prepare.speakers) {
    CHECK_FALSE(speaker.has_distance);
    CHECK(speaker.size == SpeakerSize::Large);
  }
  const RealtimeConfig& rt = config.realtime;
  for (float trim : rt.trim_db) CHECK(trim == 0.0f);
  CHECK(rt.lfe_gain_db == 10.0f);
  CHECK(rt.lfe_mix_db == 0.0f);
  CHECK(rt.upmix_enabled);
  CHECK(rt.upmix_center_width == 0.2f);
  CHECK(rt.upmix_front_ambience == 0.5f);
  CHECK_FALSE(rt.upmix_lfe_from_upmix);
  CHECK(rt.dialogue_level_db == 0.0f);
  CHECK_FALSE(rt.has_program_lufs);
  CHECK(rt.target_lufs == -24.0f);
  CHECK(rt.night_amount == 0.0f);
  CHECK(rt.room_mix_db == -6.0f);
  CHECK(rt.room_enabled);
  CHECK(rt.head_tracking_enabled);
  CHECK(rt.limiter_enabled);
  CHECK(rt.limiter_ceiling_db == -1.0f);

  RendererConfig again;
  REQUIRE_NOTHROW(again = parse_renderer_config(serialize_renderer_config(config)));
  CHECK(first_prepare_difference(config.prepare, again.prepare).empty());
  CHECK(serialize_renderer_config(again) == serialize_renderer_config(config));
}

TEST_CASE("a partial document fills only the omitted keys", "[playback][config]") {
  const RendererConfig config = parse_renderer_config(
      R"({"upmix": {"enabled": false}, "night_mode": {"amount": 0.5},
          "target": {"kind": "speakers", "layout": "5.1", "speakers": {"C": {"trim_db": -3}}}})");
  CHECK_FALSE(config.realtime.upmix_enabled);
  CHECK(config.realtime.upmix_center_width == 0.2f);
  CHECK(config.realtime.night_amount == 0.5f);
  CHECK(config.realtime.limiter_enabled);
  CHECK(config.prepare.target_kind == TargetKind::Speakers);
  CHECK(config.prepare.target_layout == ChannelLayout::FivePointOne);
  CHECK(config.realtime.trim_db[static_cast<size_t>(SpeakerRole::C)] == -3.0f);
  CHECK(config.realtime.trim_db[static_cast<size_t>(SpeakerRole::L)] == 0.0f);
  CHECK(config.prepare.room_preset == RoomPreset::LivingRoom);
}

TEST_CASE("an unknown key is refused by name", "[playback][config]") {
  CHECK(mentions(refusal(R"({"bogus": 1})"), "bogus"));
  CHECK(mentions(refusal(R"({"upmix": {"width": 0.3}})"), "upmix.width"));
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers", "layout": "5.1",
                               "speakers": {"L": {"delay_ms": 1}}}})"),
                 "target.speakers.L.delay_ms"));
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers", "layout": "5.1",
                               "speakers": {"LFE": {"trim_db": 1}}}})"),
                 "target.speakers.LFE"));
}

TEST_CASE("an out-of-range value is refused by name", "[playback][config]") {
  CHECK(mentions(refusal(R"({"night_mode": {"amount": 1.5}})"), "night_mode.amount"));
  CHECK(mentions(refusal(R"({"room": {"mix_db": 7}})"), "room.mix_db"));
  CHECK(mentions(refusal(R"({"loudness": {"program_lufs": -71}})"), "loudness.program_lufs"));
  CHECK(mentions(refusal(R"({"upmix": {"center_width": 0.01}})"), "upmix.center_width"));
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers", "layout": "7.1",
                               "speakers": {"Lss": {"distance_m": 31}}}})"),
                 "target.speakers.Lss.distance_m"));
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers", "layout": "stereo",
                               "bass_management": {"crossover_hz": 20}}})"),
                 "target.bass_management.crossover_hz"));
  CHECK(
      mentions(refusal(R"({"output_limiter": {"ceiling_db": 0.5}})"), "output_limiter.ceiling_db"));
  CHECK(mentions(refusal(R"({"room": {"preset": "cathedral"}})"), "room.preset"));
  CHECK(mentions(refusal(R"({"upmix": {"enabled": 1}})"), "upmix.enabled"));
}

TEST_CASE("a number no float can hold is refused, not narrowed", "[playback][config]") {
  const std::string message = refusal(R"({"loudness": {"target_lufs": -1e39}})");
  CHECK(mentions(message, "loudness.target_lufs"));
  CHECK(mentions(message, "32-bit float"));
}

TEST_CASE("target.layout is required for speakers and refused for headphones",
          "[playback][config]") {
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers"}})"), "target.layout"));
  CHECK(mentions(refusal(R"({"target": {"layout": "5.1"}})"), "target.layout"));
  CHECK(mentions(refusal(R"({"target": {"kind": "headphones", "layout": "stereo"}})"),
                 "target.layout"));
  CHECK(
      mentions(refusal(R"({"target": {"kind": "speakers", "layout": "mono"}})"), "target.layout"));
  CHECK(
      mentions(refusal(R"({"target": {"speakers": {"L": {"trim_db": 1}}}})"), "target.speakers.L"));
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers", "layout": "5.1",
                               "speakers": {"Lss": {"trim_db": 1}}}})"),
                 "target.speakers.Lss"));
}

TEST_CASE("a channel map needs a fixed layout and a valid role set", "[playback][config]") {
  CHECK(mentions(refusal(R"({"input": {"channel_map": ["L", "R"]}})"),
                 "channel_map requires a fixed input.layout"));
  CHECK(mentions(refusal(R"({"input": {"layout": "auto", "channel_map": ["L", "R"]}})"),
                 "channel_map requires a fixed input.layout"));
  CHECK_FALSE(refusal(R"({"input": {"layout": "stereo", "channel_map": ["L", "L"]}})").empty());
  CHECK_FALSE(refusal(R"({"input": {"layout": "stereo", "channel_map": ["L"]}})").empty());
  CHECK_FALSE(refusal(R"({"input": {"layout": "stereo", "channel_map": ["L", "C"]}})").empty());
  CHECK_FALSE(refusal(R"({"input": {"layout": "stereo", "channel_map": ["L", "Q"]}})").empty());

  const RendererConfig aac = parse_renderer_config(
      R"({"input": {"layout": "5.1", "channel_map": ["C", "L", "R", "Ls", "Rs", "LFE"]}})");
  CHECK(aac.prepare.has_channel_map);
  CHECK(aac.prepare.channel_map[0] == SpeakerRole::C);
  CHECK(aac.prepare.channel_map[5] == SpeakerRole::LFE);
}

TEST_CASE("bass management needs a destination for the low band", "[playback][config]") {
  CHECK(mentions(refusal(R"({"target": {"kind": "speakers", "layout": "5.1",
                               "speakers": {"L": {"size": "small"}},
                               "bass_management": {"enabled": true, "subwoofer": false}}})"),
                 "target.bass_management.subwoofer"));
  CHECK(refusal(R"({"target": {"kind": "speakers", "layout": "5.1",
                     "speakers": {"Ls": {"size": "small"}},
                     "bass_management": {"enabled": true, "subwoofer": false}}})")
            .empty());
}

TEST_CASE("a complete document round-trips through serialize and parse", "[playback][config]") {
  RendererConfig config = speakers_config(ChannelLayout::SevenPointOne);
  config.prepare.input_layout = InputLayout::FivePointOne;
  config.prepare.has_channel_map = true;
  config.prepare.channel_map = {SpeakerRole::C,  SpeakerRole::L,  SpeakerRole::R,
                                SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::LFE};
  SpeakerPrepare& ls = config.prepare.speakers[static_cast<size_t>(SpeakerRole::Ls)];
  ls.has_distance = true;
  ls.distance_m = 2.35f;
  ls.size = SpeakerSize::Small;
  config.prepare.bass_management.enabled = true;
  config.prepare.bass_management.crossover_hz = 95.5f;
  config.prepare.room_preset = RoomPreset::HomeTheater;
  RealtimeConfig& rt = config.realtime;
  rt.trim_db[static_cast<size_t>(SpeakerRole::Rss)] = -2.7f;
  rt.lfe_gain_db = 7.3f;
  rt.lfe_mix_db = -4.1f;
  rt.upmix_enabled = false;
  rt.upmix_center_width = 0.33f;
  rt.upmix_front_ambience = 0.1f;
  rt.upmix_lfe_from_upmix = true;
  rt.dialogue_level_db = 4.5f;
  rt.has_program_lufs = true;
  rt.program_lufs = -31.2f;
  rt.target_lufs = -23.0f;
  rt.night_amount = 0.7f;
  rt.room_mix_db = -11.0f;
  rt.room_enabled = false;
  rt.head_tracking_enabled = false;
  rt.limiter_enabled = false;
  rt.limiter_ceiling_db = -2.5f;

  const std::string text = serialize_renderer_config(config);
  const RendererConfig back = parse_renderer_config(text);
  CHECK(first_prepare_difference(config.prepare, back.prepare).empty());
  CHECK(back.prepare.channel_map == config.prepare.channel_map);
  CHECK(back.prepare.target_layout == ChannelLayout::SevenPointOne);
  const RealtimeConfig& b = back.realtime;
  CHECK(b.trim_db == rt.trim_db);
  CHECK(b.lfe_gain_db == rt.lfe_gain_db);
  CHECK(b.lfe_mix_db == rt.lfe_mix_db);
  CHECK(b.upmix_enabled == rt.upmix_enabled);
  CHECK(b.upmix_center_width == rt.upmix_center_width);
  CHECK(b.upmix_front_ambience == rt.upmix_front_ambience);
  CHECK(b.upmix_lfe_from_upmix == rt.upmix_lfe_from_upmix);
  CHECK(b.dialogue_level_db == rt.dialogue_level_db);
  CHECK(b.has_program_lufs == rt.has_program_lufs);
  CHECK(b.program_lufs == rt.program_lufs);
  CHECK(b.target_lufs == rt.target_lufs);
  CHECK(b.night_amount == rt.night_amount);
  CHECK(b.room_mix_db == rt.room_mix_db);
  CHECK(b.room_enabled == rt.room_enabled);
  CHECK(b.head_tracking_enabled == rt.head_tracking_enabled);
  CHECK(b.limiter_enabled == rt.limiter_enabled);
  CHECK(b.limiter_ceiling_db == rt.limiter_ceiling_db);
  CHECK(serialize_renderer_config(back) == text);
}

TEST_CASE("set_config refuses a changed prepare key", "[playback][config]") {
  RendererConfig config = speakers_config(ChannelLayout::Stereo);
  PlaybackRenderer renderer(config, nullptr, 48000, 512);

  const auto refusal_of = [&](const RendererConfig& changed) {
    try {
      renderer.set_config(changed);
    } catch (const sonare::SonareException& e) {
      CHECK(e.code() == sonare::ErrorCode::InvalidParameter);
      return std::string(e.what());
    }
    return std::string();
  };

  RendererConfig changed = config;
  changed.prepare.target_layout = ChannelLayout::FivePointOne;
  CHECK(mentions(refusal_of(changed), "requires a new renderer: target.layout"));
  changed = config;
  changed.prepare.room_preset = RoomPreset::None;
  CHECK(mentions(refusal_of(changed), "requires a new renderer: room.preset"));
  changed = config;
  changed.prepare.speakers[static_cast<size_t>(SpeakerRole::L)].has_distance = true;
  changed.prepare.speakers[static_cast<size_t>(SpeakerRole::L)].distance_m = 2.0f;
  CHECK(mentions(refusal_of(changed), "requires a new renderer: target.speakers.L.distance_m"));
  changed = config;
  changed.prepare.input_layout = InputLayout::Stereo;
  CHECK(mentions(refusal_of(changed), "requires a new renderer: input.layout"));
  CHECK(renderer.config().prepare.target_layout == ChannelLayout::Stereo);

  // Realtime keys go through, and the renderer reports them back.
  changed = config;
  changed.realtime.night_amount = 0.75f;
  changed.realtime.upmix_enabled = false;
  CHECK(refusal_of(changed).empty());
  CHECK(renderer.config().realtime.night_amount == 0.75f);
  CHECK_FALSE(renderer.config().realtime.upmix_enabled);
  CHECK(renderer.latency_samples() == 288);
}

TEST_CASE("the renderer refuses an invalid configuration, rate or block size",
          "[playback][config]") {
  RendererConfig config = speakers_config(ChannelLayout::FivePointOne);
  config.realtime.night_amount = 2.0f;
  CHECK_THROWS_AS(PlaybackRenderer(config, nullptr, 48000, 512), sonare::SonareException);
  const RendererConfig valid = speakers_config(ChannelLayout::FivePointOne);
  CHECK_THROWS_AS(PlaybackRenderer(valid, nullptr, 0, 512), sonare::SonareException);
  CHECK_THROWS_AS(PlaybackRenderer(valid, nullptr, 48000, 0), sonare::SonareException);
}

TEST_CASE("diagnostics name the inactive stages and the latency split", "[playback][config]") {
  RendererConfig config = speakers_config(ChannelLayout::FivePointOne);
  config.prepare.input_layout = InputLayout::Stereo;
  config.realtime.dialogue_level_db = 6.0f;
  config.realtime.has_program_lufs = true;
  config.realtime.program_lufs = -70.0f;
  config.realtime.target_lufs = -24.0f;
  PlaybackRenderer renderer(config, nullptr, 48000, 512);
  const RendererDiagnostics d = renderer.diagnostics();
  const auto inactive = [&](Stage stage) {
    return (d.inactive_stages & (1u << static_cast<unsigned>(stage))) != 0;
  };
  CHECK(inactive(Stage::DialogueLevel));  // stereo input has no discrete centre
  CHECK_FALSE(inactive(Stage::Upmix));
  CHECK_FALSE(inactive(Stage::Loudness));
  CHECK(inactive(Stage::NightMode));
  CHECK(inactive(Stage::Binaural));
  CHECK_FALSE(inactive(Stage::OutputLimiter));
  CHECK(d.loudness_gain_db == 12.0f);
  CHECK(d.loudness_gain_clamped);
  CHECK(d.active_input_layout == ChannelLayout::Stereo);
  CHECK(d.stage_latency_q8[static_cast<size_t>(Stage::Upmix)] == 1024 * 256);
  CHECK(d.stage_latency_q8[static_cast<size_t>(Stage::NightMode)] == 240 * 256);
  CHECK(d.stage_latency_q8[static_cast<size_t>(Stage::OutputLimiter)] == 48 * 256);
  CHECK(d.latency_samples == 1312);

  const std::string json = diagnostics_to_json(d);
  CHECK(mentions(json, "\"active_input_layout\":\"stereo\""));
  CHECK(mentions(json, "\"dialogue_level\""));
  CHECK(mentions(json, "\"upmix\":{\"samples\":1024,\"q8\":262144}"));
  CHECK(mentions(json, "\"layout_switches\":0"));
  CHECK(mentions(json, "\"truncated_drains\":0"));
}
