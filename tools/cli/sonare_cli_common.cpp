#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/audio_tags.h"
#include "sonare_cli.h"

int print_name_catalog(const CliArgs& args, std::string_view key,
                       const std::vector<std::string>& names) {
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key(std::string(key)).begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

std::string cli_token(std::string_view library_token) {
  std::string out;
  out.reserve(library_token.size() + 4);
  auto is_digit = [](char c) { return (c >= '0' && c <= '9') || c == '.'; };
  for (size_t i = 0; i < library_token.size(); ++i) {
    const char c = library_token[i];
    const bool upper = c >= 'A' && c <= 'Z';
    const bool boundary = i > 0 && (upper || is_digit(c) != is_digit(library_token[i - 1]));
    if (boundary) out.push_back('-');
    out.push_back(upper ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

std::string library_token(std::string_view flag, std::string_view value,
                          const std::vector<std::string>& library_tokens) {
  std::string accepted;
  std::vector<std::string> spellings;
  for (const auto& token : library_tokens) spellings.push_back(cli_token(token));
  if (const auto spelled = find_cli_token(value, spellings)) {
    return library_tokens[static_cast<size_t>(
        std::find(spellings.begin(), spellings.end(), *spelled) - spellings.begin())];
  }
  for (const auto& spelled : spellings) accepted += accepted.empty() ? spelled : ", " + spelled;
  throw std::invalid_argument(std::string(flag) + ": unknown value '" + std::string(value) +
                              "'; expected one of: " + accepted);
}

std::string catalog_token(const std::string& value, const std::vector<std::string>& tokens) {
  return find_cli_token(value, tokens).value_or(value);
}

// The registry's list validator has already refused anything but finite numbers.
std::vector<float> parse_float_list(const std::string& text) {
  std::vector<float> values;
  for (const auto& item : split_string(text, ',')) {
    values.push_back(parse_float_strict("values", item));
  }
  return values;
}

std::vector<int> parse_int_list(const std::string& text) {
  std::vector<int> values;
  for (const auto& item : split_string(text, ',')) {
    values.push_back(parse_int_strict("values", item));
  }
  return values;
}

namespace {

// The seek/tell probe only sizes the allocation: it is a snapshot the read
// cannot rely on, because the file may grow between the two calls and a
// non-regular input (a FIFO, a character device) has no size to report at all.
// The cap is therefore enforced on the bytes as they arrive, so nothing past it
// is ever buffered.
bool read_binary_file_bounded(const std::string& path, std::vector<uint8_t>* out,
                              size_t max_bytes) {
  const auto reject_oversized = [&path, max_bytes]() {
    throw std::invalid_argument("input file exceeds " + std::to_string(max_bytes) +
                                " byte limit: " + path);
  };
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size > static_cast<std::streamoff>(max_bytes)) reject_oversized();
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
    if (read_bytes > max_bytes - out->size()) reject_oversized();
    out->insert(out->end(), chunk.data(), chunk.data() + read_bytes);
  }
  return true;
}

}  // namespace

bool read_binary_file(const std::string& path, std::vector<uint8_t>* out) {
  return read_binary_file_bounded(path, out, kMaxCliInputBytes);
}

std::string read_plain_text_file(const std::string& path) {
  std::vector<uint8_t> bytes;
  if (!read_binary_file(path, &bytes)) {
    throw std::invalid_argument("cannot open text file: " + path);
  }
  return std::string(bytes.begin(), bytes.end());
}

// A struct the command fills is named by its members alone, as every binding names them.
void throw_c_failure(SonareError err, const char* root, const char* bag_flag) {
  std::string message = sonare_last_error_message();
  const size_t count = sonare_last_refusal_subject_count();
  if (root != nullptr && count > 0) {
    const std::string prefix = std::string(root) + ".";
    const std::string indexed_prefix = std::string(root) + "[";
    std::vector<std::string> subjects;
    std::vector<const char*> pointers;
    subjects.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      std::string subject = sonare_last_refusal_subject(i);
      if (bag_flag != nullptr &&
          (subject.rfind(prefix, 0) == 0 || subject.rfind(indexed_prefix, 0) == 0)) {
        subject = cli_bag_subject(bag_flag, subject, root);
      } else if (subject.rfind(prefix, 0) == 0) {
        subject.erase(0, prefix.size());
      }
      subjects.push_back(std::move(subject));
    }
    for (const std::string& subject : subjects) pointers.push_back(subject.c_str());
    char* rendered = nullptr;
    if (sonare_last_refusal_render(pointers.data(), nullptr, count, SONARE_PRESENCE_SPELLING_C,
                                   &rendered) == SONARE_OK) {
      message = rendered;
      sonare_free_string(rendered);
    }
  }
  throw sonare::SonareException(static_cast<sonare::ErrorCode>(err), message);
}

void warn_downmixed_input(int channels) {
  std::cerr << "warning: " << channels
            << "-channel input is downmixed to mono by this CLI command; use the stereo "
               "library API for channel-preserving processing\n";
}

namespace {

std::string cli_atomic_tmp_path(const std::string& path) {
  static std::atomic<uint64_t> counter{0};
#ifdef _WIN32
  const unsigned long pid = ::GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  const uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
  return path + ".sonare-tmp." + std::to_string(pid) + "." + std::to_string(sequence);
}

struct CliTmpFileGuard {
  std::string path;
  bool committed = false;

  ~CliTmpFileGuard() {
    if (!committed) std::remove(path.c_str());
  }
};

bool replace_cli_output(const std::string& temporary, const std::string& destination) {
#ifdef _WIN32
  return ::MoveFileExA(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}

}  // namespace

bool write_binary_file(const std::string& path, const uint8_t* data, size_t len) {
  if ((len > 0 && data == nullptr) ||
      len > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }

  const std::string temporary = cli_atomic_tmp_path(path);
  CliTmpFileGuard cleanup{temporary};
  try {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    if (len > 0) {
      file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    }
    const bool write_ok = file.good();
    file.flush();
    const bool flush_ok = file.good();
    file.close();
    const bool close_ok = !file.fail();
    if (!write_ok || !flush_ok || !close_ok) return false;
    if (!replace_cli_output(temporary, path)) return false;
    cleanup.committed = true;
    return true;
  } catch (...) {
    return false;
  }
}

namespace {

Audio load_reference_audio_impl(const CliArgs& args, std::optional<int> expected_sample_rate,
                                std::optional<size_t> expected_size) {
  const std::string path = args.get_string("reference");
  if (path.empty()) {
    throw std::invalid_argument("--reference is required");
  }
  auto [samples, sample_rate] = load_audio(path);
  if (expected_sample_rate && sample_rate != *expected_sample_rate) {
    throw std::invalid_argument("reference sample rate must match input sample rate");
  }
  if (expected_size && samples.size() != *expected_size) {
    throw std::invalid_argument("reference length must match input length");
  }
  validate_offline_audio_input("reference", samples.data(), samples.size(), sample_rate);
  return Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

Audio load_reference_audio(const CliArgs& args, int expected_sample_rate, size_t expected_size) {
  return load_reference_audio_impl(args, expected_sample_rate, expected_size);
}

Audio load_pair_reference_audio(const CliArgs& args) {
  return load_reference_audio_impl(args, std::nullopt, std::nullopt);
}

StereoPlanes load_stereo_planes(const CliArgs& args, const Audio& audio) {
  if (args.decoded_input != nullptr) {
    const auto& cached = cached_interleaved_audio(args);
    SONARE_CHECK(args.decoded_input->sample_rate == audio.sample_rate() &&
                     args.decoded_input->retained_channels == 2,
                 ErrorCode::DecodeFailed);
    SONARE_CHECK(cached.size() % 2 == 0, ErrorCode::DecodeFailed);
    StereoPlanes planes;
    planes.left.resize(cached.size() / 2);
    planes.right.resize(cached.size() / 2);
    for (size_t frame = 0; frame < planes.left.size(); ++frame) {
      planes.left[frame] = cached[2 * frame];
      planes.right[frame] = cached[2 * frame + 1];
    }
    // Nothing reads the interleaved source after this; release it while the planes are processed.
    std::vector<float>().swap(args.decoded_input->interleaved);
    return planes;
  }
  auto [interleaved, sample_rate, channels] = load_audio_interleaved(args.input_file);
  SONARE_CHECK(sample_rate == audio.sample_rate() && channels == 2, ErrorCode::DecodeFailed);
  StereoPlanes planes;
  planes.left.resize(interleaved.size() / 2);
  planes.right.resize(interleaved.size() / 2);
  for (size_t frame = 0; frame < planes.left.size(); ++frame) {
    planes.left[frame] = interleaved[2 * frame];
    planes.right[frame] = interleaved[2 * frame + 1];
  }
  return planes;
}

const std::vector<float>& cached_interleaved_audio(const CliArgs& args) {
  SONARE_CHECK_MSG(args.decoded_input != nullptr, ErrorCode::DecodeFailed,
                   "decoded audio cache is unavailable");
  if (args.decoded_input->retained_error != nullptr) {
    std::rethrow_exception(args.decoded_input->retained_error);
  }
  SONARE_CHECK_MSG(!args.decoded_input->interleaved.empty(), ErrorCode::DecodeFailed,
                   "decoded audio has no retained channels");
  return args.decoded_input->interleaved;
}

OutputFit& OutputFit::include(const float* samples, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    // A non-finite sample is the writer's to count; it would turn the gain into NaN.
    if (std::isfinite(samples[i])) peak_ = std::max(peak_, std::abs(samples[i]));
  }
  return *this;
}

namespace {

// The samples a write hands the encoder: @p samples itself when the fit is unity, else a scaled
// copy in @p scaled.
const float* fitted(const float* samples, size_t count, const OutputFit& fit,
                    std::vector<float>& scaled) {
  if (fit.gain() == 1.0f) return samples;
  scaled.assign(samples, samples + count);
  for (float& sample : scaled) sample *= fit.gain();
  return scaled.data();
}

// A fitted set peaks at exactly 1.0 (x * (1/x) never rounds above 1 in binary32), so the writer
// holding a sample past full scale would be a defect in the fit, not a property of the audio.
void require_fitted(const QuantizeReport& report) {
  if (report.clipped_samples != 0) {
    throw std::logic_error("the output fit left " + std::to_string(report.clipped_samples) +
                           " samples past full scale");
  }
}

void write_stereo_wav_impl(const std::string& path, const float* left, size_t left_size,
                           const float* right, size_t right_size, int sample_rate, int bits,
                           const OutputFit& fit) {
  SONARE_CHECK_MSG(left != nullptr || left_size == 0, ErrorCode::InvalidParameter,
                   "stereo WAV left channel is null");
  SONARE_CHECK_MSG(right != nullptr || right_size == 0, ErrorCode::InvalidParameter,
                   "stereo WAV right channel is null");
  SONARE_CHECK_MSG(left_size <= std::numeric_limits<size_t>::max() / 2, ErrorCode::InvalidParameter,
                   "stereo WAV is too large");
  std::vector<float> interleaved(left_size * 2);
  for (size_t frame = 0; frame < left_size; ++frame) {
    interleaved[2 * frame] = left[frame] * fit.gain();
    interleaved[2 * frame + 1] = frame < right_size ? right[frame] * fit.gain() : 0.0f;
  }
  QuantizeReport report;
  save_wav_multichannel(path, interleaved.data(), left_size, 2, ChannelLayout::Stereo, sample_rate,
                        bits, &report);
  require_fitted(report);
}

}  // namespace

void write_wav(const std::string& path, const float* samples, size_t count, int sample_rate,
               int bits, const OutputFit& fit) {
  std::vector<float> scaled;
  QuantizeReport report;
  save_wav(path, fitted(samples, count, fit, scaled), count, sample_rate, bits, &report);
  require_fitted(report);
}

void write_wav(const std::string& path, const Audio& audio, int bits, const OutputFit& fit) {
  write_wav(path, audio.data(), audio.size(), audio.sample_rate(), bits, fit);
}

void write_stereo_wav(const std::string& path, const std::vector<float>& left,
                      const std::vector<float>& right, int sample_rate, int bits,
                      const OutputFit& fit) {
  write_stereo_wav_impl(path, left.data(), left.size(), right.data(), right.size(), sample_rate,
                        bits, fit);
}

void write_stereo_wav(const std::string& path, const Audio& left, const Audio& right,
                      int sample_rate, int bits, const OutputFit& fit) {
  SONARE_CHECK_MSG(left.sample_rate() == sample_rate && right.sample_rate() == sample_rate,
                   ErrorCode::InvalidParameter, "stereo WAV sample rates do not match");
  write_stereo_wav_impl(path, left.data(), left.size(), right.data(), right.size(), sample_rate,
                        bits, fit);
}

void write_wav_multichannel(const std::string& path, const float* interleaved, size_t frames,
                            int channels, ChannelLayout layout, int sample_rate, int bits,
                            const OutputFit& fit) {
  SONARE_CHECK_MSG(
      channels > 0 && frames <= std::numeric_limits<size_t>::max() / static_cast<size_t>(channels),
      ErrorCode::InvalidParameter, "multichannel WAV is too large");
  std::vector<float> scaled;
  QuantizeReport report;
  save_wav_multichannel(path,
                        fitted(interleaved, frames * static_cast<size_t>(channels), fit, scaled),
                        frames, channels, layout, sample_rate, bits, &report);
  require_fitted(report);
}

void note_output_fit(const CliArgs& args, const OutputFit& fit) {
  if (args.quiet || fit.gain() == 1.0f) return;
  std::cerr << color::yellow << "Output lowered by " << std::fixed << std::setprecision(2)
            << -fit.gain_db() << std::defaultfloat
            << " dB so its peak fits full scale instead of clipping" << color::reset << "\n";
}

sonare::util::json::Value parse_cli_json_document(const std::string& text) {
  try {
    return sonare::util::json::parse_strict(text);
  } catch (const sonare::util::json::JsonDuplicateKeyError& error) {
    throw CliDuplicateDocumentError(error.what());
  }
}

sonare::util::json::Value parse_cli_json_scalar(const std::string& raw) {
  try {
    return sonare::util::json::parse(raw);
  } catch (const std::exception&) {
    return sonare::util::json::Value(raw);
  }
}

std::string find_voice_preset_in_pack(const std::string& pack_json, const std::string& preset_id) {
  const auto root = parse_cli_json_document(pack_json);
  const auto* presets = root.find("presets");
  if (presets == nullptr || !presets->is_array()) {
    throw std::invalid_argument("preset pack must contain a presets array");
  }
  const sonare::util::json::Value* match = nullptr;
  for (const auto& item : presets->as_array()) {
    const auto* id = item.find("id");
    if (id == nullptr || !id->is_string()) continue;
    if (id->as_string() == preset_id) {
      if (match != nullptr)
        throw std::invalid_argument("duplicate preset id in preset pack: " + preset_id);
      match = &item;
    }
  }
  if (match == nullptr)
    throw std::invalid_argument("preset not found in preset pack: " + preset_id);
  return sonare::util::json::dump(*match);
}

namespace {

bool voice_set_path_has_prefix(const VoiceSetPath& path, const VoiceSetPath& prefix) {
  return path.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), path.begin());
}

std::string voice_set_flatten_path(const VoiceSetPath& path) {
  std::string flat;
  for (const auto& part : path) {
    if (part.array_index) {
      flat += "[" + part.value + "]";
    } else {
      if (!flat.empty()) flat.push_back('.');
      flat += part.value;
    }
  }
  return flat;
}

void erase_voice_set_subtree(std::map<VoiceSetPath, VoiceSetSource>* paths,
                             const VoiceSetPath& prefix) {
  for (auto it = paths->begin(); it != paths->end();) {
    if (voice_set_path_has_prefix(it->first, prefix)) {
      it = paths->erase(it);
    } else {
      ++it;
    }
  }
}

void erase_voice_set_subtree(std::set<VoiceSetPath>* paths, const VoiceSetPath& prefix) {
  for (auto it = paths->begin(); it != paths->end();) {
    if (voice_set_path_has_prefix(*it, prefix)) {
      it = paths->erase(it);
    } else {
      ++it;
    }
  }
}

void collect_voice_set_document_paths(const sonare::util::json::Value& value,
                                      const VoiceSetPath& path,
                                      std::set<VoiceSetPath>* paths) {
  if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      VoiceSetPath child_path = path;
      child_path.push_back({key, false});
      paths->insert(child_path);
      collect_voice_set_document_paths(child, child_path, paths);
    }
  } else if (value.is_array()) {
    for (size_t index = 0; index < value.as_array().size(); ++index) {
      VoiceSetPath child_path = path;
      child_path.push_back({std::to_string(index), true});
      paths->insert(child_path);
      collect_voice_set_document_paths(value.as_array()[index], child_path, paths);
    }
  }
}

}  // namespace

void set_json_path(sonare::util::json::Value& root, const std::string& path,
                   sonare::util::json::Value value, VoiceSetOverlay* overlay,
                   std::string_view assignment) {
  auto parts = split_string(path, '.');
  if (parts.empty()) throw std::invalid_argument("empty --set path");
  if (!root.is_object()) {
    root = sonare::util::json::Object{};
    if (overlay != nullptr) {
      overlay->assignment_by_path.clear();
      overlay->untouched_document_paths.clear();
    }
  }
  sonare::util::json::Value* cursor = &root;
  VoiceSetPath assigned_path;
  for (const auto& part : parts) assigned_path.push_back({part, false});
  VoiceSetPath current;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    auto& object = cursor->as_object();
    auto it = object.find(parts[i]);
    current.push_back({parts[i], false});
    if (it == object.end() || !it->second.is_object()) {
      if (overlay != nullptr) {
        erase_voice_set_subtree(&overlay->assignment_by_path, current);
        erase_voice_set_subtree(&overlay->untouched_document_paths, current);
        overlay->assignment_by_path[current] = {std::string(assignment), assigned_path, true};
      }
      it = object.insert_or_assign(parts[i], sonare::util::json::Object{}).first;
    }
    cursor = &it->second;
  }

  current.push_back({parts.back(), false});
  if (overlay != nullptr) {
    erase_voice_set_subtree(&overlay->assignment_by_path, current);
    erase_voice_set_subtree(&overlay->untouched_document_paths, current);
  }
  cursor->as_object()[parts.back()] = std::move(value);
  if (overlay != nullptr) {
    const auto record = [&](const auto& self, const sonare::util::json::Value& current_value,
                            const VoiceSetPath& current_path) -> void {
      overlay->assignment_by_path[current_path] = {std::string(assignment), assigned_path, false};
      if (current_value.is_object()) {
        for (const auto& [key, child] : current_value.as_object()) {
          VoiceSetPath child_path = current_path;
          child_path.push_back({key, false});
          self(self, child, child_path);
        }
      } else if (current_value.is_array()) {
        for (size_t index = 0; index < current_value.as_array().size(); ++index) {
          VoiceSetPath child_path = current_path;
          child_path.push_back({std::to_string(index), true});
          self(self, current_value.as_array()[index], child_path);
        }
      }
    };
    record(record, cursor->as_object().at(parts.back()), current);
  }
}

// One --set occurrence is one PATH=VALUE assignment. The value reaches the JSON
// parser byte for byte, so an object, an array, or a string that carries its own
// commas survives; splitting the occurrences on any separator would tear those
// apart with no escape available to the caller.
std::string apply_voice_preset_sets(std::string config_text,
                                    const std::vector<std::string>& assignments,
                                    VoiceSetOverlay* overlay) {
  if (overlay != nullptr) *overlay = {};
  if (assignments.empty()) return config_text;
  auto root = parse_cli_json_document(config_text);
  if (overlay != nullptr) {
    collect_voice_set_document_paths(root, {}, &overlay->untouched_document_paths);
  }
  for (const auto& assignment : assignments) {
    if (assignment.empty()) continue;
    // The command validator (voice_set_refusal) has already refused a malformed assignment.
    const auto eq = assignment.find('=');
    const std::string path = assignment.substr(0, eq);
    auto value = parse_cli_json_scalar(assignment.substr(eq + 1));
    set_json_path(root, path, std::move(value), overlay, path);
  }
  return sonare::util::json::dump(root);
}

namespace {

std::optional<std::string> voice_set_caller_subject(const std::string& subject,
                                                    const VoiceSetOverlay& overlay) {
  const VoiceSetSource* owner = nullptr;
  size_t owner_length = 0;
  for (const auto& [path, source] : overlay.assignment_by_path) {
    const std::string candidate = voice_set_flatten_path(path);
    const bool exact = candidate == subject;
    const bool descendant =
        subject.size() > candidate.size() &&
        subject.compare(0, candidate.size(), candidate) == 0 &&
        (subject[candidate.size()] == '.' || subject[candidate.size()] == '[');
    if (!exact && !descendant) continue;
    if (candidate.size() < owner_length) continue;
    if (candidate.size() == owner_length && owner != nullptr &&
        (owner->assignment != source.assignment || owner->value_path != source.value_path ||
         owner->intermediate_container != source.intermediate_container)) {
      return std::nullopt;
    }
    owner = &source;
    owner_length = candidate.size();
  }
  if (owner == nullptr) return std::nullopt;
  for (const auto& path : overlay.untouched_document_paths) {
    if (voice_set_flatten_path(path) == subject) return std::nullopt;
  }
  if (owner->intermediate_container) {
    return cli_bag_subject("--set", owner->assignment);
  }
  const std::string assigned_path = voice_set_flatten_path(owner->value_path);
  if (subject.size() < assigned_path.size() ||
      subject.compare(0, assigned_path.size(), assigned_path) != 0 ||
      (subject.size() > assigned_path.size() && subject[assigned_path.size()] != '.' &&
       subject[assigned_path.size()] != '[')) {
    return std::nullopt;
  }
  return cli_bag_subject("--set",
                         owner->assignment + subject.substr(assigned_path.size()));
}

}  // namespace

SonareParameterRefusal respell_voice_set_refusal(const SonareParameterRefusal& refusal,
                                                 const VoiceSetOverlay& overlay) {
  bool has_argument_subject = false;
  for (const auto& subject : refusal.subjects()) {
    if (voice_set_caller_subject(subject, overlay)) {
      has_argument_subject = true;
      break;
    }
  }
  const ErrorCode code =
      has_argument_subject ? ErrorCode::InvalidParameter : refusal.code();
  return refusal.respelled(code, [&](const std::string& subject) {
    if (const auto spelled = voice_set_caller_subject(subject, overlay)) return *spelled;
    return subject;
  }, false);
}

void print_float_values(const CliArgs& args, const std::vector<float>& values) {
  if (args.json_output) {
    JsonBuilder().float_array(values).print();
    return;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) std::cout << ",";
    std::cout << values[i];
  }
  std::cout << "\n";
}

void print_int_values(const CliArgs& args, const std::vector<int>& values) {
  JsonBuilder json;
  if (args.json_output) {
    json.begin_array();
    for (int value : values) json.value(value);
    json.end_array().print();
    return;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) std::cout << ",";
    std::cout << values[i];
  }
  std::cout << "\n";
}

std::vector<float> require_float_values(const CliArgs& args) {
  return parse_float_list(args.get_string("values"));
}

std::vector<int> require_int_values(const CliArgs& args) {
  return parse_int_list(args.get_string("values"));
}

// ============================================================================
// Command Implementations
// ============================================================================

int cmd_version(const CliArgs& args) {
  // The CLI ships in lockstep with the library, so both report the same compiled
  // version (SONARE_VERSION_STRING via version()) rather than a stale literal.
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("cli", "native")
        .kv("cli_version", version())
        .kv("lib_version", version())
        .end_object()
        .print();
  } else {
    std::cout << "sonare-cli version " << version() << "\n";
    std::cout << "libsonare version " << version() << "\n";
  }
  return 0;
}

int cmd_doctor(const CliArgs& args) {
  char* raw_capabilities = nullptr;
  const SonareError capabilities_err = sonare_capabilities_json(&raw_capabilities);
  SONARE_CHECK_MSG(
      capabilities_err == SONARE_OK, sonare::ErrorCode::OutOfMemory,
      std::string("Failed to read native capabilities: ") + sonare_last_error_message());
  const std::string capabilities_json(raw_capabilities);
  sonare_free_string(raw_capabilities);
  const auto native_capabilities = sonare::util::json::parse_strict(capabilities_json);
  const auto& native_object = native_capabilities.as_object();

  // The C ABI keeps its historical camelCase field name for compatibility.
  // CLI JSON is the cross-language surface, so normalize and close the
  // top-level shape here without changing the public capabilities descriptor.
  sonare::util::json::Object capabilities_object;
  capabilities_object.emplace("version", native_object.at("version"));
  capabilities_object.emplace("abi", native_object.at("abi"));
  capabilities_object.emplace("platform", native_object.at("platform"));
  capabilities_object.emplace("features", native_object.at("features"));
  capabilities_object.emplace("decode", native_object.at("decode"));
  capabilities_object.emplace("simd", native_object.at("simd"));
  capabilities_object.emplace("hardware_concurrency", native_object.at("hardwareConcurrency"));
  const auto capabilities = sonare::util::json::Value(std::move(capabilities_object));

  if (args.json_output) {
    std::cout << sonare::util::json::dump(capabilities) << "\n";
    return 0;
  }

  const auto& abi = capabilities["abi"];
  const auto& features = capabilities["features"];
  const auto& decode = capabilities["decode"];

  std::cout << color::cyan << color::bold << "libsonare Doctor" << color::reset << "\n";
  std::cout << "  Version: " << capabilities["version"].as_string() << "\n";
  std::cout << "  Platform: " << capabilities["platform"].as_string() << "\n";
  std::cout << "  ABI: project " << abi["project"].as_number() << ", engine "
            << abi["engine"].as_number() << "\n";
  std::cout << "  Features: mastering=" << (features["mastering"].as_bool() ? "yes" : "no")
            << ", mixing=" << (features["mixing"].as_bool() ? "yes" : "no")
            << ", fx=" << (features["fx"].as_bool() ? "yes" : "no")
            << ", ffmpeg=" << (features["ffmpeg"].as_bool() ? "yes" : "no") << "\n";
  std::cout << "  Built-in decode: " << decode["builtin"].size() << " formats\n";
  std::cout << "  SIMD: " << capabilities["simd"].as_string() << "\n";
  std::cout << "  Hardware concurrency: " << capabilities["hardware_concurrency"].as_number()
            << "\n";
  return 0;
}

int cmd_system_info(const CliArgs& args) {
  if (args.json_output) {
    char* json = nullptr;
    if (sonare_system_info_json(&json) != SONARE_OK) {
      std::cerr << color::red << "Error: " << sonare_last_error_message() << color::reset << "\n";
      return 1;
    }
    std::cout << json << "\n";
    sonare_free_string(json);
    return 0;
  }
  const auto count_text = [](const std::optional<int>& count) {
    return count ? std::to_string(*count) : std::string("unknown");
  };
  const auto gigabytes_text = [](const std::optional<size_t>& bytes) {
    if (!bytes) return std::string("unknown");
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB",
                  static_cast<float>(*bytes) / (1024.0f * 1024.0f * 1024.0f));
    return std::string(text);
  };
  std::cout << color::cyan << color::bold << "System Information" << color::reset << "\n";
  std::cout << "  CPU Cores: " << count_text(system_info::logical_cores()) << " logical, "
            << count_text(system_info::physical_cores()) << " physical\n";
  std::cout << "  Memory: " << gigabytes_text(system_info::total_memory_bytes()) << " total, "
            << gigabytes_text(system_info::available_memory_bytes()) << " available\n";
  std::cout << "\n"
            << color::green << color::bold << "Parallel Configuration" << color::reset << "\n";
  std::cout << "  Parallel Enabled: " << (system_info::parallel_enabled() ? "yes" : "no") << "\n";
  std::cout << "  Workers: " << system_info::parallel_workers() << "\n";
  std::cout << "  Strategy: " << system_info::parallel_strategy() << "\n";
  return 0;
}

int cmd_frames_to_samples(const CliArgs& args, const Audio&) {
  const int frames = args.get_int("frames", 0);
  const int hop_length = args.hop_length;
  const int samples = args.has("n-fft") ? frames_to_samples(frames, hop_length, args.n_fft)
                                        : frames_to_samples(frames, hop_length);
  if (args.json_output) {
    JsonBuilder().begin_object().kv("samples", samples).end_object().print();
  } else {
    std::cout << samples << "\n";
  }
  return 0;
}

int cmd_samples_to_frames(const CliArgs& args, const Audio&) {
  const int samples = args.get_int("samples", 0);
  const int hop_length = args.hop_length;
  const int frames = args.has("n-fft") ? samples_to_frames(samples, hop_length, args.n_fft)
                                       : samples_to_frames(samples, hop_length);
  if (args.json_output) {
    JsonBuilder().begin_object().kv("frames", frames).end_object().print();
  } else {
    std::cout << frames << "\n";
  }
  return 0;
}

int cmd_power_to_db(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, power_to_db(values, args.get_float("ref", kDefaultDbReference),
                                       args.get_float("amin", constants::kEpsilon),
                                       args.get_float("top-db", constants::kDefaultTopDb)));
  return 0;
}

int cmd_amplitude_to_db(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, amplitude_to_db(values, args.get_float("ref", kDefaultDbReference),
                                           args.get_float("amin", kDefaultAmplitudeDbFloor),
                                           args.get_float("top-db", constants::kDefaultTopDb)));
  return 0;
}

int cmd_db_to_power(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, db_to_power(values, args.get_float("ref", kDefaultDbReference)));
  return 0;
}

int cmd_db_to_amplitude(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, db_to_amplitude(values, args.get_float("ref", kDefaultDbReference)));
  return 0;
}

int cmd_frame_signal(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  const int frame_length = args.get_int("frame-length", 0);
  const int hop_length = args.get_int("hop-length", 0);
  auto frames = frame(values, frame_length, hop_length);
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", frame_count(values.size(), frame_length, hop_length))
        .key("frames")
        .float_array(frames)
        .end_object()
        .print();
  } else {
    print_float_values(args, frames);
  }
  return 0;
}

int cmd_pad_center(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  const auto size = static_cast<size_t>(args.get_int("size", 0));
  print_float_values(args, pad_center(values, size, args.get_float("pad-value", kDefaultPadValue)));
  return 0;
}

int cmd_fix_length(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  const auto size = static_cast<size_t>(args.get_int("size", 0));
  print_float_values(args, fix_length(values, size, args.get_float("pad-value", kDefaultPadValue)));
  return 0;
}

int cmd_fix_frames(const CliArgs& args, const Audio&) {
  auto values = require_int_values(args);
  print_int_values(args,
                   fix_frames(values, args.get_int("x-min", kDefaultFixFramesMin),
                              args.get_int("x-max", kFixFramesNoUpperBound), !args.has("no-pad")));
  return 0;
}

int cmd_peak_pick(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_int_values(args, peak_pick(values, args.get_int("pre-max", 0), args.get_int("post-max", 0),
                                   args.get_int("pre-avg", 0), args.get_int("post-avg", 0),
                                   args.get_float("delta", 0.0f), args.get_int("wait", 0)));
  return 0;
}

int cmd_vector_normalize(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  const int norm_type = args.get_int("norm-type", static_cast<int>(NormType::Inf));
  const NormType norm = static_cast<NormType>(norm_type);
  print_float_values(args, args.has("threshold")
                               ? normalize(values, norm, args.get_float("threshold", 0.0f))
                               : normalize(values, norm));
  return 0;
}

int cmd_pcen(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  PcenConfig config;
  config.sr = args.get_int("sample-rate", config.sr);
  if (args.has("hop-length")) config.hop_length = args.hop_length;
  config.time_constant = args.get_float("time-constant", config.time_constant);
  config.gain = args.get_float("gain", config.gain);
  config.bias = args.get_float("bias", config.bias);
  config.power = args.get_float("power", config.power);
  config.eps = args.get_float("eps", config.eps);
  const int n_bins = args.get_int("n-bins", 0);
  const int n_frames = args.get_int("n-frames", 0);
  print_float_values(args, pcen(values, n_bins, n_frames, config));
  return 0;
}

int cmd_info(const CliArgs& args, const Audio& audio) {
  if (args.source_channels <= 0) {
    throw std::invalid_argument("invalid audio channel count");
  }

  const int channels = args.source_channels;
  float peak = 0.0f;
  double rms_sum = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    const float val = std::abs(audio.data()[i]);
    peak = std::max(peak, val);
    const double sample = static_cast<double>(audio.data()[i]);
    rms_sum += sample * sample;
  }
  const double rms = std::sqrt(rms_sum / static_cast<double>(audio.size()));
  const float peak_db = 20.0f * std::log10(std::max(peak, 1e-10f));
  const double rms_db = 20.0 * std::log10(std::max(rms, 1e-10));

  if (args.json_output) {
    std::vector<uint8_t> bytes;
    if (!read_binary_file_bounded(args.input_file, &bytes, sonare::resource::kMaxAudioFileBytes)) {
      throw SonareException(ErrorCode::FileNotFound, "cannot open audio file: " + args.input_file);
    }
    AudioTags tags;
    try {
      tags = read_audio_tags(bytes.data(), bytes.size());
    } catch (const SonareException& error) {
      // A playable container without a tag reader still has audio information.
      if (error.code() != ErrorCode::InvalidFormat) throw;
    }
    JsonBuilder json;
    json.begin_object()
        .kv("path", args.input_file)
        .kv("duration", audio.duration())
        .kv("sample_rate", audio.sample_rate())
        .kv("channels", channels)
        .kv("samples", audio.size())
        .kv("peak_db", peak_db)
        .kv("rms_db", rms_db)
        .key("tags")
        .begin_object();
    for (const auto& [key, value] : tags) json.kv(key, value);
    json.end_object().end_object().print();
  } else {
    int mins = static_cast<int>(audio.duration()) / 60;
    int secs = static_cast<int>(audio.duration()) % 60;
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << mins << ":" << std::setfill('0')
              << std::setw(2) << secs << " (" << std::fixed << std::setprecision(1)
              << audio.duration() << "s)" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Sample Rate: " << audio.sample_rate() << " Hz"
              << color::reset << "\n";
    if (channels > 0) {
      std::cout << "  " << color::blue << "> Channels: " << channels << color::reset << "\n";
    }
    std::cout << "  " << color::blue << "> Samples: " << audio.size() << color::reset << "\n";
    std::cout << "  " << color::green << "> Peak Level: " << std::fixed << std::setprecision(1)
              << peak_db << " dB" << color::reset << "\n";
    std::cout << "  " << color::green << "> RMS Level: " << rms_db << " dB" << color::reset
              << "\n\n";
  }
  return 0;
}
