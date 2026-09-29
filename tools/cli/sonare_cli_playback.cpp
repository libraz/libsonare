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

// Matches src/playback/renderer.h's kOfflineRenderBlockFrames: the chunk
// size the C++ one-shot reference render loop uses.
constexpr size_t kOfflineRenderBlockFrames = 4096;

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
  Value root = Value(Object{});
  if (args.has("config")) {
    const std::string path = args.get_string("config");
    root = sonare::util::json::parse(read_plain_text_file(path));
    // Refused, not silently treated as an absent --config: matching the
    // Python CLI's own --config path, which rejects a non-object root.
    if (!root.is_object()) {
      throw std::invalid_argument("--config JSON root must be an object: " + path);
    }
  }

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

// Renders `frames` input frames through `renderer` block by block, mirroring
// `playback::render_interleaved` (src/playback/renderer.cpp): the tail is
// padded with `latency_samples()` silent frames to flush the pipeline, and
// that many frames are dropped from the front of the output, so the returned
// audio and any diagnostics read from `renderer` afterward both come from the
// same, actually-processed handle.
std::vector<float> render_via_reporting_handle(SonarePlaybackRenderer* renderer,
                                               const std::vector<float>& interleaved,
                                               int in_channels, int out_channels, size_t frames) {
  int latency = 0;
  SonareError err = sonare_playback_renderer_latency_samples(renderer, &latency);
  if (err != SONARE_OK) throw_playback_error("query playback renderer", err);

  const size_t total = frames + static_cast<size_t>(latency);
  std::vector<float> result(frames * static_cast<size_t>(out_channels), 0.0f);
  std::vector<float> in_block(kOfflineRenderBlockFrames * static_cast<size_t>(in_channels), 0.0f);
  std::vector<float> out_block(kOfflineRenderBlockFrames * static_cast<size_t>(out_channels), 0.0f);

  for (size_t start = 0; start < total; start += kOfflineRenderBlockFrames) {
    const size_t count = std::min(kOfflineRenderBlockFrames, total - start);
    std::fill(in_block.begin(), in_block.begin() + count * in_channels, 0.0f);
    if (start < frames) {
      const size_t available = std::min(count, frames - start);
      std::copy(interleaved.begin() + start * in_channels,
                interleaved.begin() + (start + available) * in_channels, in_block.begin());
    }
    err = sonare_playback_renderer_process_interleaved(renderer, in_block.data(), in_channels,
                                                       out_block.data(), out_channels,
                                                       static_cast<int>(count));
    if (err != SONARE_OK) throw_playback_error("render playback", err);

    const size_t lo = latency > static_cast<int>(start) ? static_cast<size_t>(latency) - start : 0;
    if (lo < count) {
      const size_t dest_start = start + lo - static_cast<size_t>(latency);
      std::copy(out_block.begin() + lo * out_channels, out_block.begin() + count * out_channels,
                result.begin() + dest_start * out_channels);
    }
  }
  return result;
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

  SonarePlaybackRenderer* renderer = nullptr;
  const SonareError create_err =
      sonare_playback_renderer_create_json(config_json.c_str(), hrtf, sample_rate,
                                           static_cast<int>(kOfflineRenderBlockFrames), &renderer);
  if (create_err != SONARE_OK) {
    if (hrtf != nullptr) sonare_hrtf_set_destroy(hrtf);
    throw_playback_error("create playback renderer", create_err);
  }

  int out_channels = 0;
  SonareError query_err = sonare_playback_renderer_output_channel_count(renderer, &out_channels);
  if (query_err != SONARE_OK) {
    sonare_playback_renderer_destroy(renderer);
    if (hrtf != nullptr) sonare_hrtf_set_destroy(hrtf);
    throw_playback_error("query playback renderer", query_err);
  }

  // Rendering and (below) diagnostics both read from `renderer`, so a
  // limiter/loudness clamp or a non-finite discard the render itself
  // triggers is visible in the report.
  std::vector<float> rendered;
  try {
    rendered =
        render_via_reporting_handle(renderer, interleaved, in_channels, out_channels, frames);
  } catch (...) {
    sonare_playback_renderer_destroy(renderer);
    if (hrtf != nullptr) sonare_hrtf_set_destroy(hrtf);
    throw;
  }

  const ChannelLayout out_layout = layout_from_channel_count(out_channels);
  save_wav_multichannel(args.output_file, rendered.data(), frames, out_channels, out_layout,
                        sample_rate);

  std::string diagnostics_text = "{}";
  if (args.json_output) {
    char* diagnostics = nullptr;
    if (sonare_playback_renderer_diagnostics_json(renderer, &diagnostics) == SONARE_OK &&
        diagnostics != nullptr) {
      diagnostics_text = diagnostics;
    }
    sonare_free_string(diagnostics);
  }
  sonare_playback_renderer_destroy(renderer);
  if (hrtf != nullptr) sonare_hrtf_set_destroy(hrtf);

  if (args.json_output) {
    std::cout << diagnostics_text << "\n";
  } else if (!args.quiet) {
    std::cout << color::green << "Rendered " << frames << " frames (" << out_channels << " ch @ "
              << sample_rate << " Hz) to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

#endif  // SONARE_WITH_PLAYBACK
