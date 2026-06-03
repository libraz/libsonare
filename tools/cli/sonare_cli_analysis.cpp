#include "sonare_cli.h"

int cmd_bpm(const CliArgs& args, const Audio& audio) {
  float bpm = quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().begin_object().kv("bpm", bpm).end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << bpm << " BPM" << color::reset << "\n\n";
  }
  return 0;
}

int cmd_key(const CliArgs& args, const Audio& audio) {
  KeyConfig config;
  config.n_fft = args.n_fft == 2048 ? 4096 : args.n_fft;
  config.hop_length = args.hop_length;
  config.use_hpss = args.has("use-hpss") || args.has("hpss");
  config.loudness_weighted = args.has("loudness-weighted");
  config.high_pass_hz = args.get_float("high-pass-hz", 0.0f);
  if (args.has("genre-hint")) {
    config.genre_hint = args.get_string("genre-hint");
  }
  if (args.has("profile")) {
    config.profile_type = parse_key_profile_option(args.get_string("profile"));
  }
  if (args.has("modes")) {
    config.modes = parse_mode_list_option(args.get_string("modes"));
  }

  auto candidates =
      quick::detect_key_candidates(audio.data(), audio.size(), audio.sample_rate(), config);
  Key key = candidates.empty() ? quick::detect_key(audio.data(), audio.size(), audio.sample_rate())
                               : candidates.front().key;
  int candidate_count = 0;
  if (args.has("candidates")) {
    const std::string value = args.get_string("candidates");
    candidate_count = (value == "true") ? 5 : std::max(0, std::stoi(value));
    candidate_count = std::min(candidate_count, static_cast<int>(candidates.size()));
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("root", static_cast<int>(key.root))
        .kv("mode", static_cast<int>(key.mode))
        .kv("confidence", key.confidence)
        .kv("name", key.to_string());
    if (candidate_count > 0) {
      json.key("candidates").begin_array();
      for (int i = 0; i < candidate_count; ++i) {
        const auto& candidate = candidates[static_cast<size_t>(i)];
        json.begin_object()
            .kv("root", static_cast<int>(candidate.key.root))
            .kv("mode", static_cast<int>(candidate.key.mode))
            .kv("confidence", candidate.key.confidence)
            .kv("name", candidate.key.to_string())
            .kv("correlation", candidate.correlation)
            .end_object();
      }
      json.end_array();
    }
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::magenta << color::bold << "> Estimated Key : " << key.to_string()
              << "  (conf " << std::fixed << std::setprecision(1) << (key.confidence * 100.0f)
              << "%)" << color::reset << "\n\n";
    if (candidate_count > 0) {
      std::cout << "  " << color::blue << "> Key candidates:" << color::reset << "\n";
      for (int i = 0; i < candidate_count; ++i) {
        const auto& candidate = candidates[static_cast<size_t>(i)];
        std::cout << "    " << std::setw(2) << (i + 1) << ". " << candidate.key.to_string()
                  << "  corr " << std::fixed << std::setprecision(3) << candidate.correlation
                  << "  conf " << std::setprecision(1) << (candidate.key.confidence * 100.0f)
                  << "%\n";
      }
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_beats(const CliArgs& args, const Audio& audio) {
  auto beats = quick::detect_beats(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(beats).print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << beats.size() << " beats" << color::reset
              << "\n";
    std::cout << "  " << color::blue << "> Beat times:" << color::reset << "\n    ";
    for (size_t i = 0; i < beats.size(); ++i) {
      printf("%.2f", beats[i]);
      if (i < beats.size() - 1) std::cout << ", ";
      if ((i + 1) % 10 == 0) std::cout << "\n    ";
    }
    std::cout << "\n\n";
  }
  return 0;
}

int cmd_downbeats(const CliArgs& args, const Audio& audio) {
  auto downbeats = quick::detect_downbeats(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(downbeats).print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << downbeats.size() << " downbeats"
              << color::reset << "\n";
    std::cout << "  " << color::blue << "> Downbeat times:" << color::reset << "\n    ";
    for (size_t i = 0; i < downbeats.size(); ++i) {
      printf("%.2f", downbeats[i]);
      if (i < downbeats.size() - 1) std::cout << ", ";
      if ((i + 1) % 10 == 0) std::cout << "\n    ";
    }
    std::cout << "\n\n";
  }
  return 0;
}

int cmd_onsets(const CliArgs& args, const Audio& audio) {
  auto onsets = quick::detect_onsets(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(onsets).print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << onsets.size() << " onsets" << color::reset
              << "\n";
    std::cout << "  " << color::blue << "> Onset times:" << color::reset << "\n    ";
    for (size_t i = 0; i < onsets.size(); ++i) {
      printf("%.2f", onsets[i]);
      if (i < onsets.size() - 1) std::cout << ", ";
      if ((i + 1) % 10 == 0) std::cout << "\n    ";
    }
    std::cout << "\n\n";
  }
  return 0;
}

int cmd_chords(const CliArgs& args, const Audio& audio) {
  ChordConfig config;
  config.min_duration = args.get_float("min-duration", 0.3f);
  config.threshold = args.get_float("threshold", 0.5f);
  config.use_triads_only = args.has("triads-only");
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.chroma_method = args.has("nnls") ? ChromaMethod::NNLS : ChromaMethod::STFT;
  config.use_hmm = args.has("use-hmm");
  config.hmm_beam_width = args.get_int("hmm-beam-width", config.hmm_beam_width);
  config.detect_inversions = args.has("detect-inversions");
  config.use_key_context = args.has("key-context");
  if (config.use_key_context) {
    config.key_root = parse_pitch_class_option(args.get_string("key-root", "C"));
    config.key_mode = parse_mode_option(args.get_string("key-mode", "major"));
  }

  ChordAnalyzer analyzer(audio, config);
  const auto& chords = analyzer.chords();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("progression", analyzer.progression_pattern())
        .kv("count", chords.size())
        .key("chords")
        .begin_array();
    for (const auto& c : chords) {
      json.begin_object()
          .kv("name", c.to_string())
          .kv("root", static_cast<int>(c.root))
          .kv("bass", static_cast<int>(c.bass))
          .kv("start", c.start)
          .kv("end", c.end)
          .kv("confidence", c.confidence)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s, " << chords.size() << " chord changes" << color::reset
              << "\n";
    std::cout << "  " << color::blue << "> Chord Progression: " << analyzer.progression_pattern()
              << color::reset << "\n";
    std::cout << "  " << color::blue << "> Chord Details:" << color::reset << "\n";
    for (size_t i = 0; i < chords.size(); ++i) {
      const auto& c = chords[i];
      printf("    %zu. %-8s (%.2fs - %.2fs, conf %.0f%%)\n", i + 1, c.to_string().c_str(), c.start,
             c.end, c.confidence * 100.0f);
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_sections(const CliArgs& args, const Audio& audio) {
  SectionConfig config;
  config.min_section_sec = args.get_float("min-duration", 4.0f);
  config.boundary_threshold = args.get_float("threshold", 0.3f);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  SectionAnalyzer analyzer(audio, config);
  const auto& sections = analyzer.sections();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("form", analyzer.form())
        .kv("count", sections.size())
        .key("sections")
        .begin_array();
    for (const auto& s : sections) {
      json.begin_object()
          .kv("type", s.type_string())
          .kv("start", s.start)
          .kv("end", s.end)
          .kv("energy", s.energy_level)
          .kv("confidence", s.confidence)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Structure: " << analyzer.form() << " ("
              << sections.size() << " sections)" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Section Details:" << color::reset << "\n";
    for (size_t i = 0; i < sections.size(); ++i) {
      const auto& s = sections[i];
      int start_mm = static_cast<int>(s.start) / 60;
      int start_ss = static_cast<int>(s.start) % 60;
      float duration = s.end - s.start;
      std::string energy_sym = (s.energy_level < 0.33f)   ? "low E"
                               : (s.energy_level < 0.67f) ? "mid E"
                                                          : "high E";
      printf("    %zu. %s (%02d:%02d, %.1fs, %s)\n", i + 1, s.type_string().c_str(), start_mm,
             start_ss, duration, energy_sym.c_str());
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_timbre(const CliArgs& args, const Audio& audio) {
  TimbreConfig config;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.n_mels = args.n_mels;

  TimbreAnalyzer analyzer(audio, config);
  const Timbre& t = analyzer.timbre();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("brightness", t.brightness)
        .kv("warmth", t.warmth)
        .kv("density", t.density)
        .kv("roughness", t.roughness)
        .kv("complexity", t.complexity)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::magenta << "> Timbre Analysis:" << color::reset << "\n";
    printf("    Brightness: %.2f (%s)\n", t.brightness,
           describe_level(t.brightness, "dark", "neutral", "bright").c_str());
    printf("    Warmth:     %.2f (%s)\n", t.warmth,
           describe_level(t.warmth, "thin", "neutral", "warm").c_str());
    printf("    Density:    %.2f (%s)\n", t.density,
           describe_level(t.density, "sparse", "moderate", "rich").c_str());
    printf("    Roughness:  %.2f (%s)\n", t.roughness,
           describe_level(t.roughness, "smooth", "moderate", "rough").c_str());
    printf("    Complexity: %.2f (%s)\n", t.complexity,
           describe_level(t.complexity, "simple", "moderate", "complex").c_str());
    std::cout << "\n";
  }
  return 0;
}

int cmd_dynamics(const CliArgs& args, const Audio& audio) {
  DynamicsConfig config;
  config.window_sec = args.get_float("window-sec", 0.4f);
  config.hop_length = args.hop_length;

  DynamicsAnalyzer analyzer(audio, config);
  const Dynamics& d = analyzer.dynamics();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("peak_db", d.peak_db)
        .kv("rms_db", d.rms_db)
        .kv("dynamic_range_db", d.dynamic_range_db)
        .kv("crest_factor", d.crest_factor)
        .kv("loudness_range_db", d.loudness_range_db)
        .kv("is_compressed", d.is_compressed)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Dynamics Analysis:" << color::reset << "\n";
    printf("    Peak Level:     %.1f dB\n", d.peak_db);
    printf("    RMS Level:      %.1f dB\n", d.rms_db);
    printf("    Dynamic Range:  %.1f dB\n", d.dynamic_range_db);
    printf("    Crest Factor:   %.1f dB\n", d.crest_factor);
    printf("    Loudness Range: %.1f LU\n", d.loudness_range_db);
    std::cout << "    Compression:    " << (d.is_compressed ? "Yes (compressed)" : "No (natural)")
              << "\n\n";
  }
  return 0;
}

int cmd_rhythm(const CliArgs& args, const Audio& audio) {
  RhythmConfig config;
  config.start_bpm = args.get_float("start-bpm", 120.0f);
  config.bpm_min = args.get_float("bpm-min", 60.0f);
  config.bpm_max = args.get_float("bpm-max", 200.0f);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  RhythmAnalyzer analyzer(audio, config);
  const RhythmFeatures& r = analyzer.features();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .key("time_signature")
        .begin_object()
        .kv("numerator", r.time_signature.numerator)
        .kv("denominator", r.time_signature.denominator)
        .kv("confidence", r.time_signature.confidence)
        .end_object()
        .kv("bpm", analyzer.bpm())
        .kv("groove_type", r.groove_type)
        .kv("syncopation", r.syncopation)
        .kv("pattern_regularity", r.pattern_regularity)
        .kv("tempo_stability", r.tempo_stability)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Rhythm Analysis:" << color::reset << "\n";
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << analyzer.bpm() << " BPM" << color::reset << "\n";
    printf("    Time Signature:    %d/%d (conf %.0f%%)\n", r.time_signature.numerator,
           r.time_signature.denominator, r.time_signature.confidence * 100.0f);
    printf("    Groove Type:       %s\n", r.groove_type.c_str());
    printf("    Syncopation:       %.2f (%s)\n", r.syncopation,
           r.syncopation < 0.3f ? "low" : (r.syncopation < 0.7f ? "moderate" : "high"));
    printf("    Pattern Regularity: %.2f (%s)\n", r.pattern_regularity,
           r.pattern_regularity > 0.7f ? "regular" : "irregular");
    printf("    Tempo Stability:   %.2f (%s)\n", r.tempo_stability,
           r.tempo_stability > 0.8f ? "stable" : "variable");
    std::cout << "\n";
  }
  return 0;
}

int cmd_melody(const CliArgs& args, const Audio& audio) {
  MelodyConfig config;
  config.fmin = args.get_float("fmin", 80.0f);
  config.fmax = args.get_float("fmax", 1000.0f);
  config.threshold = args.get_float("threshold", 0.1f);
  config.hop_length = args.hop_length;

  MelodyAnalyzer analyzer(audio, config);
  const MelodyContour& c = analyzer.contour();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("has_melody", analyzer.has_melody())
        .kv("pitch_range_octaves", c.pitch_range_octaves)
        .kv("mean_frequency", c.mean_frequency)
        .kv("pitch_stability", c.pitch_stability)
        .kv("vibrato_rate", c.vibrato_rate)
        .kv("pitch_count", c.pitches.size())
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::cyan << "> Melody Analysis:" << color::reset << "\n";
    std::cout << "    Has Melody:      " << (analyzer.has_melody() ? "Yes" : "No") << "\n";
    printf("    Pitch Range:     %.2f octaves\n", c.pitch_range_octaves);
    printf("    Mean Frequency:  %.1f Hz\n", c.mean_frequency);
    printf("    Pitch Stability: %.2f\n", c.pitch_stability);
    printf("    Vibrato Rate:    %.1f Hz\n", c.vibrato_rate);
    printf("    Pitch Points:    %zu\n", c.pitches.size());
    std::cout << "\n";
  }
  return 0;
}

int cmd_boundaries(const CliArgs& args, const Audio& audio) {
  BoundaryConfig config;
  config.threshold = args.get_float("threshold", 0.3f);
  config.kernel_size = args.get_int("kernel-size", 64);
  config.peak_distance = args.get_float("min-distance", 2.0f);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  BoundaryDetector detector(audio, config);
  const auto& bounds = detector.boundaries();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("count", bounds.size()).key("boundaries").begin_array();
    for (const auto& b : bounds) {
      json.begin_object()
          .kv("time", b.time)
          .kv("frame", b.frame)
          .kv("strength", b.strength)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Structural Boundaries (" << bounds.size()
              << " detected):" << color::reset << "\n";
    for (size_t i = 0; i < bounds.size(); ++i) {
      const auto& b = bounds[i];
      int mm = static_cast<int>(b.time) / 60;
      int ss = static_cast<int>(b.time) % 60;
      printf("    %zu. %02d:%02d (strength %.2f)\n", i + 1, mm, ss, b.strength);
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_analyze(const CliArgs& args, const Audio& audio) {
  MusicAnalyzerConfig config;
  config.use_triads_only = !args.has("with-seventh");
  config.use_hpss = !args.has("no-hpss");
  if (args.has("chroma-highpass")) {
    config.chroma_highpass_hz = args.get_float("chroma-highpass", 200.0f);
  }

  if (!args.quiet && !args.json_output) {
    int workers = system_info::parallel_workers();
    if (system_info::parallel_enabled()) {
      std::cerr << color::green << "Parallel analysis: " << workers << " workers ("
                << system_info::parallel_strategy() << ")" << color::reset << "\n";
    }
  }

  MusicAnalyzer analyzer(audio, config);
  if (!args.quiet && !args.json_output) {
    analyzer.set_progress_callback(progress_callback);
  }

  AnalysisResult r = analyzer.analyze();

  if (!args.quiet && !args.json_output) {
    clear_progress();
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("bpm", r.bpm)
        .kv("bpmConfidence", r.bpm_confidence)
        .key("key")
        .begin_object()
        .kv("root", static_cast<int>(r.key.root))
        .kv("mode", static_cast<int>(r.key.mode))
        .kv("confidence", r.key.confidence)
        .kv("name", r.key.to_string())
        .end_object()
        .key("timeSignature")
        .begin_object()
        .kv("numerator", r.time_signature.numerator)
        .kv("denominator", r.time_signature.denominator)
        .kv("confidence", r.time_signature.confidence)
        .end_object()
        .key("beats")
        .begin_array();
    for (const auto& b : r.beats) {
      json.begin_object().kv("time", b.time).kv("strength", b.strength).end_object();
    }
    json.end_array().key("chords").begin_array();
    for (const auto& c : r.chords) {
      json.begin_object()
          .kv("name", c.to_string())
          .kv("start", c.start)
          .kv("end", c.end)
          .kv("confidence", c.confidence)
          .end_object();
    }
    json.end_array().key("sections").begin_array();
    for (const auto& s : r.sections) {
      json.begin_object()
          .kv("type", s.type_string())
          .kv("start", s.start)
          .kv("end", s.end)
          .end_object();
    }
    json.end_array()
        .key("timbre")
        .begin_object()
        .kv("brightness", r.timbre.brightness)
        .kv("warmth", r.timbre.warmth)
        .kv("density", r.timbre.density)
        .kv("roughness", r.timbre.roughness)
        .kv("complexity", r.timbre.complexity)
        .end_object()
        .key("dynamics")
        .begin_object()
        .kv("dynamicRangeDb", r.dynamics.dynamic_range_db)
        .kv("loudnessRangeDb", r.dynamics.loudness_range_db)
        .kv("crestFactor", r.dynamics.crest_factor)
        .kv("isCompressed", r.dynamics.is_compressed)
        .end_object()
        .key("rhythm")
        .begin_object()
        .kv("syncopation", r.rhythm.syncopation)
        .kv("grooveType", r.rhythm.groove_type)
        .kv("patternRegularity", r.rhythm.pattern_regularity)
        .end_object()
        .kv("form", r.form)
        .end_object()
        .print();
  } else {
    // bpm-detector style output
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";

    // Summary line
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s, BPM: " << r.bpm << ", Key: " << r.key.to_string()
              << color::reset << "\n";

    // Chord progression (if available)
    if (!r.chords.empty()) {
      std::cout << "  " << color::blue << "> Chord Progression: ";
      size_t max_chords = std::min(r.chords.size(), static_cast<size_t>(4));
      for (size_t i = 0; i < max_chords; ++i) {
        if (i > 0) std::cout << " -> ";
        std::cout << r.chords[i].to_string();
      }
      std::cout << color::reset << "\n";
    }

    // Structure
    if (!r.sections.empty()) {
      std::cout << "  " << color::blue << "> Structure: " << r.form << " (" << r.sections.size()
                << " sections)" << color::reset << "\n";

      // Section details
      std::cout << "  " << color::blue << "> Section Details (" << r.sections.size()
                << " sections):" << color::reset << "\n";

      for (size_t i = 0; i < r.sections.size(); ++i) {
        const auto& s = r.sections[i];
        int start_mm = static_cast<int>(s.start) / 60;
        int start_ss = static_cast<int>(s.start) % 60;
        float duration = s.end - s.start;
        int bars = static_cast<int>(std::round(duration / (4.0f * 60.0f / r.bpm)));

        std::string energy_sym = (s.energy_level < 0.33f)   ? "low E"
                                 : (s.energy_level < 0.67f) ? "mid E"
                                                            : "high E";

        printf("    %zu. %s (%02d:%02d, %dbars, %s)\n", i + 1, s.type_string().c_str(), start_mm,
               start_ss, bars, energy_sym.c_str());
      }
    }

    // Rhythm
    std::cout << "  " << color::blue << "> Rhythm: " << r.time_signature.numerator << "/"
              << r.time_signature.denominator << " time, " << r.rhythm.groove_type << " groove"
              << color::reset << "\n";

    // Timbre
    std::cout << "  " << color::magenta << "> Timbre: Brightness " << std::fixed
              << std::setprecision(1) << r.timbre.brightness << ", Warmth " << r.timbre.warmth
              << color::reset << "\n";

    // Dynamics
    std::cout << "  " << color::yellow << "> Dynamics: " << std::fixed << std::setprecision(1)
              << r.dynamics.dynamic_range_db << "dB range" << color::reset << "\n";

    // Final BPM and Key results
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << r.bpm << " BPM  (conf " << std::setprecision(1)
              << (r.bpm_confidence * 100.0f) << "%)" << color::reset << "\n";
    std::cout << "  " << color::magenta << color::bold << "> Estimated Key : " << r.key.to_string()
              << "  (conf " << std::fixed << std::setprecision(1) << (r.key.confidence * 100.0f)
              << "%)" << color::reset << "\n\n";
  }
  return 0;
}
