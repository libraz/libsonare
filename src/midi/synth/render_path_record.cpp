#include "midi/synth/render_path_record.h"

#include <algorithm>
#include <cstdio>
#include <utility>

// The one translation unit that includes the generated digest, so a registry
// change recompiles this file alone.
#include "bank_registry_digest.h"

#if defined(SONARE_TUNING) && SONARE_TUNING
#include <cstdlib>
#endif

namespace sonare::midi::synth {

namespace {

void append_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
      out.append(buf);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

void append_strings(std::string& out, const std::vector<std::string>& values) {
  out.push_back('[');
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out.push_back(',');
    append_string(out, values[i]);
  }
  out.push_back(']');
}

void append_int(std::string& out, int64_t value) { out.append(std::to_string(value)); }

void append_part(std::string& out, const RenderPathPart& p) {
  out.append("{\"part\":");
  append_int(out, p.part);
  out.append(",\"program\":");
  append_int(out, p.program);
  out.append(",\"bank\":");
  append_int(out, p.bank);
  out.append(",\"backend\":");
  append_string(out, p.backend);
  out.append(",\"rig_source\":");
  append_string(out, p.rig_source);
  out.append(",\"stages\":");
  append_strings(out, p.stages);
  if (!p.skipped.empty()) {
    out.append(",\"skipped\":");
    append_strings(out, p.skipped);
  }
  out.append(",\"unit\":");
  if (p.unit < 0) {
    out.append("null");
  } else {
    append_int(out, p.unit);
  }
  out.append(",\"mono_prefix\":");
  append_int(out, p.mono_prefix);
  out.append(",\"send_tap\":");
  append_string(out, p.send_tap);
  out.push_back('}');
}

void append_unit(std::string& out, const RenderPathUnit& u) {
  out.append("{\"unit\":");
  append_int(out, u.unit);
  char type[8];
  std::snprintf(type, sizeof(type), "0x%04X", static_cast<unsigned>(u.type));
  out.append(",\"type\":");
  append_string(out, type);
  out.append(",\"realization\":");
  append_string(out, u.realization == GsEfxRealization::kClassic ? "classic" : "modern");
  out.append(",\"stages\":");
  append_strings(out, u.stages);
  out.append(",\"enabled\":[");
  for (size_t i = 0; i < u.enabled.size(); ++i) {
    if (i != 0) out.push_back(',');
    out.append(u.enabled[i] ? "true" : "false");
  }
  out.push_back(']');
  if (!u.skipped.empty()) {
    out.append(",\"skipped\":");
    append_strings(out, u.skipped);
  }
  out.push_back('}');
}

bool same_text(const char* a, const char* b) {
  return std::string_view(a == nullptr ? "" : a) == std::string_view(b == nullptr ? "" : b);
}

}  // namespace

bool RenderPathPart::operator==(const RenderPathPart& o) const {
  return part == o.part && program == o.program && bank == o.bank &&
         same_text(backend, o.backend) && same_text(rig_source, o.rig_source) &&
         stages == o.stages && skipped == o.skipped && unit == o.unit &&
         mono_prefix == o.mono_prefix && same_text(send_tap, o.send_tap);
}

bool RenderPathUnit::operator==(const RenderPathUnit& o) const {
  return unit == o.unit && type == o.type && realization == o.realization && stages == o.stages &&
         enabled == o.enabled && skipped == o.skipped;
}

void RenderPathRecorder::set_block_frame(int64_t frame) {
  if (stopped_) return;
  if (frame < max_frame_) {
    stopped_ = true;
    mark_incomplete("the instrument rendered more than one pass; only the first is recorded");
    return;
  }
  frame_ = frame;
  max_frame_ = frame;
}

void RenderPathRecorder::record_topology(RenderPathTopology topology) {
  if (stopped_) return;
  if (has_topology_) {
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
      if (it->kind != RenderPathEvent::Kind::kTopology) continue;
      if (it->topology == topology) return;
      break;
    }
  }
  const bool refused = std::any_of(topology.parts.begin(), topology.parts.end(),
                                   [](const RenderPathPart& p) { return !p.skipped.empty(); }) ||
                       std::any_of(topology.units.begin(), topology.units.end(),
                                   [](const RenderPathUnit& u) { return !u.skipped.empty(); });
  if (refused) mark_incomplete("the insert factory refused a stage; it is listed under skipped");
  RenderPathEvent event;
  event.kind = RenderPathEvent::Kind::kTopology;
  event.frame = frame_;
  event.topology = std::move(topology);
  events_.push_back(std::move(event));
  has_topology_ = true;
}

void RenderPathRecorder::record_param(uint8_t unit, uint8_t slot, uint8_t value) {
  if (stopped_) return;
  RenderPathEvent event;
  event.kind = RenderPathEvent::Kind::kParam;
  event.frame = frame_;
  event.unit = unit;
  event.slot = slot;
  event.value = value;
  events_.push_back(std::move(event));
}

void RenderPathRecorder::begin_snapshot_build() noexcept {
  for (auto& names : refused_) names.clear();
}

void RenderPathRecorder::note_refused_stage(int part, std::string_view name) {
  refused_[static_cast<size_t>(part & 0x0F)].emplace_back(name);
}

void RenderPathRecorder::mark_incomplete(std::string_view reason) {
  if (reason_.empty()) reason_ = std::string(reason);
}

std::string RenderPathRecorder::to_json(std::string_view library_version) const {
  std::string out;
  out.append("{\"schema\":1,\"bank_registry_digest\":");
  const char* digest = bank_registry_digest();
  if (digest == nullptr) {
    out.append("null");
  } else {
    append_string(out, digest);
  }
  out.append(",\"library_version\":");
  append_string(out, library_version);
  out.append(",\"complete\":");
  out.append(complete() ? "true" : "false");
  out.append(",\"reason\":");
  if (complete()) {
    out.append("null");
  } else {
    append_string(out, reason_);
  }
  out.append(",\"events\":[");
  for (size_t i = 0; i < events_.size(); ++i) {
    const RenderPathEvent& e = events_[i];
    out.append(i == 0 ? "\n" : ",\n");
    out.append("{\"frame\":");
    append_int(out, e.frame);
    if (e.kind == RenderPathEvent::Kind::kParam) {
      out.append(",\"kind\":\"param\",\"unit\":");
      append_int(out, e.unit);
      out.append(",\"slot\":");
      append_int(out, e.slot);
      out.append(",\"value\":");
      append_int(out, e.value);
      out.push_back('}');
      continue;
    }
    out.append(",\"kind\":\"topology\",\"parts\":[");
    for (size_t p = 0; p < e.topology.parts.size(); ++p) {
      if (p != 0) out.push_back(',');
      append_part(out, e.topology.parts[p]);
    }
    out.append("],\"units\":[");
    for (size_t u = 0; u < e.topology.units.size(); ++u) {
      if (u != 0) out.push_back(',');
      append_unit(out, e.topology.units[u]);
    }
    out.append("]}");
  }
  out.append("]}\n");
  return out;
}

const char* bank_registry_digest() noexcept { return generated::kBankRegistryDigest; }

#if defined(SONARE_TUNING) && SONARE_TUNING

std::string render_path_dump_path() {
  const char* path = std::getenv("SONARE_RENDER_PATH_DUMP");
  return path == nullptr ? std::string() : std::string(path);
}

bool write_render_path_dump(const std::string& path, const RenderPathRecorder& recorder,
                            std::string_view library_version) {
  if (path.empty()) return false;
  const std::string json = recorder.to_json(library_version);
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const bool written = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  return std::fclose(file) == 0 && written;
}

#endif  // SONARE_TUNING

}  // namespace sonare::midi::synth
