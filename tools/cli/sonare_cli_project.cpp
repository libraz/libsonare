#include "sonare_cli.h"

#ifdef SONARE_WITH_ARRANGEMENT

#include <atomic>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// Every subcommand here reports its result on stdout and keeps stderr for
// errors, usage, and advisory output, matching the rest of this CLI. The
// deciding reason is that `--json` already prints the result to stdout: the
// text branch renders the same result the JSON branch emits, so sending it to
// stderr would split one result across two streams depending only on a
// formatting flag.
//
// Diagnostics split on the same principle: one that changes the exit status is
// part of the result and goes to stdout; one that does not is advisory and goes
// to stderr. That is why `--strict` prints its diagnostics to stdout in place
// of the success line, while a plain run prints the success line to stdout and
// the same diagnostics to stderr.
//
// Usage exit code, mirroring kExitUsage in tools/sonare_cli.cpp. A missing or
// blank `project` subcommand is a usage error, which is a different class from
// the invalid-parameter code a plain `1` carries.
constexpr int kExitUsage = 2;

// Invalid-parameter exit code, mirroring kExitInvalidParameter in
// tools/sonare_cli.cpp. This is what a plain `1` from any handler normalizes
// to, so a branch only spells it out when it is returning the code alongside
// others (see project_exit_code).
constexpr int kExitInvalidParameter = 3;

// Invalid-state exit code used when `project validate --strict` finds loader
// diagnostics after still writing the canonical artifact and JSON payload, and
// when the C ABI itself reports an invalid state.
constexpr int kExitInvalidState = 9;

// Upper bound on a project JSON / SMF / MIDI 2.0 file loaded into memory, mirrored
// from the Python CLI's _MAX_PROJECT_OR_MIDI_BYTES. Bounds the allocation so an
// oversized (or hostile) input is rejected instead of exhausting memory.
constexpr size_t kMaxProjectOrMidiBytes = 64ull * 1024ull * 1024ull;

struct ProjectHandle {
  SonareProject* ptr = nullptr;
  ~ProjectHandle() { sonare_project_destroy(ptr); }
  ProjectHandle() = default;
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

std::string project_error_string(SonareError err) {
  const char* msg = sonare_error_message(err);
  return msg != nullptr ? std::string(msg) : ("error " + std::to_string(static_cast<int>(err)));
}

void project_report_error(const std::string& what, SonareError err) {
  std::cerr << color::red << "Error: " << what << ": " << project_error_string(err) << color::reset
            << "\n";
}

// Maps a C-ABI error onto the published exit-code contract. A project handler
// that has the error in hand returns this instead of a plain 1: the failure
// keeps the class it actually carries (a file that does not exist stays a
// file-not-found, a rejected preset stays an invalid parameter) all the way out
// to the caller, which is what makes the two CLIs agree per condition.
int project_exit_code(SonareError err) {
  switch (err) {
    case SONARE_ERROR_FILE_NOT_FOUND:
      return cli_exit_code_for_error(sonare::ErrorCode::FileNotFound, false);
    case SONARE_ERROR_INVALID_FORMAT:
      return cli_exit_code_for_error(sonare::ErrorCode::InvalidFormat, false);
    case SONARE_ERROR_DECODE_FAILED:
      return cli_exit_code_for_error(sonare::ErrorCode::DecodeFailed, false);
    case SONARE_ERROR_INVALID_PARAMETER:
      return cli_exit_code_for_error(sonare::ErrorCode::InvalidParameter, false);
    case SONARE_ERROR_OUT_OF_MEMORY:
      return cli_exit_code_for_error(sonare::ErrorCode::OutOfMemory, false);
    case SONARE_ERROR_NOT_SUPPORTED:
      return cli_exit_code_for_error(sonare::ErrorCode::NotImplemented, false);
    case SONARE_ERROR_INVALID_STATE:
      return kExitInvalidState;
    case SONARE_ERROR_CANCELLED:
      return cli_exit_code_for_error(sonare::ErrorCode::Cancelled, false);
    case SONARE_ERROR_ENCODE_FAILED:
      return cli_exit_code_for_error(sonare::ErrorCode::EncodeFailed, false);
    case SONARE_OK:
    case SONARE_ERROR_UNKNOWN:
    default:
      return kExitInvalidParameter;
  }
}

// The one diagnostic an oversized input reports, raised both by the size probe
// and by the read that enforces the same cap on the bytes actually delivered.
// Sharing it keeps those two rejections indistinguishable to a caller.
[[noreturn]] void reject_oversized_input(const std::string& path) {
  throw std::invalid_argument("input file exceeds " + std::to_string(kMaxProjectOrMidiBytes) +
                              " byte limit: " + path);
}

// Reads an arbitrary file into a byte buffer (binary-safe). The CLI owns file
// I/O; the core / C ABI exchange in-memory buffers only. An input larger than
// kMaxProjectOrMidiBytes is rejected with a clear diagnostic (mapped to the
// invalid-parameter exit code by main()).
//
// The seek/tell probe only sizes the allocation: it is a snapshot the read
// cannot rely on, because the file may grow between the two calls and a
// non-regular input (a FIFO, a character device) has no size to report at all.
// The cap is therefore enforced on the bytes as they arrive, so nothing past it
// is ever buffered.
bool read_binary_file(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size > static_cast<std::streamoff>(kMaxProjectOrMidiBytes)) reject_oversized_input(path);
  // An input with no size to probe fails the seek rather than reporting one, and
  // a failed stream reads nothing. Clearing on both sides of the rewind keeps
  // such an input readable from where it was opened, which is also the case the
  // bounded read below exists for.
  file.clear();
  file.seekg(0, std::ios::beg);
  file.clear();

  constexpr size_t kReadChunkBytes = 64u * 1024u;
  out->clear();
  if (size > 0) out->reserve(static_cast<size_t>(size));
  std::vector<char> chunk(kReadChunkBytes);
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto read_bytes = static_cast<size_t>(file.gcount());
    if (read_bytes == 0) break;
    // Subtraction rather than addition: the sum of two size_t operands can wrap
    // where the remaining budget cannot.
    if (read_bytes > kMaxProjectOrMidiBytes - out->size()) reject_oversized_input(path);
    out->insert(out->end(), chunk.data(), chunk.data() + read_bytes);
  }
  return true;
}

// Per-writer-unique sibling temp path for an atomic write. The process id plus a
// monotonic counter keep concurrent writers to the same destination on distinct
// temp files, so a fixed name cannot let two writers interleave into the same
// temp before either renames. Mirrors atomic_tmp_path() in
// src/core/audio_io.cpp (the CLI reaches the core only through the C ABI and
// cannot share that internal helper).
std::string project_atomic_tmp_path(const std::string& path) {
  static std::atomic<uint64_t> counter{0};
#ifdef _WIN32
  const unsigned long pid = ::GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
  return path + ".sonare-tmp." + std::to_string(pid) + "." + std::to_string(seq);
}

// Writes atomically: the payload lands in a sibling temp file that only replaces
// the destination once fully written, so an interrupted or failed export never
// truncates an existing project file.
bool write_binary_file(const std::string& path, const uint8_t* data, size_t len) {
  const std::string tmp = project_atomic_tmp_path(path);
  {
    std::ofstream file(tmp, std::ios::binary);
    if (!file.is_open()) return false;
    if (len > 0) file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    file.flush();
    if (!file.good()) {
      file.close();
      std::remove(tmp.c_str());
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

// Loads a project JSON file from --in into a fresh handle. Returns true on
// success. On failure prints an error and leaves the handle empty.
//
// `project` accepts one positional and the subcommand name consumes it, so
// there is no second positional to fall back to: the --project and
// args.input_file branches below are unreachable from every current caller,
// all of which declare --in as required. They are kept as a guard for a future
// subcommand that does not, which would otherwise read its own subcommand name
// as a file path.
bool load_project_from_args(const CliArgs& args, ProjectHandle* handle,
                            std::string* diagnostics = nullptr, SonareError* load_error = nullptr) {
  if (load_error != nullptr) *load_error = SONARE_OK;
  const std::string in_path =
      args.has("in") ? args.get_string("in") : args.get_string("project", args.input_file);
  if (in_path.empty()) {
    if (load_error != nullptr) *load_error = SONARE_ERROR_INVALID_PARAMETER;
    std::cerr << color::red << "Error: missing project JSON (use --in <project.json>)"
              << color::reset << "\n";
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_binary_file(in_path, &bytes)) {
    if (load_error != nullptr) *load_error = SONARE_ERROR_FILE_NOT_FOUND;
    std::cerr << color::red << "Error: cannot open project file: " << in_path << color::reset
              << "\n";
    return false;
  }
  char* diag = nullptr;
  SonareError err = sonare_project_deserialize(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size(), &handle->ptr, &diag);
  if (err != SONARE_OK) {
    if (load_error != nullptr) *load_error = err;
    std::cerr << color::red << "Error: failed to parse project JSON: " << project_error_string(err);
    if (diag != nullptr) std::cerr << " (" << diag << ")";
    std::cerr << color::reset << "\n";
    sonare_free_string(diag);
    return false;
  }
  if (diagnostics != nullptr && diag != nullptr) *diagnostics = diag;
  sonare_free_string(diag);
  return true;
}

size_t project_diagnostic_count(const std::string& diagnostics) {
  if (diagnostics.empty()) return 0;
  return 1 + static_cast<size_t>(std::count(diagnostics.begin(), diagnostics.end(), '\n'));
}

std::vector<std::string> project_compile_messages(const char* messages) {
  std::vector<std::string> lines;
  if (messages == nullptr || messages[0] == '\0') return lines;
  std::istringstream stream(messages);
  std::string line;
  while (std::getline(stream, line)) lines.push_back(line);
  return lines;
}

void print_project_validation_json(bool valid, size_t bytes, const std::string& diagnostics) {
  JsonBuilder json;
  json.begin_object()
      .kv("valid", valid)
      .kv("bytes", bytes)
      .kv("diagnostic_count", project_diagnostic_count(diagnostics));
  json.key("diagnostics").begin_array();
  std::istringstream stream(diagnostics);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) json.value(line);
  }
  json.end_array().end_object().print();
}

// `project abi` — print the runtime project ABI version (0 when the arrangement
// subsystem was compiled out).
int cmd_project_abi(const CliArgs& args) {
  const uint32_t version = sonare_project_abi_version();
  if (args.json_output) {
    JsonBuilder().begin_object().kv("abi_version", static_cast<int>(version)).end_object().print();
  } else {
    std::cout << version << "\n";
  }
  return 0;
}

// `project synth-presets` — expose the full NativeSynth catalog accepted by
// `project bounce --synth`, rather than documenting a small waveform subset.
int cmd_project_synth_presets(const CliArgs& args) {
  const char* joined = sonare_synth_preset_names();
  const std::vector<std::string> names = joined != nullptr && joined[0] != '\0'
                                             ? split_string(joined, '\n')
                                             : std::vector<std::string>{};
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

// `project new -o out.json` — create an empty project and serialize it to disk.
// The registry declares `-o` required for this leaf, so the missing-output case
// is refused before dispatch and is not re-checked here.
int cmd_project_new(const CliArgs& args) {
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return project_exit_code(err);
  }
  const double sample_rate = args.get_float("sample-rate", 0.0f);
  if (sample_rate > 0.0) {
    err = sonare_project_set_sample_rate(handle.ptr, sample_rate);
    if (err != SONARE_OK) {
      project_report_error("set sample rate", err);
      return project_exit_code(err);
    }
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
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Wrote empty project to " << args.output_file << color::reset
              << "\n";
  }
  return 0;
}

// `project validate --in in.json` — round-trip a project JSON through the
// deserializer + serializer; with -o, writes the canonical JSON back out.
int cmd_project_validate(const CliArgs& args) {
  ProjectHandle handle;
  std::string diagnostics;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, &diagnostics, &load_error)) {
    // A syntactically malformed project is a format failure, not a generic
    // project state failure. Keep stdout empty so machine callers can branch
    // on the exit code without having to parse an error payload.
    if (load_error == SONARE_ERROR_INVALID_FORMAT) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "failed to parse project JSON");
    }
    return project_exit_code(load_error);
  }
  // A successful parse is valid even when the loader emitted repair/warning
  // diagnostics, and that is what the JSON payload reports. `--strict` promotes
  // those diagnostics to a failing exit status, so the text output has to
  // describe the outcome the caller will branch on: printing the green "valid"
  // line while returning a failure left the user no way to see what was wrong
  // except to re-run with --json.
  const bool valid = true;
  const bool strict_failure = args.has("strict") && !diagnostics.empty();
  char* json = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return project_exit_code(err);
  }
  if (!args.output_file.empty()) {
    const bool ok =
        write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
    if (!ok) {
      sonare_free_string(json);
      std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
      return 1;
    }
  }
  sonare_free_string(json);
  if (args.json_output) {
    print_project_validation_json(valid, len, diagnostics);
  } else if (strict_failure && !args.quiet) {
    std::cout << color::yellow << "Project JSON loaded with "
              << project_diagnostic_count(diagnostics) << " diagnostic(s):\n"
              << diagnostics << color::reset << "\n";
  } else if (!args.quiet) {
    std::cout << color::green << "Project JSON is valid (" << len << " bytes canonical)"
              << color::reset << "\n";
    // Advisory here rather than part of the result: without --strict these do
    // not change the exit status, so they go to stderr and leave stdout as the
    // result alone. Suppressing them entirely hid a repaired dangling reference
    // from anyone who did not also ask for --json.
    std::istringstream diagnostic_lines(diagnostics);
    std::string line;
    while (std::getline(diagnostic_lines, line)) {
      if (!line.empty()) {
        std::cerr << color::yellow << "warning: " << line << color::reset << "\n";
      }
    }
  }
  if (strict_failure) return kExitInvalidState;
  return 0;
}

// `project compile --in in.json` — compile the project into a renderable
// timeline and surface diagnostics.
int cmd_project_compile(const CliArgs& args) {
  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);
  SonareProjectCompileResult result{};
  SonareError err = sonare_project_compile(handle.ptr, &result);
  if (err != SONARE_OK) {
    sonare_project_free_compile_result(&result);
    project_report_error("compile project", err);
    return project_exit_code(err);
  }
  const bool has_timeline = result.has_timeline != 0;
  if (args.json_output) {
    const std::string messages = result.messages != nullptr ? result.messages : "";
    const std::vector<std::string> diagnostic_messages = project_compile_messages(result.messages);
    JsonBuilder builder;
    builder.begin_object()
        .kv("has_timeline", has_timeline)
        .kv("diagnostic_count", result.diagnostic_count)
        .key("diagnostics")
        .begin_array();
    for (size_t i = 0; i < result.diagnostic_count; ++i) {
      builder.begin_object()
          .kv("code", static_cast<int>(result.diagnostics[i].code))
          .kv("severity", static_cast<int>(result.diagnostics[i].severity))
          .kv("target_id", static_cast<int>(result.diagnostics[i].target_id))
          .kv("message", i < diagnostic_messages.size() ? diagnostic_messages[i] : "")
          .end_object();
    }
    builder.end_array().kv("messages", messages);
    builder.end_object().print();
  } else if (!args.quiet) {
    std::cout << (has_timeline ? color::green : color::yellow)
              << (has_timeline ? "Compiled (renderable timeline)" : "Compiled with errors")
              << color::reset << ", " << result.diagnostic_count << " diagnostic(s)\n";
    if (result.messages != nullptr && result.messages[0] != '\0') {
      std::cout << result.messages << "\n";
    }
  }
  sonare_project_free_compile_result(&result);
  return has_timeline ? 0 : 1;
}

// `project bounce --in in.json -o out.wav` — compile + render the project
// offline to an interleaved WAV file. With `--synth [preset]` MIDI tracks are
// rendered through the NativeSynth catalog. A named preset is a fixed patch;
// the bare flag follows GM bank/program changes and routes channel 10 through
// the GM drum-kit map. Without --synth MIDI tracks render silently.
int cmd_project_bounce(const CliArgs& args) {
  const bool use_synth = args.has("synth");
  SonareSynthInstrumentBinding synth_binding{};
  if (use_synth) {
    const std::string requested = args.get_string("synth");
    const bool auto_select_gm = requested.empty() || requested == "true";
    const std::string preset = auto_select_gm ? "sine" : requested;
    const SonareError patch_error = sonare_synth_preset_patch(preset.c_str(), &synth_binding.patch);
    if (patch_error != SONARE_OK) {
      std::cerr << color::red << "Error: unknown synth preset '" << preset << "'" << color::reset
                << "\n";
      return project_exit_code(patch_error);
    }
    synth_binding.destination_id = 0;
    synth_binding.use_gm_programs = auto_select_gm ? 1 : 0;
  }

  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  double project_sample_rate = 0.0;
  SonareError sr_err = sonare_project_get_sample_rate(handle.ptr, &project_sample_rate);
  if (sr_err != SONARE_OK) {
    project_report_error("read project sample rate", sr_err);
    return project_exit_code(sr_err);
  }

  SonareProjectBounceOptions options{};
  options.total_frames = static_cast<int64_t>(args.get_int("frames", 0));
  options.block_size = args.get_int("block-size", 0);
  options.num_channels = args.get_int("channels", 0);
  options.instrument_latency_samples = args.get_int("instrument-latency", 0);
  // CRITICAL: the WAV header MUST be tagged with the SAME sample rate the render
  // actually used, or the file plays back at the wrong pitch. Default to the
  // project's own rate (queried above via sonare_project_get_sample_rate) rather
  // than a hardcoded value, so a 44.1k/96k project bounces without --sample-rate
  // at all. An explicit --sample-rate is only accepted when it matches the
  // project's rate: the C ABI rejects a genuine mismatch with a generic
  // invalid-parameter error, so the check is duplicated here to name both rates.
  int render_sample_rate = static_cast<int>(std::lround(project_sample_rate));
  if (args.has("sample-rate")) {
    render_sample_rate = args.get_int("sample-rate", render_sample_rate);
    if (std::abs(static_cast<double>(render_sample_rate) - project_sample_rate) > 1e-6) {
      std::cerr << color::red << "Error: --sample-rate " << render_sample_rate
                << " does not match the project's sample rate (" << project_sample_rate
                << " Hz); project bounce renders at the project's own rate" << color::reset << "\n";
      return kExitInvalidParameter;
    }
  }
  options.sample_rate = render_sample_rate;

  float* interleaved = nullptr;
  size_t total = 0;
  SonareError err = use_synth ? sonare_project_bounce_with_synth_instruments(
                                    handle.ptr, &options, &synth_binding, 1, &interleaved, &total)
                              : sonare_project_bounce(handle.ptr, &options, &interleaved, &total);
  if (err != SONARE_OK) {
    project_report_error("bounce project", err);
    return project_exit_code(err);
  }
  const int channels = options.num_channels > 0 ? options.num_channels : 2;
  // The WAV header sample rate equals the render rate the engine used (see above).
  const int sample_rate = render_sample_rate;
  const size_t frames = channels > 0 ? total / static_cast<size_t>(channels) : total;
  std::vector<float> rendered(interleaved, interleaved + total);
  sonare_free_floats(interleaved);
  // The WAV writer requires the count and the layout to agree, so the layout is
  // derived from the count through the shared mapping rather than by a local
  // mono-or-stereo rule that would label any other width stereo. The count is
  // restricted to what the bounce renders (mono or stereo) by
  // validate_cli_arguments before this point.
  const ChannelLayout layout = layout_from_channel_count(channels);
  save_wav_multichannel(args.output_file, rendered.data(), frames, channels, layout, sample_rate);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("frames", frames)
        .kv("channels", channels)
        .kv("sample_rate", sample_rate)
        .kv("synth", use_synth)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Bounced " << frames << " frames (" << channels << " ch @ "
              << sample_rate << " Hz" << (use_synth ? ", NativeSynth" : "") << ") to "
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
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
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
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
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
    return 1;
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
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
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
    return 1;
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
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
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

void print_project_usage(std::ostream& out) {
  out << "Usage: sonare project <subcommand> [options]\n\n"
      << "PROJECT SUBCOMMANDS (headless arrangement / DAW):\n"
      << "  abi                  Print the project C ABI version\n"
      << "  synth-presets        List NativeSynth preset names accepted by --synth\n"
      << "  new                  Create an empty project (-o out.json)\n"
      << "  validate             Round-trip / validate a project (--in in.json [--strict] [-o "
         "out.json])\n"
      << "  compile              Compile a project + report diagnostics (--in in.json)\n"
      << "  bounce               Render a project offline to WAV (--in in.json -o out.wav)\n"
      << "                       Use bare --synth for GM program/channel routing and drums;\n"
      << "                       --synth <preset> selects one fixed NativeSynth patch\n"
      << "                       SF2 and per-destination synth JSON are not exposed here; use the\n"
      << "                       project C/Node/Python/WASM APIs for SoundFont-backed bounces\n"
      << "  export-smf           Export tempo map + MIDI clips to SMF (--in in.json -o out.mid)\n"
      << "  import-smf           Import an SMF into a new project (--smf in.mid -o out.json)\n"
      << "  export-midi2         Export tempo map + MIDI clips to MIDI2 Clip File (--in in.json -o "
         "out.midi2)\n"
      << "  import-midi2         Import MIDI2 Clip File into a new project (--midi2 in.midi2 -o "
         "out.json)\n"
      << "\nOPTIONS:\n"
      << "  --in <file>          Input project JSON\n"
      << "  --smf <file>         Input Standard MIDI File (import-smf)\n"
      << "  --midi2 <file>       Input MIDI 2.0 Clip File (import-midi2)\n"
      << "  -o, --output <file>  Output file\n"
      << "  --sample-rate <hz>   Sample rate (new / bounce; bounce defaults to the project's own "
         "rate)\n"
      << "  --frames <n>         Bounce length in frames\n"
      << "  --channels <n>       Bounce channel count: 1 (mono downmix) or 2 (default 2)\n"
      << "  --strict             Treat project load diagnostics as validation failures\n"
      << "  --synth [preset]     Bare flag: GM program/channel routing + channel-10 drums\n"
      << "                       Value: fixed NativeSynth preset (see synth-presets)\n"
      << "                       No --sf2/--synth-json CLI wiring in this command\n"
      << "  --json               Emit JSON results\n";
}

// `project <subcommand> ...` — dispatches the headless-project subcommands. The
// subcommand lands in the second positional (args.input_file).
int cmd_project(const CliArgs& args, const Audio&) {
  const std::string& sub = args.input_file;
  if (args.help || sub.empty() || sub == "help") {
    print_project_usage(sub.empty() && !args.help ? std::cerr : std::cout);
    return (sub.empty() && !args.help) ? kExitUsage : 0;
  }
  if (sub == "abi") return cmd_project_abi(args);
  if (sub == "synth-presets") return cmd_project_synth_presets(args);
  if (sub == "new") return cmd_project_new(args);
  if (sub == "validate") return cmd_project_validate(args);
  if (sub == "compile") return cmd_project_compile(args);
  if (sub == "bounce") return cmd_project_bounce(args);
  if (sub == "export-smf") return cmd_project_export_smf(args);
  if (sub == "import-smf") return cmd_project_import_smf(args);
  if (sub == "export-midi2") return cmd_project_export_midi2(args);
  if (sub == "import-midi2") return cmd_project_import_midi2(args);
  std::cerr << color::red << "Error: unknown project subcommand '" << sub << "'" << color::reset
            << "\n\n";
  print_project_usage(std::cerr);
  return kExitUsage;
}
#endif  // SONARE_WITH_ARRANGEMENT
