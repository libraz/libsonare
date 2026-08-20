#include "sonare_cli.h"
#include "util/constants.h"

// Offline-effect output contract: a command that renders an audio buffer
// requires -o/--output. Running it without a destination has no useful result
// (the render would be computed and thrown away), so the missing-output case is
// a hard error mapped to the invalid-parameter exit code, not a silent no-op.
// trim-silence is the deliberate exception: it doubles as an analysis command
// (it reports the trimmed length), so its output stays optional.

int cmd_pitch_shift(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: pitch-shift requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("semitones")) {
    std::cerr << color::red << "Error: --semitones required" << color::reset << "\n";
    return 1;
  }

  float semitones = args.get_float("semitones", 0.0f);
  PitchShiftConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Pitch shifting by " << semitones << " semitones..." << color::reset
              << "\n";
  }

  Audio result = pitch_shift(audio, semitones, config);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("semitones", semitones)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_time_stretch(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: time-stretch requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("rate")) {
    std::cerr << color::red << "Error: --rate required" << color::reset << "\n";
    return 1;
  }

  float rate = args.get_float("rate", 1.0f);
  TimeStretchConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Time stretching with rate " << rate << "..." << color::reset
              << "\n";
  }

  Audio result = time_stretch(audio, rate, config);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("rate", rate)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_pitch_correct(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: pitch-correct requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }

  const float current_midi = args.get_float("current-midi", 69.0f);
  const float target_midi = args.get_float("target-midi", 69.0f);
  editing::pitch_editor::PitchCorrector corrector;
  editing::pitch_editor::F0Track track;
  track.sample_rate = audio.sample_rate();
  // The command corrects to one constant pitch, so the track it builds is a
  // single frame and carries a fixed hop purely to satisfy the F0Track
  // contract. Reading the global --hop-length here would consume an option the
  // command's schema does not accept, and no facade exposes a hop control for
  // constant-pitch correction on any surface.
  track.hop_length = sonare::constants::kDefaultHopLength;
  track.f0_hz = {editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
  track.voiced = {true};
  track.voiced_prob = {1.0f};

  Audio result = corrector.correct_to_midi(audio, track, target_midi);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("current_midi", current_midi)
        .kv("target_midi", target_midi)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_note_stretch(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: note-stretch requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }

  editing::pitch_editor::NoteRegion region;
  region.onset_sample = args.get_int("onset", 0);
  region.offset_sample = args.get_int("offset", 0);
  const float ratio = args.get_float("ratio", 1.0f);

  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, ratio);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("onset_sample", region.onset_sample)
        .kv("offset_sample", region.offset_sample)
        .kv("ratio", ratio)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// The four voice-changer commands below stay registered in the CLI's command
// table regardless of BUILD_VOICE_CHANGER (see get_commands() in
// tools/sonare_cli.cpp), so a build without the voice changer must still
// answer the subcommand -- with a NotImplemented diagnostic mapped to the
// CLI's not-supported exit code -- instead of failing to link.
#if defined(SONARE_WITH_VOICE_CHANGER)

int cmd_voice_change(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: voice-change requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }

  const bool has_preset = args.has("preset");
  const bool has_preset_json = args.has("preset-json");
  const bool has_preset_pack = args.has("preset-pack");
  const int selector_count = static_cast<int>(has_preset) + static_cast<int>(has_preset_json);
  if (selector_count > 1) {
    throw std::invalid_argument(
        "voice-change preset selectors are mutually exclusive: choose one of --preset, "
        "--preset-json, or --preset-pack");
  }
  if (has_preset_pack && has_preset_json) {
    throw std::invalid_argument("--preset-pack and --preset-json are mutually exclusive");
  }
  // A pack names the file, --preset names the entry inside it, so the pair is
  // one selector. This check precedes the ones below so that a pack without an
  // entry reports the missing --preset rather than a downstream rule that reads
  // as if no selector had been given at all.
  if (has_preset_pack && !has_preset) {
    throw std::invalid_argument("--preset-pack requires --preset to select an entry");
  }
  if (selector_count > 0 && (args.has("pitch-semitones") || args.has("formant-factor"))) {
    throw std::invalid_argument(
        "--pitch-semitones/--formant-factor cannot be combined with a realtime preset");
  }
  if (args.has("set") && selector_count == 0) {
    throw std::invalid_argument("--set requires --preset, --preset-json, or --preset-pack");
  }
  const bool uses_realtime_preset = selector_count > 0 || args.has("set");

  Audio result;
  std::string preset_id;
  int latency_samples = 0;
  float pitch_semitones = 0.0f;
  float formant_factor = 1.0f;
  if (uses_realtime_preset) {
    const std::string requested_preset = args.get_string("preset", "");
    // Only advertise an ID when it identifies the selected source. A
    // --preset-json document has its own identity (or may be anonymous), so
    // falling back to the neutral preset here would be misleading. A
    // --preset-pack entry remains identified by the explicit --preset value.
    if (has_preset) preset_id = requested_preset;
    std::string config_text = requested_preset;
    if (args.has("preset-json")) {
      config_text = read_plain_text_file(args.get_string("preset-json"));
    } else if (args.has("preset-pack")) {
      config_text = find_voice_preset_in_pack(read_plain_text_file(args.get_string("preset-pack")),
                                              requested_preset);
    } else if (args.has("set")) {
      const auto id =
          editing::voice_changer::realtime_voice_changer_preset_from_id(requested_preset);
      config_text = editing::voice_changer::realtime_voice_changer_preset_json(id);
    }
    if (args.has("set")) config_text = apply_voice_preset_sets(config_text, args.get_string("set"));

    // Route through the same strict validator as the C ABI / Python entry
    // points (realtime_voice_changer_config_from_input) instead of the
    // tolerant realtime_voice_changer_config_from_json: a mistyped section
    // name, a missing "dsp" wrapper, or a partial hand-written preset must
    // fail loudly rather than silently render with unrelated defaults.
    editing::voice_changer::RealtimeVoiceChangerConfig config;
    std::string config_error;
    if (!editing::voice_changer::realtime_voice_changer_config_from_input(config_text, &config,
                                                                          &config_error)) {
      throw std::invalid_argument("invalid voice preset: " + config_error);
    }
    editing::voice_changer::RealtimeVoiceChanger changer(config);
    // Block size and pre-roll/drop latency compensation mirror the C-ABI
    // oracle (process_realtime_voice_change_compensated in
    // src/c_api/sonare_c_voice_changer.cpp): pad the input by the chain
    // latency, process in fixed 128-sample blocks for bit-identical DSP
    // across surfaces, then drop the leading pre-roll so output sample k
    // corresponds to input sample k and the output length equals the input
    // length.
    constexpr int kBlock = 128;
    changer.prepare(audio.sample_rate(), kBlock, 1);
    latency_samples = std::max(changer.latency_samples(), 0);
    const size_t latency_frames = static_cast<size_t>(latency_samples);
    const size_t total = audio.size() + latency_frames;
    std::vector<float> padded_input(total, 0.0f);
    std::copy(audio.data(), audio.data() + audio.size(), padded_input.begin());
    std::vector<float> padded_output(total, 0.0f);
    for (size_t pos = 0; pos < total; pos += kBlock) {
      const int n = static_cast<int>(std::min<size_t>(kBlock, total - pos));
      changer.process_block(padded_input.data() + pos, padded_output.data() + pos, n);
    }
    std::vector<float> output(
        padded_output.begin() + static_cast<std::ptrdiff_t>(latency_frames),
        padded_output.begin() + static_cast<std::ptrdiff_t>(latency_frames + audio.size()));
    result = Audio::from_vector(std::move(output), audio.sample_rate());
  } else {
    pitch_semitones = args.get_float("pitch-semitones", 0.0f);
    formant_factor = args.get_float("formant-factor", 1.0f);
    editing::voice_changer::VoiceChangerConfig config;
    config.pitch_semitones = pitch_semitones;
    config.formant_factor = formant_factor;
    editing::voice_changer::VoiceChanger changer(config);
    result = changer.process(audio);
  }

  // Pitch/formant processing uses spectral transforms whose boundary
  // convention can produce one extra (or one missing) sample.  The CLI's
  // voice-change contract is sample-preserving, so normalize both branches
  // to the input length before writing the artifact or reporting metadata.
  if (result.size() != audio.size()) {
    std::vector<float> sized_result(audio.size(), 0.0f);
    const size_t copy_size = std::min(result.size(), audio.size());
    if (copy_size > 0) {
      std::copy(result.data(), result.data() + copy_size, sized_result.begin());
    }
    result = Audio::from_vector(std::move(sized_result), audio.sample_rate());
  }
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("output", args.output_file)
        .kv("length", result.size())
        .kv("duration", result.duration())
        .kv("sample_rate", result.sample_rate())
        .kv("latency_samples", latency_samples);
    if (uses_realtime_preset && !preset_id.empty()) {
      json.kv("preset", preset_id);
    } else if (!uses_realtime_preset) {
      // Offline voice-change path: echo the simple pitch/formant knobs the
      // caller supplied so JSON consumers can correlate input args with the
      // result without re-parsing CLI flags.
      json.kv("pitch_semitones", pitch_semitones).kv("formant_factor", formant_factor);
    }
    json.end_object().print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_voice_presets(const CliArgs& args, const Audio&) {
  const auto names = editing::voice_changer::realtime_voice_changer_preset_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key("presets").begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

int cmd_voice_preset(const CliArgs& args, const Audio&) {
  const std::string preset = args.get_string("preset", "neutral-monitor");
  const auto id = editing::voice_changer::realtime_voice_changer_preset_from_id(preset);
  std::cout << editing::voice_changer::realtime_voice_changer_preset_json(id) << "\n";
  return 0;
}

int cmd_voice_preset_validate(const CliArgs& args, const Audio&) {
  const std::string path = args.get_string("preset-json", args.input_file);
  if (path.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::FileNotFound,
                                  "voice-preset-validate requires a JSON file");
  }
  // read_plain_text_file historically reports open failures as
  // std::invalid_argument. Classify the pre-validation missing-file case as
  // FileNotFound so it maps to exit 4 (or legacy exit 1) and never emits a
  // JSON validation envelope.
  {
    std::ifstream input(path);
    if (!input.is_open()) {
      throw sonare::SonareException(sonare::ErrorCode::FileNotFound,
                                    "cannot open text file: " + path);
    }
  }
  std::string config_text = read_plain_text_file(path);
  if (args.has("preset")) {
    config_text = find_voice_preset_in_pack(config_text, args.get_string("preset"));
  }
  if (args.has("set")) {
    config_text = apply_voice_preset_sets(config_text, args.get_string("set"));
  }
  std::string normalized;
  std::string error;
  if (!editing::voice_changer::validate_realtime_voice_changer_preset_json(config_text, &normalized,
                                                                           &error)) {
    if (error.empty()) error = "invalid voice preset";
    if (args.json_output) {
      JsonBuilder().begin_object().kv("ok", false).kv("error", error).end_object().print();
    } else {
      std::cerr << error << "\n";
    }
    return 3;
  }
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("ok", true).kv("normalized_json", normalized).end_object().print();
  } else {
    std::cout << normalized << "\n";
  }
  return 0;
}

#else  // !SONARE_WITH_VOICE_CHANGER

int cmd_voice_change(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

int cmd_voice_presets(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

int cmd_voice_preset(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

int cmd_voice_preset_validate(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

#endif  // SONARE_WITH_VOICE_CHANGER

int cmd_hpss(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: hpss requires output prefix (-o)" << color::reset << "\n";
    return 1;
  }

  const int output_mode_count = static_cast<int>(args.has("harmonic-only")) +
                                static_cast<int>(args.has("percussive-only")) +
                                static_cast<int>(args.has("with-residual"));
  if (output_mode_count > 1) {
    throw std::invalid_argument(
        "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
        "--percussive-only, or --with-residual");
  }

  HpssConfig config;
  config.kernel_size_harmonic = args.get_int("kernel-harmonic", 31);
  config.kernel_size_percussive = args.get_int("kernel-percussive", 31);
  config.use_soft_mask = !args.has("hard-mask");

  StftConfig stft{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Performing harmonic-percussive separation..." << color::reset
              << "\n";
  }

  std::string base = args.output_file;
  if (base.size() > 4 && base.substr(base.size() - 4) == ".wav") {
    base = base.substr(0, base.size() - 4);
  }

  auto save_audio = [](const std::string& path, const Audio& a) {
    save_wav(path, a.data(), a.size(), a.sample_rate());
  };

  if (args.has("harmonic-only")) {
    std::string path = base + ".wav";
    save_audio(path, harmonic(audio, config, stft));
    if (!args.quiet) {
      std::cerr << color::green << "Saved harmonic to " << path << color::reset << "\n";
    }
    if (args.json_output) JsonBuilder().begin_object().kv("harmonic", path).end_object().print();
  } else if (args.has("percussive-only")) {
    std::string path = base + ".wav";
    save_audio(path, percussive(audio, config, stft));
    if (!args.quiet) {
      std::cerr << color::green << "Saved percussive to " << path << color::reset << "\n";
    }
    if (args.json_output) JsonBuilder().begin_object().kv("percussive", path).end_object().print();
  } else if (args.has("with-residual")) {
    auto r = hpss_with_residual(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav",
                res = base + "_residual.wav";
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    save_audio(res, r.residual);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << ", " << res << color::reset
                << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("harmonic", h)
          .kv("percussive", p)
          .kv("residual", res)
          .end_object()
          .print();
  } else {
    auto r = hpss(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav";
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder().begin_object().kv("harmonic", h).kv("percussive", p).end_object().print();
  }
  return 0;
}

int cmd_preemphasis(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: preemphasis requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const float coef = args.get_float("coef", 0.97f);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = preemphasis(input, coef);
  save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("coef", coef)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_deemphasis(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: deemphasis requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const float coef = args.get_float("coef", 0.97f);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = deemphasis(input, coef);
  save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("coef", coef)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_trim_silence(const CliArgs& args, const Audio& audio) {
  // Match Python's public trim() command: an absolute dB threshold. --top-db
  // remains a native compatibility alias for the legacy relative algorithm.
  if (args.has("threshold-db") && args.has("top-db")) {
    throw std::invalid_argument("--threshold-db and --top-db are mutually exclusive");
  }
  const bool use_legacy_top_db = args.has("top-db") && !args.has("threshold-db");
  const float threshold_db = args.get_float("threshold-db", -60.0f);
  Audio result =
      use_legacy_top_db
          ? Audio::from_vector(
                sonare::trim(std::vector<float>(audio.begin(), audio.end()),
                             args.get_float("top-db", 60.0f), args.n_fft, args.hop_length)
                    .audio,
                audio.sample_rate())
          : trim_absolute(audio, threshold_db, args.n_fft, args.hop_length);

  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("length", result.size()).kv("sample_rate", audio.sample_rate());
    // Report the parameter that actually drove the trim: the relative top_db for
    // the legacy algorithm, the absolute threshold_db otherwise.
    if (use_legacy_top_db) {
      json.kv("top_db", args.get_float("top-db", 60.0f));
    } else {
      json.kv("threshold_db", threshold_db);
    }
    if (!args.output_file.empty()) json.kv("output", args.output_file);
    json.end_object().print();
  } else {
    std::cout << "Silence Trim:\n";
    printf("  Samples: %zu\n", result.size());
    if (!args.output_file.empty()) std::cout << "  Output:  " << args.output_file << "\n";
  }
  return 0;
}

int cmd_split_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", 60.0f);
  std::vector<float> input(audio.begin(), audio.end());
  auto ranges = sonare::split(input, top_db, args.n_fft, args.hop_length);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_array();
    for (const auto& range : ranges) {
      json.begin_object()
          .kv("start_sample", range.first)
          .kv("end_sample", range.second)
          .end_object();
    }
    json.end_array().print();
  } else {
    std::cout << "Non-silent intervals: " << ranges.size() << "\n";
    for (const auto& range : ranges) {
      printf("  %d - %d\n", range.first, range.second);
    }
  }
  return 0;
}
