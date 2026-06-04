#include "midi/synth/sf2_file.h"

#include <algorithm>
#include <cstring>

namespace sonare::midi::synth {

namespace {

/// Bounds-checked little-endian cursor over the raw file bytes.
class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t size) noexcept : data_(data), size_(size) {}

  size_t pos() const noexcept { return pos_; }
  size_t remaining() const noexcept { return size_ - pos_; }
  bool ok() const noexcept { return ok_; }

  void seek(size_t pos) noexcept {
    if (pos > size_) {
      ok_ = false;
      pos_ = size_;
    } else {
      pos_ = pos;
    }
  }

  void skip(size_t n) noexcept { seek(pos_ + n); }

  uint8_t u8() noexcept {
    if (pos_ + 1 > size_) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }

  uint16_t u16() noexcept {
    if (pos_ + 2 > size_) {
      ok_ = false;
      pos_ = size_;
      return 0;
    }
    const uint16_t v =
        static_cast<uint16_t>(data_[pos_]) | (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }

  int16_t s16() noexcept { return static_cast<int16_t>(u16()); }

  uint32_t u32() noexcept {
    if (pos_ + 4 > size_) {
      ok_ = false;
      pos_ = size_;
      return 0;
    }
    const uint32_t v = static_cast<uint32_t>(data_[pos_]) |
                       (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                       (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                       (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
  }

  /// Reads a 4-char chunk id into @p out (not NUL-terminated).
  bool fourcc(char out[4]) noexcept {
    if (pos_ + 4 > size_) {
      ok_ = false;
      return false;
    }
    std::memcpy(out, data_ + pos_, 4);
    pos_ += 4;
    return true;
  }

  /// Reads a fixed-size NUL-padded name field of @p n bytes.
  std::string name(size_t n) noexcept {
    if (pos_ + n > size_) {
      ok_ = false;
      pos_ = size_;
      return {};
    }
    const char* p = reinterpret_cast<const char*>(data_ + pos_);
    size_t len = 0;
    while (len < n && p[len] != '\0') ++len;
    pos_ += n;
    return std::string(p, len);
  }

  const uint8_t* raw(size_t n) noexcept {
    if (pos_ + n > size_) {
      ok_ = false;
      return nullptr;
    }
    const uint8_t* p = data_ + pos_;
    pos_ += n;
    return p;
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  bool ok_ = true;
};

bool id_is(const char id[4], const char* expect) noexcept {
  return std::memcmp(id, expect, 4) == 0;
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

// Raw pdta record arrays prior to zone resolution.
struct PresetHeader {
  std::string name;
  uint16_t preset = 0;
  uint16_t bank = 0;
  uint16_t bag_index = 0;
};

struct InstHeader {
  std::string name;
  uint16_t bag_index = 0;
};

struct Bag {
  uint16_t gen_index = 0;
  uint16_t mod_index = 0;
};

/// Decodes one bag range [first, last) of generators/modulators into an
/// Sf2Zone, capturing key/vel ranges and the terminal instrument/sampleID.
Sf2Zone make_zone(const std::vector<Sf2Gen>& gens, const std::vector<Sf2Mod>& mods,
                  const Bag& begin, const Bag& end, bool preset_level) {
  Sf2Zone zone;
  const size_t g0 = begin.gen_index;
  const size_t g1 = std::max<size_t>(end.gen_index, g0);
  const size_t m0 = begin.mod_index;
  const size_t m1 = std::max<size_t>(end.mod_index, m0);
  zone.gens.assign(gens.begin() + static_cast<ptrdiff_t>(std::min(g0, gens.size())),
                   gens.begin() + static_cast<ptrdiff_t>(std::min(g1, gens.size())));
  zone.mods.assign(mods.begin() + static_cast<ptrdiff_t>(std::min(m0, mods.size())),
                   mods.begin() + static_cast<ptrdiff_t>(std::min(m1, mods.size())));
  for (const Sf2Gen& g : zone.gens) {
    switch (g.oper) {
      case kGenKeyRange:
        zone.key_lo = g.range_lo();
        zone.key_hi = g.range_hi();
        break;
      case kGenVelRange:
        zone.vel_lo = g.range_lo();
        zone.vel_hi = g.range_hi();
        break;
      case kGenInstrument:
        if (preset_level) zone.instrument = static_cast<uint16_t>(g.amount);
        break;
      case kGenSampleId:
        if (!preset_level) zone.sample = static_cast<uint16_t>(g.amount);
        break;
      default:
        break;
    }
  }
  return zone;
}

}  // namespace

const Sf2Gen* Sf2Zone::find_gen(uint16_t oper) const noexcept {
  const Sf2Gen* found = nullptr;
  for (const Sf2Gen& g : gens) {
    if (g.oper == oper) found = &g;
  }
  return found;
}

int Sf2File::find_preset(uint16_t bank, uint16_t program) const noexcept {
  for (size_t i = 0; i < presets_.size(); ++i) {
    if (presets_[i].bank == bank && presets_[i].program == program) return static_cast<int>(i);
  }
  return -1;
}

void Sf2File::clear() {
  presets_.clear();
  instruments_.clear();
  samples_.clear();
  sample_pool_.clear();
}

bool Sf2File::parse(const uint8_t* data, size_t size, std::string* error) {
  clear();
  if (data == nullptr || size < 12) return fail(error, "sf2: file too small");

  ByteReader file(data, size);
  char id[4];
  if (!file.fourcc(id) || !id_is(id, "RIFF")) return fail(error, "sf2: missing RIFF header");
  const uint32_t riff_size = file.u32();
  if (!file.fourcc(id) || !id_is(id, "sfbk")) return fail(error, "sf2: not an sfbk form");
  // The RIFF size must cover the form type + chunks (tolerate trailing slack).
  if (riff_size < 4 || static_cast<size_t>(riff_size) + 8 > size + 1) {
    return fail(error, "sf2: RIFF size exceeds file");
  }

  // Locate the three top-level LIST chunks.
  const uint8_t* smpl_data = nullptr;
  size_t smpl_bytes = 0;
  const uint8_t* sm24_data = nullptr;
  size_t sm24_bytes = 0;
  size_t pdta_pos = 0;
  size_t pdta_size = 0;
  bool saw_info = false;

  while (file.remaining() >= 8) {
    if (!file.fourcc(id)) return fail(error, "sf2: truncated chunk header");
    const uint32_t chunk_size = file.u32();
    if (chunk_size > file.remaining()) return fail(error, "sf2: chunk size exceeds file");
    const size_t chunk_pos = file.pos();
    if (id_is(id, "LIST") && chunk_size >= 4) {
      ByteReader list(data + chunk_pos, chunk_size);
      char list_type[4];
      list.fourcc(list_type);
      if (id_is(list_type, "INFO")) {
        saw_info = true;
      } else if (id_is(list_type, "sdta")) {
        // Sub-chunks: smpl (16-bit PCM), optional sm24 (low bytes).
        while (list.remaining() >= 8) {
          char sub_id[4];
          if (!list.fourcc(sub_id)) break;
          const uint32_t sub_size = list.u32();
          if (sub_size > list.remaining()) return fail(error, "sf2: sdta sub-chunk overruns");
          if (id_is(sub_id, "smpl")) {
            smpl_data = data + chunk_pos + list.pos();
            smpl_bytes = sub_size;
          } else if (id_is(sub_id, "sm24")) {
            sm24_data = data + chunk_pos + list.pos();
            sm24_bytes = sub_size;
          }
          list.skip(sub_size + (sub_size & 1u));
        }
      } else if (id_is(list_type, "pdta")) {
        pdta_pos = chunk_pos + 4;
        pdta_size = chunk_size - 4;
      }
    }
    file.seek(chunk_pos + chunk_size + (chunk_size & 1u));
  }

  (void)saw_info;  // INFO is informational; a missing INFO list is tolerated.
  if (pdta_size == 0) return fail(error, "sf2: missing pdta LIST");

  // --- pdta sub-chunks ---
  std::vector<PresetHeader> phdr;
  std::vector<Bag> pbag;
  std::vector<Sf2Mod> pmod;
  std::vector<Sf2Gen> pgen;
  std::vector<InstHeader> inst;
  std::vector<Bag> ibag;
  std::vector<Sf2Mod> imod;
  std::vector<Sf2Gen> igen;

  {
    ByteReader pdta(data + pdta_pos, pdta_size);
    while (pdta.remaining() >= 8) {
      char sub_id[4];
      if (!pdta.fourcc(sub_id)) return fail(error, "sf2: truncated pdta sub-chunk");
      const uint32_t sub_size = pdta.u32();
      if (sub_size > pdta.remaining()) return fail(error, "sf2: pdta sub-chunk overruns");
      ByteReader sub(data + pdta_pos + pdta.pos(), sub_size);
      if (id_is(sub_id, "phdr")) {
        if (sub_size % 38 != 0) return fail(error, "sf2: phdr record size mismatch");
        for (size_t i = 0; i < sub_size / 38; ++i) {
          PresetHeader h;
          h.name = sub.name(20);
          h.preset = sub.u16();
          h.bank = sub.u16();
          h.bag_index = sub.u16();
          sub.skip(12);  // library / genre / morphology
          phdr.push_back(std::move(h));
        }
      } else if (id_is(sub_id, "pbag") || id_is(sub_id, "ibag")) {
        if (sub_size % 4 != 0) return fail(error, "sf2: bag record size mismatch");
        auto& bags = sub_id[0] == 'p' ? pbag : ibag;
        for (size_t i = 0; i < sub_size / 4; ++i) {
          Bag b;
          b.gen_index = sub.u16();
          b.mod_index = sub.u16();
          bags.push_back(b);
        }
      } else if (id_is(sub_id, "pmod") || id_is(sub_id, "imod")) {
        if (sub_size % 10 != 0) return fail(error, "sf2: mod record size mismatch");
        auto& mods = sub_id[0] == 'p' ? pmod : imod;
        for (size_t i = 0; i < sub_size / 10; ++i) {
          Sf2Mod m;
          m.src_oper = sub.u16();
          m.dest_oper = sub.u16();
          m.amount = sub.s16();
          m.amount_src_oper = sub.u16();
          m.transform_oper = sub.u16();
          mods.push_back(m);
        }
      } else if (id_is(sub_id, "pgen") || id_is(sub_id, "igen")) {
        if (sub_size % 4 != 0) return fail(error, "sf2: gen record size mismatch");
        auto& gens = sub_id[0] == 'p' ? pgen : igen;
        for (size_t i = 0; i < sub_size / 4; ++i) {
          Sf2Gen g;
          g.oper = sub.u16();
          g.amount = sub.s16();
          gens.push_back(g);
        }
      } else if (id_is(sub_id, "inst")) {
        if (sub_size % 22 != 0) return fail(error, "sf2: inst record size mismatch");
        for (size_t i = 0; i < sub_size / 22; ++i) {
          InstHeader h;
          h.name = sub.name(20);
          h.bag_index = sub.u16();
          inst.push_back(std::move(h));
        }
      } else if (id_is(sub_id, "shdr")) {
        if (sub_size % 46 != 0) return fail(error, "sf2: shdr record size mismatch");
        for (size_t i = 0; i < sub_size / 46; ++i) {
          Sf2Sample s;
          s.name = sub.name(20);
          s.start = sub.u32();
          s.end = sub.u32();
          s.loop_start = sub.u32();
          s.loop_end = sub.u32();
          s.sample_rate = sub.u32();
          s.original_pitch = sub.u8();
          s.correction = static_cast<int8_t>(sub.u8());
          s.sample_link = sub.u16();
          s.sample_type = sub.u16();
          samples_.push_back(std::move(s));
        }
      }
      if (!sub.ok()) return fail(error, "sf2: truncated pdta records");
      pdta.skip(sub_size + (sub_size & 1u));
    }
  }

  if (phdr.empty() || samples_.empty()) {
    clear();
    return fail(error, "sf2: missing phdr or shdr records");
  }
  // Drop the terminal records (EOP / EOI / EOS).
  phdr.pop_back();
  if (!inst.empty()) inst.pop_back();
  samples_.pop_back();

  // --- sample pool: convert 16-bit (+ optional 24-bit extension) to float ---
  if (smpl_data == nullptr || smpl_bytes < 2) {
    clear();
    return fail(error, "sf2: missing smpl PCM data");
  }
  const size_t num_points = smpl_bytes / 2;
  const bool use_sm24 = sm24_data != nullptr && sm24_bytes >= num_points;
  sample_pool_.resize(num_points);
  for (size_t i = 0; i < num_points; ++i) {
    const int16_t hi = static_cast<int16_t>(static_cast<uint16_t>(smpl_data[2 * i]) |
                                            (static_cast<uint16_t>(smpl_data[2 * i + 1]) << 8));
    if (use_sm24) {
      const int32_t v24 = (static_cast<int32_t>(hi) << 8) | sm24_data[i];
      sample_pool_[i] = static_cast<float>(v24) * (1.0f / 8388608.0f);
    } else {
      sample_pool_[i] = static_cast<float>(hi) * (1.0f / 32768.0f);
    }
  }

  // Validate / clamp sample header indices against the pool.
  for (Sf2Sample& s : samples_) {
    const uint32_t pool = static_cast<uint32_t>(sample_pool_.size());
    s.start = std::min(s.start, pool);
    s.end = std::clamp(s.end, s.start, pool);
    s.loop_start = std::clamp(s.loop_start, s.start, s.end);
    s.loop_end = std::clamp(s.loop_end, s.loop_start, s.end);
  }

  // --- resolve instrument zones ---
  for (size_t i = 0; i < inst.size(); ++i) {
    Sf2Instrument out;
    out.name = inst[i].name;
    const size_t bag_first = inst[i].bag_index;
    const size_t bag_last = i + 1 < inst.size() ? inst[i + 1].bag_index : ibag.size() - 1;
    if (bag_first > bag_last || bag_last + 1 > ibag.size()) {
      clear();
      return fail(error, "sf2: instrument bag indices out of range");
    }
    for (size_t b = bag_first; b < bag_last; ++b) {
      Sf2Zone zone = make_zone(igen, imod, ibag[b], ibag[b + 1], /*preset_level=*/false);
      if (zone.sample >= 0 && static_cast<size_t>(zone.sample) >= samples_.size()) {
        continue;  // dangling sample reference: drop the zone, keep the file
      }
      // Only the first zone may be global (carries no terminal sampleID).
      if (zone.is_global() && !out.zones.empty()) continue;
      out.zones.push_back(std::move(zone));
    }
    instruments_.push_back(std::move(out));
  }

  // --- resolve preset zones ---
  for (size_t i = 0; i < phdr.size(); ++i) {
    Sf2Preset out;
    out.name = phdr[i].name;
    out.program = phdr[i].preset;
    out.bank = phdr[i].bank;
    const size_t bag_first = phdr[i].bag_index;
    const size_t bag_last = i + 1 < phdr.size() ? phdr[i + 1].bag_index : pbag.size() - 1;
    if (bag_first > bag_last || bag_last + 1 > pbag.size()) {
      clear();
      return fail(error, "sf2: preset bag indices out of range");
    }
    for (size_t b = bag_first; b < bag_last; ++b) {
      Sf2Zone zone = make_zone(pgen, pmod, pbag[b], pbag[b + 1], /*preset_level=*/true);
      if (zone.instrument >= 0 && static_cast<size_t>(zone.instrument) >= instruments_.size()) {
        continue;  // dangling instrument reference
      }
      if (zone.is_global() && !out.zones.empty()) continue;
      out.zones.push_back(std::move(zone));
    }
    presets_.push_back(std::move(out));
  }

  return true;
}

}  // namespace sonare::midi::synth
