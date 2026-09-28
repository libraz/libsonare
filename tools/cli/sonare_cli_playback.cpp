/// @file sonare_cli_playback.cpp
/// @brief `sonare playback`: render movie audio to speakers or headphones.

#include "sonare_cli.h"

#ifdef SONARE_WITH_PLAYBACK

#include <sonare/sonare_c_playback.h>

#include <fstream>
#include <iterator>
#include <optional>

namespace {

using sonare::util::json::Object;
using sonare::util::json::Value;

std::vector<uint8_t> read_hrtf_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) throw std::invalid_argument("cannot open --hrtf file: " + path);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

// SonareError and sonare::ErrorCode share the same numeric values for every
// code the C ABI can return here (SONARE_ERROR_UNKNOWN, 99, has no C++
// counterpart and is the one case handled separately).
[[noreturn]] void throw_playback_error(const std::string& what, SonareError err) {
  const std::string message = what + ": " + sonare_error_message(err);
  if (err == SONARE_OK || err == SONARE_ERROR_UNKNOWN) throw std::runtime_error(message);
  throw sonare::SonareException(static_cast<sonare::ErrorCode>(err), message);
}

// Overlays the CLI's flags onto the (optional) --config document; an absent
// flag leaves whatever the document already says (or the schema default)
// alone, so "flags win" only applies to a flag the caller actually gave.
Value playback_config_document(const CliArgs& args) {
  Value root = args.has("config")
                   ? sonare::util::json::parse(read_plain_text_file(args.get_string("config")))
                   : Value(Object{});
  if (!root.is_object()) root = Value(Object{});

  if (args.has("target")) {
    const std::string target = args.get_string("target");
    if (target == "headphones") {
      set_json_path(root, "target.kind", Value(std::string("headphones")));
      // headphones forbids target.layout; drop one a --config document set.
      auto& object = root.as_object();
      const auto it = object.find("target");
      if (it != object.end() && it->second.is_object()) it->second.as_object().erase("layout");
    } else {
      set_json_path(root, "target.kind", Value(std::string("speakers")));
      set_json_path(root, "target.layout", Value(target));
    }
  }
  if (args.has("input-layout")) {
    set_json_path(root, "input.layout", Value(args.get_string("input-layout")));
  }
  if (args.has("no-upmix")) set_json_path(root, "upmix.enabled", Value(false));
  if (args.has("night")) {
    set_json_path(root, "night_mode.amount",
                  Value(static_cast<double>(args.get_float("night", 0.0f))));
  }
  if (args.has("dialogue-db")) {
    set_json_path(root, "dialogue_level_db",
                  Value(static_cast<double>(args.get_float("dialogue-db", 0.0f))));
  }
  if (args.has("target-lufs")) {
    set_json_path(root, "loudness.target_lufs",
                  Value(static_cast<double>(args.get_float("target-lufs", -24.0f))));
  }
  if (args.has("room")) set_json_path(root, "room.preset", Value(args.get_string("room")));
  return root;
}

// The program loudness a config document ends up with: the caller's own
// --program-lufs, or (since this is an offline batch tool) a fresh measure of
// the whole input, matching the design's "measure the input, then render"
// contract for the flag's own absence. Returns nullopt when there is nothing
// to overlay -- an input too short or too quiet for BS.1770 gating to report
// anything measures at the library's generic dB floor, well outside the
// schema's accepted range, and the schema's own null already means exactly
// this case ("program = target, gain 0").
std::optional<float> resolved_program_lufs(const CliArgs& args,
                                           const std::vector<float>& interleaved, int in_channels,
                                           int sample_rate, size_t frames) {
  if (args.has("program-lufs")) return args.get_float("program-lufs", 0.0f);
  SonarePlaybackLoudnessMeter* meter = nullptr;
  SonareError err = sonare_playback_loudness_meter_create(in_channels, sample_rate, &meter);
  if (err != SONARE_OK) throw_playback_error("create loudness meter", err);
  err = sonare_playback_loudness_meter_push_interleaved(meter, interleaved.data(), frames);
  if (err != SONARE_OK) {
    sonare_playback_loudness_meter_destroy(meter);
    throw_playback_error("measure program loudness", err);
  }
  float lufs = 0.0f;
  err = sonare_playback_loudness_meter_integrated_lufs(meter, &lufs);
  sonare_playback_loudness_meter_destroy(meter);
  if (err != SONARE_OK) throw_playback_error("read program loudness", err);
  if (!std::isfinite(lufs) || lufs < -70.0f || lufs > 0.0f) return std::nullopt;
  return lufs;
}

}  // namespace

int cmd_playback(const CliArgs& args, const Audio&) {
  // The input file's channel count is a property of the file, not an option
  // value a registry domain can express, so it is checked here rather than by
  // a CliCommandValidator (which sees only CliArgs).
  const int probed_channels = args.source_channels;
  if (probed_channels != 1 && probed_channels != 2 && !is_surround_channel_count(probed_channels)) {
    throw std::invalid_argument("sonare playback accepts a 1, 2, 6, or 8 channel input file (got " +
                                std::to_string(probed_channels) + " channels)");
  }

  auto [interleaved, sample_rate, in_channels] = load_audio_interleaved(args.input_file);
  const size_t frames = in_channels > 0 ? interleaved.size() / static_cast<size_t>(in_channels) : 0;

  Value config_document = playback_config_document(args);
  const std::optional<float> program_lufs =
      resolved_program_lufs(args, interleaved, in_channels, sample_rate, frames);
  if (program_lufs.has_value()) {
    set_json_path(config_document, "loudness.program_lufs",
                  Value(static_cast<double>(*program_lufs)));
  }
  const std::string config_json = sonare::util::json::dump(config_document);

  std::vector<uint8_t> hrtf_bytes;
  SonareHrtfSet* hrtf = nullptr;
  if (args.has("hrtf")) {
    hrtf_bytes = read_hrtf_file(args.get_string("hrtf"));
    const SonareError hrtf_err =
        sonare_hrtf_set_create_from_memory(hrtf_bytes.data(), hrtf_bytes.size(), &hrtf);
    if (hrtf_err != SONARE_OK) throw_playback_error("load --hrtf", hrtf_err);
  }

  float* rendered = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  const SonareError render_err = sonare_playback_render_interleaved(
      interleaved.data(), frames, in_channels, sample_rate, config_json.c_str(), hrtf, &rendered,
      &out_frames, &out_channels);
  if (render_err != SONARE_OK) {
    if (hrtf != nullptr) sonare_hrtf_set_destroy(hrtf);
    throw_playback_error("render playback", render_err);
  }

  const ChannelLayout out_layout = layout_from_channel_count(out_channels);
  save_wav_multichannel(args.output_file, rendered, out_frames, out_channels, out_layout,
                        sample_rate);
  sonare_free_playback_render(rendered);

  // `--json` reports the renderer's own diagnostics (inactive stages, latency
  // breakdown, loudness/limiter clamps); a second, unprocessed handle is
  // created only to read them, since the one-shot render above owns no handle
  // of its own to ask.
  std::string diagnostics_text = "{}";
  if (args.json_output) {
    SonarePlaybackRenderer* renderer = nullptr;
    if (sonare_playback_renderer_create_json(config_json.c_str(), hrtf, sample_rate, 1024,
                                             &renderer) == SONARE_OK) {
      char* diagnostics = nullptr;
      if (sonare_playback_renderer_diagnostics_json(renderer, &diagnostics) == SONARE_OK &&
          diagnostics != nullptr) {
        diagnostics_text = diagnostics;
      }
      sonare_free_string(diagnostics);
      sonare_playback_renderer_destroy(renderer);
    }
  }
  if (hrtf != nullptr) sonare_hrtf_set_destroy(hrtf);

  if (args.json_output) {
    std::cout << diagnostics_text << "\n";
  } else if (!args.quiet) {
    std::cout << color::green << "Rendered " << out_frames << " frames (" << out_channels
              << " ch @ " << sample_rate << " Hz) to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

#endif  // SONARE_WITH_PLAYBACK
