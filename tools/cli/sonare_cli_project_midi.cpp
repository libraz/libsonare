#include <cstdint>
#include <string>
#include <vector>

#include "sonare_cli_project.h"

#ifdef SONARE_WITH_ARRANGEMENT

// `transcribe in.wav -o out.mid` — audio to a Standard MIDI File. The notes
// land on a PROJECT's tempo map, which is why an explicit --tempo-bpm is
// installed as that map rather than handed to the transcriber: the clip entry
// takes no tempo at all. Omitting it detects one from the take.
int cmd_transcribe(const CliArgs& args, const Audio& audio) {
  const bool tempo_given = args.has("tempo-bpm");
  if (tempo_given && !(args.get_float("tempo-bpm", 0.0f) > 0.0f)) {
    throw std::invalid_argument("--tempo-bpm must be greater than 0");
  }

  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return project_exit_code(err);
  }

  float tempo_bpm = 0.0f;
  if (tempo_given) {
    tempo_bpm = args.get_float("tempo-bpm", 0.0f);
    SonareProjectTempoSegment segment{};
    segment.start_ppq = 0.0;
    segment.bpm = static_cast<double>(tempo_bpm);
    err = sonare_project_set_tempo_segments(handle.ptr, &segment, 1);
    if (err != SONARE_OK) {
      project_report_error("set tempo", err);
      return project_exit_code(err);
    }
  } else {
    const SonareProjectTempoOptions options = sonare_project_tempo_options_default();
    err = sonare_project_auto_tempo_with_options(handle.ptr, audio.data(), audio.size(),
                                                 audio.sample_rate(), &options, 0, 0, &tempo_bpm);
    if (err != SONARE_OK) {
      project_report_error("detect tempo", err);
      return project_exit_code(err);
    }
  }

  // PPQ coordinates are beats, so the take's length in beats is what the clip
  // has to span for its last note-off to fall inside it.
  const double duration =
      audio.sample_rate() > 0 ? static_cast<double>(audio.size()) / audio.sample_rate() : 0.0;
  const double length_ppq = std::max(1.0, std::ceil(duration * tempo_bpm / 60.0));
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  err = sonare_project_add_midi_clip(handle.ptr, 0.0, length_ppq, &track_id, &clip_id);
  if (err != SONARE_OK) {
    project_report_error("add MIDI clip", err);
    return project_exit_code(err);
  }

  // 0 is the C ABI's "keep the library default" for every field here, which is
  // what an unset option means, so an absent option is left at 0 rather than
  // given a value this CLI would have to keep in step with the core's.
  SonareTranscribeConfig config{};
  config.struct_version = 1;
  config.polyphonic = args.has("polyphonic") ? 1 : 0;
  config.reference_hz = args.get_float("reference-hz", 0.0f);
  config.fmin = args.get_float("fmin", 0.0f);
  config.fmax = args.get_float("fmax", 0.0f);
  config.min_note_ms = args.get_float("min-note-ms", 0.0f);
  config.segmentation_threshold_cents = args.get_float("segmentation-threshold-cents", 0.0f);
  config.velocity_floor_db = args.get_float("velocity-floor-db", 0.0f);
  config.fixed_velocity = args.get_int("fixed-velocity", 0);
  config.group = args.get_int("group", 0);
  config.channel = args.get_int("channel", 0);

  size_t note_count = 0;
  err = sonare_project_transcribe_to_clip(handle.ptr, clip_id, audio.data(), audio.size(),
                                          audio.sample_rate(), &config, &note_count);
  if (err != SONARE_OK) {
    project_report_error("transcribe", err);
    return project_exit_code(err);
  }

  uint8_t* bytes = nullptr;
  size_t len = 0;
  err = sonare_project_export_smf(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export SMF", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("note_count", note_count)
        .kv("tempo_bpm", tempo_bpm)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Transcribed " << note_count << " notes at " << std::fixed
              << std::setprecision(2) << tempo_bpm << std::defaultfloat << " BPM to "
              << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project export-smf --in in.json -o out.mid` — export the project's tempo map
// + MIDI clips to a Standard MIDI File.
int cmd_project_export_smf(const CliArgs& args) {
  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_export_smf(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export SMF", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Exported SMF (" << len << " bytes) to " << args.output_file
              << color::reset << "\n";
  }
  return 0;
}

// `project export-midi2 --in in.json -o out.midi2` — export the project's tempo
// map + MIDI clips to a MIDI 2.0 Clip File.
int cmd_project_export_midi2(const CliArgs& args) {
  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_export_clip_file(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export MIDI2 Clip File", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Exported MIDI2 Clip File (" << len << " bytes) to "
              << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project import-smf --smf in.mid -o out.json` — import an SMF into a new
// project and serialize it to JSON.
int cmd_project_import_smf(const CliArgs& args) {
  const std::string smf_path = args.get_string("smf");
  if (smf_path.empty()) {
    std::cerr << color::red << "Error: missing SMF input (use --smf <file.mid>)" << color::reset
              << "\n";
    return 1;
  }
  std::vector<uint8_t> smf;
  if (!read_binary_file(smf_path, &smf)) {
    std::cerr << color::red << "Error: cannot open SMF file: " << smf_path << color::reset << "\n";
    // A user-named input file that cannot be opened keeps the file-not-found
    // class here for the same reason load_project_from_args does: a caller that
    // branches on "fetch the input again" versus "the arguments are wrong" must
    // get the same answer from whichever subcommand read the file.
    return project_exit_code(SONARE_ERROR_FILE_NOT_FOUND);
  }
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return project_exit_code(err);
  }
  uint32_t first_clip = 0;
  err = sonare_project_import_smf(handle.ptr, smf.data(), smf.size(), &first_clip);
  if (err != SONARE_OK) {
    project_report_error("import SMF", err);
    return project_exit_code(err);
  }
  char* json = nullptr;
  size_t len = 0;
  err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
  sonare_free_string(json);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("first_clip_id", static_cast<int>(first_clip))
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Imported SMF to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project import-midi2 --midi2 in.midi2 -o out.json` — import a MIDI 2.0 Clip
// File into a new project and serialize it to JSON.
int cmd_project_import_midi2(const CliArgs& args) {
  const std::string midi2_path = args.get_string("midi2");
  if (midi2_path.empty()) {
    std::cerr << color::red << "Error: missing MIDI2 input (use --midi2 <file.midi2>)"
              << color::reset << "\n";
    return 1;
  }
  std::vector<uint8_t> midi2;
  if (!read_binary_file(midi2_path, &midi2)) {
    std::cerr << color::red << "Error: cannot open MIDI2 file: " << midi2_path << color::reset
              << "\n";
    return project_exit_code(SONARE_ERROR_FILE_NOT_FOUND);
  }
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return project_exit_code(err);
  }
  uint32_t first_clip = 0;
  err = sonare_project_import_clip_file(handle.ptr, midi2.data(), midi2.size(), &first_clip);
  if (err != SONARE_OK) {
    project_report_error("import MIDI2 Clip File", err);
    return project_exit_code(err);
  }
  char* json = nullptr;
  size_t len = 0;
  err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
  sonare_free_string(json);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("first_clip_id", static_cast<int>(first_clip))
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Imported MIDI2 Clip File to " << args.output_file << color::reset
              << "\n";
  }
  return 0;
}
#endif  // SONARE_WITH_ARRANGEMENT
