#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include "playback/config.h"
#include "playback/renderer.h"
#include "util/exception.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

TEST_CASE("an empty document takes the schema defaults and round-trips", "[playback][config]") {
  RendererConfig config;
  REQUIRE_NOTHROW(config = parse_renderer_config("{}"));
  CHECK(config.prepare.input_layout == InputLayout::Auto);
  CHECK(config.prepare.target_kind == TargetKind::Headphones);
  CHECK(config.prepare.room_preset == RoomPreset::LivingRoom);
  CHECK(config.realtime.upmix_enabled);
  CHECK(config.realtime.target_lufs == -24.0f);
  CHECK(config.realtime.limiter_ceiling_db == -1.0f);

  RendererConfig again;
  REQUIRE_NOTHROW(again = parse_renderer_config(serialize_renderer_config(config)));
  CHECK(first_prepare_difference(config.prepare, again.prepare).empty());
  CHECK(again.realtime.room_mix_db == config.realtime.room_mix_db);
}

TEST_CASE("an unknown key is an invalid parameter", "[playback][config]") {
  sonare::ErrorCode code = sonare::ErrorCode::Ok;
  try {
    parse_renderer_config(R"({"bogus": 1})");
  } catch (const sonare::SonareException& e) {
    code = e.code();
  }
  CHECK(code == sonare::ErrorCode::InvalidParameter);
}

TEST_CASE("set_config refuses a changed prepare key", "[playback][config]") {
  RendererConfig config;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::Stereo;
  PlaybackRenderer renderer(config, nullptr, 48000, 512);
  RendererConfig changed = config;
  changed.prepare.target_layout = ChannelLayout::FivePointOne;
  std::string message;
  try {
    renderer.set_config(changed);
  } catch (const sonare::SonareException& e) {
    message = e.what();
  }
  CHECK(message.find("requires a new renderer: target.layout") != std::string::npos);
}
