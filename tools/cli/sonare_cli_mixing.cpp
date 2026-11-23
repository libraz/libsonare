#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "sonare_cli.h"

#ifdef SONARE_WITH_MIXING
mixing::PanMode parse_pan_mode_option(const std::string& value) {
  // The scene JSON's pan mode tokens; --pan-mode spells each in kebab-case.
  const std::string token = library_token("--pan-mode", value, {"balance", "stereoPan", "dualPan"});
  if (token == "stereoPan") return mixing::PanMode::StereoPan;
  if (token == "dualPan") return mixing::PanMode::DualPan;
  return mixing::PanMode::Balance;
}

int cmd_mixing_presets(const CliArgs& args, const Audio&) {
  const auto names = mixing::api::scene_preset_names();
  return print_name_catalog(args, "presets", names);
}

int cmd_mixing_preset(const CliArgs& args, const Audio&) {
  const std::string preset_name = catalog_token(args.get_string("preset", "vocalReverbSend"),
                                                mixing::api::scene_preset_names());
  const auto preset = mixing::api::scene_preset_from_string(preset_name);
  std::cout << mixing::api::scene_to_json(mixing::api::scene_preset(preset)) << "\n";
  return 0;
}

namespace {

/// Hold one decoded plane to the offline-input policy, reported in decoder terms
/// as every other decoded input is: an unusable rate is a malformed container,
/// any other refusal (a non-finite sample included) a failed decode.
void require_decoded_plane(const std::vector<float>& plane, int sample_rate) {
  const std::string problem =
      offline_audio_input_problem("input", plane.data(), plane.size(), sample_rate);
  if (!problem.empty()) {
    throw SonareException(
        is_supported_sample_rate(sample_rate) ? ErrorCode::DecodeFailed : ErrorCode::InvalidFormat,
        problem);
  }
}

// The block a scene is rendered in; the Python CLI renders in the same one.
constexpr size_t kMixHostChunkFrames = 4096;

/// One strip's stereo input at the mixer rate.
struct StripPair {
  std::vector<float> left;
  std::vector<float> right;
};

/// Load one file as the pair a strip processes: a stereo file keeps its two
/// channels, a mono one is carried on both sides, and a wider one is downmixed
/// with the warning every downmixing command gives.
StripPair load_strip_pair(const std::string& path, int sample_rate) {
  auto decoded = load_audio_input_once(path, RetainChannels::StereoIfExact);
  if (decoded.metadata_error != nullptr) std::rethrow_exception(decoded.metadata_error);
  StripPair pair;
  if (decoded.source_channels == 2) {
    if (decoded.retained_error != nullptr) std::rethrow_exception(decoded.retained_error);
    SONARE_CHECK(decoded.retained_channels == 2 && decoded.interleaved.size() % 2 == 0,
                 ErrorCode::DecodeFailed);
    const size_t frames = decoded.interleaved.size() / 2;
    pair.left.resize(frames);
    pair.right.resize(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
      pair.left[frame] = decoded.interleaved[2 * frame];
      pair.right[frame] = decoded.interleaved[2 * frame + 1];
    }
    require_decoded_plane(pair.right, decoded.sample_rate);
  } else {
    if (decoded.source_channels > 2) warn_downmixed_input(decoded.source_channels);
    pair.left = std::move(decoded.mono);
  }
  require_decoded_plane(pair.left, decoded.sample_rate);
  const bool stereo = decoded.source_channels == 2;
  if (decoded.sample_rate != sample_rate) {
    pair.left = resample(pair.left.data(), pair.left.size(), decoded.sample_rate, sample_rate);
    if (stereo) {
      pair.right = resample(pair.right.data(), pair.right.size(), decoded.sample_rate, sample_rate);
    }
  }
  if (!stereo) pair.right = pair.left;
  return pair;
}

std::string joined_ids(const std::vector<std::string>& ids) {
  std::string text;
  for (size_t i = 0; i < ids.size(); ++i) text += (i > 0 ? ", " : "") + ids[i];
  return text;
}

/// Resolve `--input [ID=]WAV` entries onto the scene's strips, in strip order.
///
/// An entry addressed by id (explicit, or a bare path whose base name names a
/// strip) feeds that strip and leaves the others silent, which is how a scene
/// with effect returns fed only by sends is rendered. Entries naming no strip
/// are positional, one per strip. Every buffer is padded to the longest input,
/// so a part entering late is not truncated away.
std::vector<StripPair> resolve_strip_inputs(const std::vector<std::string>& entries,
                                            const std::vector<std::string>& strip_ids,
                                            int sample_rate, size_t* length) {
  std::map<size_t, StripPair> resolved;
  std::vector<StripPair> positional;
  bool addressed = false;
  for (const std::string& entry : entries) {
    const auto separator = entry.find('=');
    std::string id;
    std::string path;
    if (separator == std::string::npos) {
      path = entry;
      id = bare_path_id(entry);
    } else {
      id = entry.substr(0, separator);
      path = entry.substr(separator + 1);
    }

    StripPair pair = load_strip_pair(path, sample_rate);
    const auto found = std::find(strip_ids.begin(), strip_ids.end(), id);
    if (found == strip_ids.end()) {
      // An explicit id is an assertion about the scene, so a miss is refused.
      if (separator != std::string::npos) {
        throw std::invalid_argument(
            "--input names strip '" + id +
            "', which the scene does not have (strips: " + joined_ids(strip_ids) + ")");
      }
      positional.push_back(std::move(pair));
      continue;
    }
    const auto index = static_cast<size_t>(found - strip_ids.begin());
    if (resolved.count(index) != 0) {
      throw std::invalid_argument("--input names strip '" + id + "' more than once");
    }
    addressed = true;
    resolved[index] = std::move(pair);
  }

  if (addressed && !positional.empty()) {
    std::vector<std::string> sorted = strip_ids;
    std::sort(sorted.begin(), sorted.end());
    throw std::invalid_argument(
        "--input entries must either all name a strip or all be positional; " + joined_ids(sorted) +
        " are the scene's strips");
  }
  if (!positional.empty() && positional.size() != strip_ids.size()) {
    throw std::invalid_argument("scene has " + std::to_string(strip_ids.size()) + " strips but " +
                                std::to_string(positional.size()) +
                                " inputs were given; name them as --input ID=WAV to feed only "
                                "some of them");
  }
  for (size_t i = 0; i < positional.size(); ++i) resolved[i] = std::move(positional[i]);

  *length = 0;
  for (const auto& entry : resolved) *length = std::max(*length, entry.second.left.size());
  std::vector<StripPair> strips(strip_ids.size());
  for (size_t i = 0; i < strips.size(); ++i) {
    const auto it = resolved.find(i);
    if (it != resolved.end()) strips[i] = std::move(it->second);
    strips[i].left.resize(*length, 0.0f);
    strips[i].right.resize(*length, 0.0f);
  }
  return strips;
}

/// The strip ids in the order the scene declares them.
std::vector<std::string> scene_strip_ids(const std::string& scene_json) {
  std::vector<std::string> ids;
  const auto root = sonare::util::json::parse(scene_json);
  const auto* strips = root.find("strips");
  if (strips == nullptr || !strips->is_array()) return ids;
  for (const auto& strip : strips->as_array()) {
    const auto* id = strip.find("id");
    ids.push_back(id != nullptr && id->is_string() ? id->as_string() : std::string());
  }
  return ids;
}

void check_mixer(SonareError err) {
  if (err != SONARE_OK) throw_c_failure(err, nullptr);
}

/// Append one rendered block to the output planes.
void append_block(const std::vector<float>& block_left, const std::vector<float>& block_right,
                  size_t count, std::vector<float>* left, std::vector<float>* right) {
  left->insert(left->end(), block_left.begin(), block_left.begin() + count);
  right->insert(right->end(), block_right.begin(), block_right.begin() + count);
}

}  // namespace

// `mix --scene FILE | --preset NAME [--input [ID=]WAV ... -o OUT]` -- load a
// mixer scene and, given inputs, render them through it, tail included.
int cmd_mix(const CliArgs& args, const Audio&) {
  const int sample_rate = args.get_int("sample-rate", kCliMixerSampleRate);
  std::string scene_json;
  if (args.has("scene")) {
    const std::string path = args.get_string("scene");
    std::vector<uint8_t> bytes;
    if (!read_binary_file(path, &bytes)) {
      throw SonareException(ErrorCode::FileNotFound, "cannot open scene file: " + path);
    }
    scene_json.assign(bytes.begin(), bytes.end());
  } else {
    const std::string preset =
        catalog_token(args.get_string("preset"), mixing::api::scene_preset_names());
    char* json = nullptr;
    check_mixer(sonare_mixing_scene_preset_json(preset.c_str(), &json));
    scene_json = json;
    sonare_free_string(json);
  }

  SonareMixer* raw = sonare_mixer_from_scene_json(scene_json.c_str(), sample_rate,
                                                  static_cast<int>(kMixHostChunkFrames));
  if (raw == nullptr) throw_c_failure(sonare_last_error_code(), nullptr);
  const std::unique_ptr<SonareMixer, decltype(&sonare_mixer_destroy)> mixer(raw,
                                                                            sonare_mixer_destroy);
  // Read the load's warnings before any later call clears them.
  for (size_t i = 0; i < sonare_last_diagnostic_count(); ++i) {
    SonareDiagnostic diagnostic;
    sonare_diagnostic_init(&diagnostic);
    if (sonare_last_diagnostic(i, &diagnostic) == SONARE_OK) {
      std::cerr << "warning: " << diagnostic.message << "\n";
    }
  }
  size_t strip_count = 0;
  check_mixer(sonare_mixer_get_strip_count(mixer.get(), &strip_count));

  const std::vector<std::string> entries = args.get_string_list("input");
  size_t rendered_samples = 0;
  OutputFit fit;
  if (!entries.empty()) {
    // The ids come from the scene text and the buffers go to the compiled
    // mixer, so a disagreement between the two would misalign every strip.
    const std::vector<std::string> strip_ids = scene_strip_ids(scene_json);
    if (strip_ids.size() != strip_count) {
      throw std::invalid_argument("scene text declares " + std::to_string(strip_ids.size()) +
                                  " strips but the mixer compiled " + std::to_string(strip_count));
    }
    size_t length = 0;
    std::vector<StripPair> strips = resolve_strip_inputs(entries, strip_ids, sample_rate, &length);
    check_mixer(sonare_mixer_compile(mixer.get()));
    int tail_samples = 0;
    check_mixer(sonare_mixer_tail_samples(mixer.get(), &tail_samples));

    std::vector<float> out_left;
    std::vector<float> out_right;
    out_left.reserve(length + static_cast<size_t>(tail_samples));
    out_right.reserve(length + static_cast<size_t>(tail_samples));
    std::vector<float> block_left(kMixHostChunkFrames);
    std::vector<float> block_right(kMixHostChunkFrames);
    std::vector<const float*> lefts(strips.size());
    std::vector<const float*> rights(strips.size());
    for (size_t offset = 0; offset < length; offset += kMixHostChunkFrames) {
      const size_t count = std::min(kMixHostChunkFrames, length - offset);
      for (size_t i = 0; i < strips.size(); ++i) {
        lefts[i] = strips[i].left.data() + offset;
        rights[i] = strips[i].right.data() + offset;
      }
      check_mixer(sonare_mixer_process_stereo(mixer.get(), lefts.data(), rights.data(),
                                              strips.size(), block_left.data(), block_right.data(),
                                              count));
      append_block(block_left, block_right, count, &out_left, &out_right);
    }
    for (size_t remaining = static_cast<size_t>(tail_samples); remaining > 0;) {
      const size_t count = std::min(kMixHostChunkFrames, remaining);
      check_mixer(sonare_mixer_drain_tail_stereo(mixer.get(), block_left.data(), block_right.data(),
                                                 count));
      append_block(block_left, block_right, count, &out_left, &out_right);
      remaining -= count;
    }
    rendered_samples = out_left.size();
    fit.include(out_left).include(out_right);
    write_stereo_wav(args.output_file, out_left, out_right, sample_rate, 16, fit);
    note_output_fit(args, fit);
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("strip_count", strip_count).kv("sample_rate", sample_rate);
    if (!entries.empty()) {
      json.kv("rendered_samples", rendered_samples)
          .kv("output", args.output_file)
          .kv("output_gain_db", fit.gain_db());
    }
    json.end_object().print();
  } else if (!args.quiet) {
    std::cout << "  Mixer:\n";
    std::cout << "    Strips:      " << strip_count << "\n";
    std::cout << "    Sample rate: " << sample_rate << " Hz\n";
    if (!entries.empty()) {
      std::cout << "    Rendered:    " << rendered_samples << " samples (stereo)\n";
      std::cout << "    Wrote: " << args.output_file << "\n";
    }
  }
  return 0;
}

int cmd_mix_strip(const CliArgs& args, const Audio& audio) {
  const float width = args.get_float("width", 1.0f);
  mixing::ChannelStrip strip;
  strip.set_input_trim_db(args.get_float("input-trim-db", 0.0f));
  strip.set_fader_db(args.get_float("fader-db", 0.0f));
  if (!strip.set_pan(args.get_float("pan", 0.0f))) {
    throw std::invalid_argument("--pan must be a finite value within [-1, 1]");
  }
  strip.set_pan_mode(parse_pan_mode_option(args.get_string("pan-mode", "balance")));
  if (!strip.set_width(width)) {
    throw std::invalid_argument("--width must be a finite value within [0, 2]");
  }

  std::vector<float> left;
  std::vector<float> right;
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
  } else {
    left.assign(audio.begin(), audio.end());
    right = left;
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

  const OutputFit fit = OutputFit().include(left).include(right);
  if (!args.output_file.empty()) {
    write_stereo_wav(args.output_file, left, right, audio.sample_rate(), 16, fit);
    note_output_fit(args, fit);
  }

  const auto meter = strip.meter_snapshot();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("sample_rate", audio.sample_rate()).kv("length", audio.size());
    if (!args.output_file.empty()) json.kv("output_gain_db", fit.gain_db());
    json.key("meter")
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
  /// The decoder's mono fold of a stereo entry, kept only where the tempo is measured on it.
  std::vector<float> mono_fold;
  int sample_rate = 0;
};

/// Decode one entry at its own rate, keeping a two-channel file as a pair.
///
/// The image domain is the one part of the assistant that reads both channels --
/// it measures interchannel cancellation, width and mono risk -- so a folded
/// copy of a stereo stem would have it describe a signal the caller never has.
/// A mono file stays mono rather than being duplicated across both sides, which
/// would reach the same domain as a stereo track of zero width. One codec pass
/// yields both the pair and the mono fold; the fold is kept only on request.
/// Every returned channel, the fold included, passes the offline-input policy
/// here, so no measurement (the tempo read included) sees an unchecked sample.
AssistantTrack decode_assistant_track(const std::string& entry, bool keep_mono_fold) {
  AssistantTrack track;
  std::string path;
  split_track_entry(entry, &track.id, &path);

  auto decoded = load_audio_input_once(path, RetainChannels::StereoIfExact);
  if (decoded.metadata_error != nullptr) std::rethrow_exception(decoded.metadata_error);
  if (decoded.source_channels == 2) {
    if (decoded.retained_error != nullptr) std::rethrow_exception(decoded.retained_error);
    SONARE_CHECK(decoded.retained_channels == 2 && decoded.interleaved.size() % 2 == 0,
                 ErrorCode::DecodeFailed);
    const size_t frames = decoded.interleaved.size() / 2;
    track.left.resize(frames);
    track.right.resize(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
      track.left[frame] = decoded.interleaved[2 * frame];
      track.right[frame] = decoded.interleaved[2 * frame + 1];
    }
    if (keep_mono_fold) track.mono_fold = std::move(decoded.mono);
  } else {
    if (decoded.source_channels > 2) warn_downmixed_input(decoded.source_channels);
    track.left = std::move(decoded.mono);
  }
  track.sample_rate = decoded.sample_rate;
  require_decoded_plane(track.left, track.sample_rate);
  if (!track.right.empty()) require_decoded_plane(track.right, track.sample_rate);
  if (!track.mono_fold.empty()) require_decoded_plane(track.mono_fold, track.sample_rate);
  return track;
}

/// Resolve `--tempo-bpm`, detecting it from the first entry when asked to.
///
/// `auto` measures the first `--input` and not the whole set: a tempo belongs to
/// the song, so every track shares one, and measuring the track the caller
/// listed first keeps which file was read visible in the command line. It is
/// measured on the mono fold at the file's own rate rather than at
/// --sample-rate, because resampling first would measure a different signal.
/// The decoded entry is returned through @p first_track for the track list; no tempo
/// found returns nothing.
std::optional<double> resolve_assistant_tempo(const std::string& raw,
                                              const std::string& first_entry,
                                              std::optional<AssistantTrack>* first_track) {
  const std::string lowered = lowercase_cli_value(raw);
  if (lowered != "auto") {
    std::istringstream stream(raw);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    // The registry domain refuses a number at or below 0; what is left is a non-number.
    if (!stream || stream.peek() != std::char_traits<char>::eof() || !std::isfinite(value)) {
      throw CliUsageError("invalid --tempo-bpm value: " + raw +
                          " (expected 'auto' or a tempo greater than 0)");
    }
    return value;
  }
  *first_track = decode_assistant_track(first_entry, true);
  const AssistantTrack& track = **first_track;
  const std::vector<float>& mono = track.right.empty() ? track.left : track.mono_fold;
  // The same entry the `bpm` command and the Python facade use. The analysis
  // class's own detect_bpm is a different estimator and answered 165 where both
  // front-ends answer 65 on the same file.
  const double bpm =
      static_cast<double>(quick::detect_bpm(mono.data(), mono.size(), track.sample_rate));
  std::vector<float>().swap(first_track->value().mono_fold);
  // No tempo found leaves the tempo unset, so the assistant takes its transport fallback.
  if (!(std::isfinite(bpm) && bpm > 0)) return std::nullopt;
  return bpm;
}

// The assistant's document re-keyed for stdout, as `mastering-suggest` does with
// its chain: `scene` is fed back to `mix --scene` verbatim, so the names under it
// belong to the scene schema; everything beside it is measurement output and
// follows the snake_case rule. A band token keying `band_occupancy` stays the
// token it is wherever else it appears as a value. A track field the core writes
// as null or, for `name` / `exclusionReason`, as an empty string was not
// measured or does not apply, so it is omitted.
std::string mix_suggestion_json_for_cli(const std::string& core_json) {
  namespace json = sonare::util::json;
  const json::Value document = json::parse(core_json);
  json::Object out;
  for (const auto& [key, child] : document.as_object()) {
    if (key == "scene") {
      out.emplace(key, child);
    } else if (key == "tracks") {
      json::Array tracks;
      for (const auto& track : child.as_array()) {
        json::Object row;
        for (const auto& [field, value] : track.as_object()) {
          const bool empty_text = (field == "name" || field == "exclusionReason") &&
                                  value.is_string() && value.as_string().empty();
          if (value.is_null() || empty_text) continue;
          row.emplace(json_key_to_snake_case(field),
                      field == "bandOccupancy" ? value : json_keys_to_snake_case(value));
        }
        tracks.emplace_back(json::Value(std::move(row)));
      }
      out.emplace(key, json::Value(std::move(tracks)));
    } else {
      out.emplace(json_key_to_snake_case(key), json_keys_to_snake_case(child));
    }
  }
  return json::dump(json::Value(std::move(out)));
}

}  // namespace

// `suggest-mix --input [ID=]a.wav --input [ID=]b.wav` -- measure a set of tracks
// and print the suggested mixer scene, its per-track profiles and the written
// reason for every decision.
int cmd_suggest_mix(const CliArgs& args, const Audio&) {
  const std::vector<std::string> entries = args.get_string_list("input");
  const int sample_rate = args.get_int("sample-rate", kCliMixerSampleRate);

  auto params = parse_mastering_params(args.get_string("params"));
  std::optional<AssistantTrack> first_track;
  if (args.has("tempo-bpm")) {
    const std::string tempo_text = args.get_string("tempo-bpm");
    if (const auto tempo = resolve_assistant_tempo(tempo_text, entries.front(), &first_track)) {
      params.push_back({"tempoBpm", *tempo});
    }
  }
  const auto config =
      mixing::assistant::mix_assistant_config_from_params(params.data(), params.size());

  std::vector<AssistantTrack> storage;
  storage.reserve(entries.size());
  for (const std::string& entry : entries) {
    if (storage.empty() && first_track.has_value()) {
      storage.push_back(std::move(*first_track));
    } else {
      storage.push_back(decode_assistant_track(entry, false));
    }
  }

  std::vector<mixing::assistant::TrackInput> inputs;
  inputs.reserve(storage.size());
  for (const AssistantTrack& track : storage) {
    mixing::assistant::TrackInput input;
    input.id = track.id;
    input.name = track.id;
    input.left = track.left.data();
    input.right = track.right.empty() ? nullptr : track.right.data();
    input.frame_count = track.left.size();
    input.sample_rate = track.sample_rate;
    inputs.push_back(input);
  }
  // Each track goes in at its own rate and is resampled onto --sample-rate, as the C ABI does.
  std::vector<std::vector<float>> resampled;
  const auto tracks = mixing::assistant::tracks_at_rate(std::move(inputs), sample_rate, resampled);
  const auto result = mixing::assistant::suggest_scene(tracks, config);

  const std::string scene_out = args.get_string("scene-out");
  if (!scene_out.empty()) {
    // Written from the result already in hand rather than through the
    // scene-only entry point, which would re-measure every track to reach the
    // same scene.
    const std::string payload = mixing::api::scene_to_json(result.scene) + '\n';
    if (!write_binary_file(scene_out, reinterpret_cast<const uint8_t*>(payload.data()),
                           payload.size())) {
      throw SonareException(ErrorCode::EncodeFailed, "cannot write scene: " + scene_out);
    }
  }
  std::cout << mix_suggestion_json_for_cli(mixing::assistant::mix_assistant_result_to_json(result))
            << "\n";
  return 0;
}
#endif
