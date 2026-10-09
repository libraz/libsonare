#include <utility>

#include "sonare_cli.h"

#ifdef SONARE_WITH_MIXING
mixing::PanMode parse_pan_mode_option(const std::string& value) {
  const std::string key = lowercase_cli_value(value);
  if (key == "balance") return mixing::PanMode::Balance;
  if (key == "stereopan" || key == "stereo-pan" || key == "pan") return mixing::PanMode::StereoPan;
  if (key == "dualpan" || key == "dual-pan") return mixing::PanMode::DualPan;
  throw std::invalid_argument("invalid pan mode: " + value);
}

int cmd_mixing_presets(const CliArgs& args, const Audio&) {
  const auto names = mixing::api::scene_preset_names();
  return print_name_catalog(args, "presets", names);
}

int cmd_mixing_preset(const CliArgs& args, const Audio&) {
  const std::string preset_name = args.get_string("preset", "vocalReverbSend");
  const auto preset = mixing::api::scene_preset_from_string(preset_name);
  std::cout << mixing::api::scene_to_json(mixing::api::scene_preset(preset)) << "\n";
  return 0;
}

int cmd_mix(const CliArgs& args, const Audio& audio) {
  const float width = args.get_float("width", 1.0f);
  mixing::ChannelStrip strip;
  strip.set_input_trim_db(args.get_float("input-trim-db", 0.0f));
  strip.set_fader_db(args.get_float("fader-db", 0.0f));
  strip.set_pan(args.get_float("pan", 0.0f));
  strip.set_pan_mode(parse_pan_mode_option(args.get_string("pan-mode", "balance")));
  strip.set_width(width);

  std::vector<float> left(audio.begin(), audio.end());
  std::vector<float> right(audio.begin(), audio.end());
  // main() already probed the channel count for this invocation and stored it;
  // re-probing here read the file again and gave the handler a second, parallel
  // answer that nothing kept in step with the first.
  const int source_channels = args.source_channels;
  if (source_channels == 2) {
    auto planes = load_stereo_planes(args, audio);
    left = std::move(planes.left);
    right = std::move(planes.right);
  } else if (width != 1.0f) {
    std::cerr << color::red << "Error: --width requires a stereo input" << color::reset << "\n";
    return 1;
  }
  // Both the prepared block size and the processed frame count come from the
  // buffers actually being processed, not from the mono decode that sized
  // nothing here. The two agree on a well-formed static file, but that was an
  // assumption nothing enforced: on a stereo input `left`/`right` are resized
  // from the interleaved decode, so any disagreement between the decodes would
  // have read past or short of the real buffers.
  const auto frames = static_cast<int>(left.size());
  strip.prepare(static_cast<double>(audio.sample_rate()), frames);
  float* channels[] = {left.data(), right.data()};
  strip.process(channels, 2, frames);

  if (!args.output_file.empty()) {
    save_stereo_wav(args.output_file, left, right, audio.sample_rate());
  }

  const auto meter = strip.meter_snapshot();
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("sample_rate", audio.sample_rate())
        .kv("length", audio.size())
        .key("meter")
        .begin_object()
        .kv("peak_db_l", meter.peak_db[0])
        .kv("peak_db_r", meter.peak_db[1])
        .kv("rms_db_l", meter.rms_db[0])
        .kv("rms_db_r", meter.rms_db[1])
        .kv("correlation", meter.correlation)
        .kv("mono_compat_width", meter.mono_compat_width)
        .kv("likely_mono_compatible", meter.likely_mono_compatible)
        .kv("max_true_peak_db", meter.max_true_peak_db)
        .end_object()
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << "Mixed " << audio.size() << " samples";
    if (!args.output_file.empty()) std::cout << " -> " << args.output_file;
    std::cout << "\n";
    std::cout << "Correlation: " << meter.correlation
              << ", mono-compatible: " << (meter.likely_mono_compatible ? "yes" : "no") << "\n";
  }
  return 0;
}
#endif

#ifdef SONARE_WITH_MIXING_ASSISTANT
namespace {

/// One `--input` entry, decoded and held for the duration of the suggestion.
///
/// TrackInput points into these buffers rather than owning them, so the tracks
/// and their storage have to outlive the call together.
struct AssistantTrack {
  std::string id;
  std::vector<float> left;
  std::vector<float> right;
  int sample_rate = 0;
};

/// Split `[ID=]WAV` into its two halves. A bare path takes the file's own name
/// as the id, which is what the scene then addresses the strip by.
void split_track_entry(const std::string& entry, std::string* id, std::string* path) {
  const auto separator = entry.find('=');
  if (separator == std::string::npos) {
    *path = entry;
    const auto slash = entry.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? entry : entry.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    *id = dot == std::string::npos ? base : base.substr(0, dot);
  } else {
    *id = entry.substr(0, separator);
    *path = entry.substr(separator + 1);
  }
  if (id->empty()) throw std::invalid_argument("--input track id must not be empty: " + entry);
  if (path->empty()) throw std::invalid_argument("--input requires a file path: " + entry);
}

/// Load one entry at @p sample_rate, keeping a two-channel file as a pair.
///
/// The image domain is the one part of the assistant that reads both channels --
/// it measures interchannel cancellation, width and mono risk -- so a folded
/// copy of a stereo stem would have it describe a signal the caller never has.
/// A mono file stays mono rather than being duplicated across both sides, which
/// would reach the same domain as a stereo track of zero width.
AssistantTrack load_assistant_track(const std::string& entry, int sample_rate) {
  AssistantTrack track;
  std::string path;
  split_track_entry(entry, &track.id, &path);

  int source_rate = 0;
  if (audio_channel_count(path) == 2) {
    auto [interleaved, rate, channels] = load_audio_interleaved(path);
    source_rate = rate;
    const size_t frames = channels > 0 ? interleaved.size() / static_cast<size_t>(channels) : 0;
    track.left.resize(frames);
    track.right.resize(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
      track.left[frame] = interleaved[frame * static_cast<size_t>(channels)];
      track.right[frame] = interleaved[frame * static_cast<size_t>(channels) + 1];
    }
  } else {
    auto [samples, rate] = load_audio(path);
    source_rate = rate;
    track.left = std::move(samples);
  }
  validate_offline_audio_input(track.left.data(), track.left.size(), source_rate);
  if (source_rate != sample_rate) {
    track.left = resample(track.left.data(), track.left.size(), source_rate, sample_rate);
    if (!track.right.empty()) {
      track.right = resample(track.right.data(), track.right.size(), source_rate, sample_rate);
    }
  }
  track.sample_rate = sample_rate;
  return track;
}

/// Resolve `--tempo-bpm`, detecting it from the first entry when asked to.
///
/// `auto` measures the first `--input` and not the whole set: a tempo belongs to
/// the song, so every track shares one, and measuring the track the caller
/// listed first keeps which file was read visible in the command line. It is
/// decoded at its own rate rather than at --sample-rate, because resampling
/// first would measure a different signal.
double resolve_assistant_tempo(const std::string& raw, const std::string& first_entry) {
  const std::string lowered = lowercase_cli_value(raw);
  if (lowered != "auto") {
    std::istringstream stream(raw);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (!stream || stream.peek() != std::char_traits<char>::eof()) {
      throw std::invalid_argument("invalid --tempo-bpm value: " + raw);
    }
    return value;
  }
  std::string id;
  std::string path;
  split_track_entry(first_entry, &id, &path);
  auto [samples, rate] = load_audio(path);
  // The same entry the `bpm` command and the Python facade use. The analysis
  // class's own detect_bpm is a different estimator and answered 165 where both
  // front-ends answer 65 on the same file.
  return static_cast<double>(quick::detect_bpm(samples.data(), samples.size(), rate));
}

}  // namespace

// `suggest-mix --input [ID=]a.wav --input [ID=]b.wav` -- measure a set of tracks
// and print the suggested mixer scene, its per-track profiles and the written
// reason for every decision.
//
// The document is printed exactly as the core produced it, in camelCase, unlike
// every neighbouring command in this file: `scene` is fed straight back to the
// mixer through `mix --scene`, so the names under it belong to the scene schema
// that will read them rather than to this CLI's snake_case stdout convention,
// and re-keying the rest would split one document across two conventions.
int cmd_suggest_mix(const CliArgs& args, const Audio&) {
  const std::vector<std::string> entries = args.get_string_list("input");
  if (entries.empty()) {
    throw std::invalid_argument("suggest-mix requires at least one --input");
  }
  const int sample_rate = args.get_int("sample-rate", 48000);

  auto params = parse_mastering_params(args.get_string("params"));
  const std::string tempo_text = args.get_string("tempo-bpm");
  if (!tempo_text.empty()) {
    // Both spellings reach the same config field, so naming both is a
    // contradiction rather than a precedence question.
    for (const auto& param : params) {
      if (param.key == "tempoBpm" || param.key == "tempo_bpm") {
        throw std::invalid_argument("--tempo-bpm and --params tempoBpm= set the same value");
      }
    }
    params.push_back({"tempoBpm", resolve_assistant_tempo(tempo_text, entries.front())});
  }
  const auto config =
      mixing::assistant::mix_assistant_config_from_params(params.data(), params.size());

  std::vector<AssistantTrack> storage;
  storage.reserve(entries.size());
  for (const std::string& entry : entries) {
    storage.push_back(load_assistant_track(entry, sample_rate));
  }

  std::vector<mixing::assistant::TrackInput> tracks;
  tracks.reserve(storage.size());
  for (const AssistantTrack& track : storage) {
    mixing::assistant::TrackInput input;
    input.id = track.id;
    input.name = track.id;
    input.left = track.left.data();
    input.right = track.right.empty() ? nullptr : track.right.data();
    input.frame_count = track.left.size();
    input.sample_rate = track.sample_rate;
    tracks.push_back(input);
  }

  const auto result = mixing::assistant::suggest_scene(tracks, config);

  const std::string scene_out = args.get_string("scene-out");
  if (!scene_out.empty()) {
    // Written from the result already in hand rather than through the
    // scene-only entry point, which would re-measure every track to reach the
    // same scene.
    std::ofstream file(scene_out, std::ios::binary);
    file << mixing::api::scene_to_json(result.scene) << "\n";
    // The class save_wav gives a failed render, as the report writer above uses.
    if (!file) {
      throw SonareException(ErrorCode::EncodeFailed, "cannot write scene: " + scene_out);
    }
  }
  std::cout << mixing::assistant::mix_assistant_result_to_json(result) << "\n";
  return 0;
}
#endif
