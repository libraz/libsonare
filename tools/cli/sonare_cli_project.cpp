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
// Usage exit code, mirroring kExitUsage in tools/cli/sonare_cli.cpp. A missing or
// blank `project` subcommand is a usage error, which is a different class from
// the invalid-parameter code a plain `1` carries.
constexpr int kExitUsage = 2;

// Invalid-parameter exit code, mirroring kExitInvalidParameter in
// tools/cli/sonare_cli.cpp. This is what a plain `1` from any handler normalizes
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

// Reports a failed write of the caller's output artifact. Every project handler
// that writes one routes here, so the six copies of this rejection cannot drift
// apart. The class is the one save_wav already gives a failed render rather
// than the plain `1` these used to return, which normalize_handler_exit folds to
// the invalid-parameter code -- that names the argument, not the write.
int report_output_write_failure(const std::string& path) {
  std::cerr << color::red << "Error: cannot write " << path << color::reset << "\n";
  return project_exit_code(SONARE_ERROR_ENCODE_FAILED);
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
std::string project_input_path(const CliArgs& args) {
  return args.has("in") ? args.get_string("in") : args.get_string("project", args.input_file);
}

bool load_project_from_args(const CliArgs& args, ProjectHandle* handle,
                            std::string* diagnostics = nullptr, SonareError* load_error = nullptr) {
  if (load_error != nullptr) *load_error = SONARE_OK;
  const std::string in_path = project_input_path(args);
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
    // The loader has already printed the diagnostic, and its message carries the
    // deserializer detail this one cannot. Returning the class it reported keeps
    // that the single Error line: throwing here made main's handler print a
    // second, less specific one for the same failure. A malformed project still
    // exits with the format class, and stdout still stays empty either way, so a
    // machine caller branches on the exit code without parsing a payload.
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
      return report_output_write_failure(args.output_file);
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
  // A project that compiled without a renderable timeline is a project-state
  // failure, the class `project validate --strict` already reports for its own
  // failing outcome. A plain 1 normalizes to the invalid-parameter code, which
  // says the arguments were wrong when they were not.
  return has_timeline ? 0 : kExitInvalidState;
}

// A project source's serialized `kind`: 0 audio, 1 MIDI (SourceKind). Only an
// audio source carries a `uri`, so the kind decides which entries are
// addressable by --audio at all.
constexpr int kProjectSourceKindAudio = 0;

// The only URI scheme --resolve-audio opens. The core never opens a URI itself,
// so resolving one is this front-end's own file I/O, and it has no business
// fetching over a network to feed a render.
constexpr char kFileUriPrefix[] = "file://";

// Percent-decodes a URI path. A malformed escape is kept verbatim, so a path
// containing a bare '%' stays openable instead of being mangled.
std::string percent_decode(const std::string& text) {
  const auto hex_value = [](char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const int high = (text[i] == '%' && i + 2 < text.size()) ? hex_value(text[i + 1]) : -1;
    const int low = high >= 0 ? hex_value(text[i + 2]) : -1;
    if (low >= 0) {
      out.push_back(static_cast<char>((high << 4) | low));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

// Resolves a file:// URI to a local path. Returns false for any other scheme
// and for an authority naming another host, neither of which this front-end can
// open. The POSIX decoding is percent-decoding alone, which is what the other
// front-end's url2pathname does on the same platforms.
bool path_from_file_uri(const std::string& uri, std::string* out) {
  const std::string prefix(kFileUriPrefix);
  if (uri.compare(0, prefix.size(), prefix) != 0) return false;
  const std::string rest = uri.substr(prefix.size());
  const size_t slash = rest.find('/');
  if (slash == std::string::npos) return false;
  const std::string authority = rest.substr(0, slash);
  if (!authority.empty() && authority != "localhost") return false;
  *out = percent_decode(rest.substr(slash));
  return true;
}

// Maps each audio source id in the document to the URI it references.
//
// Read from the document rather than from the loaded handle: the flat source
// descriptor carries the URI in a fixed-width field, so anything longer arrives
// truncated, and a truncated URI is one --resolve-audio would then try to open.
// The document is the only place the whole string survives.
std::map<uint32_t, std::string> project_audio_source_uris(const std::string& document) {
  std::map<uint32_t, std::string> uris;
  const auto root = sonare::util::json::parse(document);
  const auto* sources = root.find("sources");
  if (sources == nullptr || !sources->is_array()) return uris;
  for (const auto& source : sources->as_array()) {
    const auto* kind = source.find("kind");
    const auto* id = source.find("id");
    const auto* uri = source.find("uri");
    if (kind == nullptr || !kind->is_number() || kind->as_int() != kProjectSourceKindAudio)
      continue;
    if (id == nullptr || !id->is_number()) continue;
    uris.emplace(static_cast<uint32_t>(id->as_number()),
                 uri != nullptr && uri->is_string() ? uri->as_string() : std::string());
  }
  return uris;
}

// Every audio source the loaded document has no PCM for, in the order the C ABI
// reports them. @p err carries the first failed C call, so a read that could not
// happen is not reported as an empty list.
std::vector<uint32_t> project_unresolved_audio_source_ids(const SonareProject* project,
                                                          SonareError* err) {
  std::vector<uint32_t> ids;
  size_t count = 0;
  *err = sonare_project_unresolved_audio_source_count(project, &count);
  if (*err != SONARE_OK) return ids;
  for (size_t index = 0; index < count; ++index) {
    uint32_t source_id = 0;
    *err = sonare_project_unresolved_audio_source_id_by_index(project, index, &source_id);
    if (*err != SONARE_OK) return {};
    ids.push_back(source_id);
  }
  return ids;
}

// Splits one `--audio <source_id>=FILE` assignment at the FIRST '=', so a path
// containing one stays intact. A source is addressed by id alone: its URI is
// unique too, but accepting either spelling would put two addresses on one
// binding, and the id is what the unresolved-source report already names.
void parse_audio_binding(const std::string& assignment, uint32_t* out_source_id,
                         std::string* out_path) {
  const size_t separator = assignment.find('=');
  if (separator == 0 || separator == std::string::npos || separator + 1 >= assignment.size()) {
    throw std::invalid_argument("--audio expects <source_id>=FILE, got '" + assignment + "'");
  }
  const long long source_id = parse_int64_strict("audio", assignment.substr(0, separator));
  if (source_id <= 0 || source_id > static_cast<long long>(UINT32_MAX)) {
    throw std::invalid_argument("--audio source id out of range: " +
                                assignment.substr(0, separator));
  }
  *out_source_id = static_cast<uint32_t>(source_id);
  *out_path = assignment.substr(separator + 1);
}

// Decodes an audio file and registers its PCM against one audio source.
SonareError bind_source_audio(SonareProject* project, uint32_t source_id, const std::string& path) {
  auto [interleaved, sample_rate, channels] = load_audio_interleaved(path);
  if (channels <= 0 || interleaved.empty()) {
    throw std::invalid_argument("--audio " + std::to_string(source_id) + "=" + path +
                                " decoded no samples");
  }
  const int64_t frames = static_cast<int64_t>(interleaved.size() / static_cast<size_t>(channels));
  return sonare_project_set_source_audio(project, source_id, interleaved.data(), frames, channels,
                                         sample_rate);
}

// Supplies the PCM a bounce needs for the document's audio sources.
//
// Project JSON references audio by URI / storage handle only and the core never
// opens either, so a document whose clips reference audio has nothing to render
// from until a host binds samples. This front-end is that host: `--audio` binds
// one named source to a file and `--resolve-audio` opens the file:// URIs the
// document already carries. Anything still unresolved is refused naming every
// source and its URI, rather than reaching the render as a bare invalid-state
// error that says nothing about what is missing.
//
// Returns 0 when the render may proceed, and the exit code to report otherwise.
int resolve_bounce_audio_sources(const CliArgs& args, SonareProject* project) {
  const std::vector<std::string> assignments = args.get_string_list("audio");
  const bool resolve = args.has("resolve-audio");
  SonareError unresolved_error = SONARE_OK;
  const std::vector<uint32_t> initial =
      project_unresolved_audio_source_ids(project, &unresolved_error);
  if (unresolved_error != SONARE_OK) {
    project_report_error("read unresolved audio sources", unresolved_error);
    return project_exit_code(unresolved_error);
  }
  // A document with no audio sources at all -- every MIDI-only project -- has
  // nothing to resolve and nothing to report, so it never pays for the second
  // read the URI map costs.
  if (assignments.empty() && !resolve && initial.empty()) return 0;

  const std::string in_path = project_input_path(args);
  std::vector<uint8_t> document;
  if (!read_binary_file(in_path, &document)) {
    std::cerr << color::red << "Error: cannot open project file: " << in_path << color::reset
              << "\n";
    return project_exit_code(SONARE_ERROR_FILE_NOT_FOUND);
  }
  const std::map<uint32_t, std::string> uris =
      project_audio_source_uris(std::string(document.begin(), document.end()));

  for (const auto& assignment : assignments) {
    uint32_t source_id = 0;
    std::string path;
    parse_audio_binding(assignment, &source_id, &path);
    if (uris.count(source_id) == 0) {
      std::cerr << color::red << "Error: --audio " << assignment
                << ": the project has no audio source " << source_id << color::reset << "\n";
      return kExitInvalidParameter;
    }
    const SonareError err = bind_source_audio(project, source_id, path);
    if (err != SONARE_OK) {
      project_report_error("bind audio source " + std::to_string(source_id), err);
      return project_exit_code(err);
    }
  }
  if (args.has("resolve-audio")) {
    SonareError err = SONARE_OK;
    for (const uint32_t source_id : project_unresolved_audio_source_ids(project, &err)) {
      const auto found = uris.find(source_id);
      const std::string uri = found != uris.end() ? found->second : std::string();
      std::string path;
      if (!path_from_file_uri(uri, &path)) {
        std::cerr << color::red << "Error: source " << source_id << " (" << uri
                  << "): --resolve-audio opens file:// URIs only; pass --audio " << source_id
                  << "=FILE" << color::reset << "\n";
        return kExitInvalidParameter;
      }
      const SonareError bind_err = bind_source_audio(project, source_id, path);
      if (bind_err != SONARE_OK) {
        project_report_error("bind audio source " + std::to_string(source_id), bind_err);
        return project_exit_code(bind_err);
      }
    }
    if (err != SONARE_OK) {
      project_report_error("read unresolved audio sources", err);
      return project_exit_code(err);
    }
  }
  SonareError err = SONARE_OK;
  const std::vector<uint32_t> remaining = project_unresolved_audio_source_ids(project, &err);
  if (err != SONARE_OK) {
    project_report_error("read unresolved audio sources", err);
    return project_exit_code(err);
  }
  if (remaining.empty()) return 0;
  // One line per source, each carrying its own id, because the caller has to act
  // on every one of them: stopping at the first turns a document with four
  // missing takes into four runs.
  for (const uint32_t source_id : remaining) {
    const auto found = uris.find(source_id);
    std::cerr << color::red << "Error: source " << source_id << " ("
              << (found != uris.end() ? found->second : std::string())
              << ") has no audio; pass --audio " << source_id << "=FILE or --resolve-audio"
              << color::reset << "\n";
  }
  return kExitInvalidState;
}

// `project bounce --in in.json -o out.wav` — compile + render the project
// offline to an interleaved WAV file. With `--synth [preset]` MIDI tracks are
// rendered through the NativeSynth catalog. A named preset is a fixed patch;
// the bare flag follows GM bank/program changes and routes channel 10 through
// the GM drum-kit map. Without --synth MIDI tracks render silently.
//
// `binds_source_audio` is false for `midi-render`, which declares neither
// --audio nor --resolve-audio: it has no way to supply the PCM an unresolved
// source needs, and naming those options in its refusal would advertise a flag
// the command does not accept.
int project_bounce_impl(const CliArgs& args, bool use_synth, bool binds_source_audio = true) {
  SonareSynthInstrumentBinding synth_binding{};
  if (use_synth) {
    const std::string requested = args.get_string("synth");
    const bool auto_select_gm = requested.empty() || requested == "true";
    if (auto_select_gm) {
      // GM routing replaces the patch at every note-on, so a base preset named
      // here would assert thirty field values the caller never chose and that
      // nothing ever reads. The zero-initialized patch is the init patch (an
      // empty preset name), which is what the other front-end sends: with the
      // patch inert either spelling renders the same, and this one stays the
      // same if the routing is ever asked to yield.
      synth_binding.patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    } else {
      const SonareError patch_error =
          sonare_synth_preset_patch(requested.c_str(), &synth_binding.patch);
      if (patch_error != SONARE_OK) {
        std::cerr << color::red << "Error: unknown synth preset '" << requested << "'"
                  << color::reset << "\n";
        return project_exit_code(patch_error);
      }
    }
    synth_binding.destination_id = 0;
    synth_binding.use_gm_programs = auto_select_gm ? 1 : 0;
  }

  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  if (binds_source_audio) {
    const int audio_exit = resolve_bounce_audio_sources(args, handle.ptr);
    if (audio_exit != 0) return audio_exit;
  }

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
    options.sample_rate = render_sample_rate;
  }
  // Left at 0 when the caller did not ask for a rate, which is what the C ABI
  // reads as "the project's own". Pinning the rounded rate unconditionally made
  // a project whose rate is not an integer fail the ABI's own equality check
  // against the full-precision value the int cannot carry. The header below
  // still reports the nearest integer to the rate the engine rendered at, which
  // is the closest a RIFF header can come to a fractional rate.

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

int cmd_project_bounce(const CliArgs& args) { return project_bounce_impl(args, args.has("synth")); }

// `midi-render --in in.json -o out.wav [--synth preset]` — the same render with
// the synth pinned on, so a MIDI project reaches audio without the caller
// having to know it is a project bounce underneath. An empty `--synth` follows
// GM bank/program changes, which is what makes the bare form useful on a
// general-MIDI file.
int cmd_midi_render(const CliArgs& args, const Audio&) {
  return project_bounce_impl(args, true, false);
}

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

// Every option a `project.*` leaf accepts that @p curated does not already
// name, rendered the way the leaf-level help renders it. Matching on the flag
// followed by a space or a newline keeps one option from hiding another whose
// name it is a prefix of.
std::vector<std::string> unlisted_project_options(const std::string& curated) {
  std::vector<std::string> out;
  for (const auto& command : cli_command_registry()) {
    if (command.path.rfind("project.", 0) != 0) continue;
    for (const auto& option : command.options) {
      if (!option.inventory || option.name == "json") continue;
      const std::string flag = "--" + option.name;
      if (curated.find(flag + " ") != std::string::npos) continue;
      if (curated.find(flag + "\n") != std::string::npos) continue;
      std::string display = flag;
      if (option.arity == CliOptionArity::RequiredValue) {
        display += " <value>";
      } else if (option.arity == CliOptionArity::OptionalValue) {
        display += " [value]";
      }
      if (std::find(out.begin(), out.end(), display) == out.end()) out.push_back(display);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
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
      << "                       Audio clips need their sources bound first: --audio "
         "<id>=<file>\n"
      << "                       per source, or --resolve-audio for file:// URIs in the "
         "document\n"
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
      << "\nOPTIONS:\n";

  // The curated lines carry per-option guidance the registry has no room for,
  // so they stay hand-written; what cannot stay hand-written is which options
  // exist. Anything a project leaf accepts and these lines do not name is
  // appended from the registry below, so a new leaf option cannot be visible in
  // `project <leaf> --help` and missing from this overview.
  const std::string curated =
      "  --in <file>          Input project JSON\n"
      "  --smf <file>         Input Standard MIDI File (import-smf)\n"
      "  --midi2 <file>       Input MIDI 2.0 Clip File (import-midi2)\n"
      "  -o, --output <file>  Output file\n"
      "  --sample-rate <hz>   Sample rate (new / bounce; bounce defaults to the project's own "
      "rate)\n"
      "  --frames <n>         Bounce length in frames\n"
      "  --channels <n>       Bounce channel count: 1 (mono downmix) or 2 (default 2)\n"
      "  --strict             Treat project load diagnostics as validation failures\n"
      "  --audio <id>=<file>  Bind decoded PCM to project audio source <id> (bounce;\n"
      "                       repeat once per source, since project JSON carries only a\n"
      "                       URI reference and the core never opens one)\n"
      "  --resolve-audio      Open the file:// URIs the document's unresolved audio\n"
      "                       sources carry; any other scheme is refused by name\n"
      "  --synth [preset]     Bare flag: GM program/channel routing + channel-10 drums\n"
      "                       Value: fixed NativeSynth preset (see synth-presets)\n"
      "                       No --sf2/--synth-json CLI wiring in this command\n"
      "  --json               Emit JSON results\n";
  out << curated;
  for (const auto& option : unlisted_project_options(curated)) out << "  " << option << "\n";
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
