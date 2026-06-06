#pragma once

/// @file sf2_builder.h
/// @brief In-memory synthetic SoundFont (SF2) builder for tests.
///
/// CI must not depend on downloading a real ~30 MB GS SoundFont, so the SF2
/// parser/voice tests build a few-KB, spec-conformant SF2 byte stream from
/// code: caller-supplied PCM, instruments and presets are emitted as a valid
/// RIFF sfbk (INFO + sdta + pdta). Malformed-file tests then truncate or
/// corrupt the produced bytes.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sonare::test {

/// Builds an SF2 file from samples / instruments / presets. Index-based:
/// add_sample() returns the sample index referenced by instrument zones;
/// add_instrument() returns the instrument index referenced by preset zones.
class Sf2Builder {
 public:
  struct GenValue {
    uint16_t oper = 0;
    int16_t amount = 0;
  };

  struct ModValue {
    uint16_t src_oper = 0;
    uint16_t dest_oper = 0;
    int16_t amount = 0;
    uint16_t amount_src_oper = 0;
    uint16_t transform_oper = 0;
  };

  struct ZoneSpec {
    // key/vel ranges emit kGenKeyRange/kGenVelRange when not full-range.
    uint8_t key_lo = 0;
    uint8_t key_hi = 127;
    uint8_t vel_lo = 0;
    uint8_t vel_hi = 127;
    /// Extra generators emitted between the ranges and the terminal gen.
    std::vector<GenValue> gens;
    /// Raw SF2 modulator records emitted for this zone.
    std::vector<ModValue> mods;
    /// Terminal target: sample index (instrument zones) or instrument index
    /// (preset zones); -1 emits a GLOBAL zone (no terminal generator).
    int32_t target = -1;
  };

  /// Adds 16-bit PCM derived from @p samples (floats in [-1,1]); returns the
  /// sample index. Loop points are relative to this sample's own frames.
  int add_sample(const std::string& name, const std::vector<float>& samples, uint32_t sample_rate,
                 uint8_t original_pitch, uint32_t loop_start, uint32_t loop_end,
                 int8_t correction = 0) {
    SampleRec rec;
    rec.name = name;
    rec.start = static_cast<uint32_t>(pcm_.size());
    for (float v : samples) {
      float c = v;
      if (c > 1.0f) c = 1.0f;
      if (c < -1.0f) c = -1.0f;
      pcm_.push_back(static_cast<int16_t>(c * 32767.0f));
    }
    rec.end = static_cast<uint32_t>(pcm_.size());
    rec.loop_start = rec.start + loop_start;
    rec.loop_end = rec.start + loop_end;
    rec.sample_rate = sample_rate;
    rec.original_pitch = original_pitch;
    rec.correction = correction;
    samples_.push_back(std::move(rec));
    // SF2 wants >= 46 zero points between samples; keep fixtures tight but
    // emit a small guard so adjacent reads stay in-bounds.
    for (int i = 0; i < 8; ++i) pcm_.push_back(0);
    return static_cast<int>(samples_.size() - 1);
  }

  int add_instrument(const std::string& name, std::vector<ZoneSpec> zones) {
    instruments_.push_back({name, std::move(zones)});
    return static_cast<int>(instruments_.size() - 1);
  }

  void add_preset(const std::string& name, uint16_t bank, uint16_t program,
                  std::vector<ZoneSpec> zones) {
    presets_.push_back({name, bank, program, std::move(zones)});
  }

  /// Emits the complete RIFF sfbk byte stream.
  std::vector<uint8_t> build() const {
    std::vector<uint8_t> out;
    append(out, "RIFF");
    const size_t riff_size_pos = out.size();
    u32(out, 0);  // patched at the end
    append(out, "sfbk");

    // --- INFO ---
    {
      std::vector<uint8_t> info;
      append(info, "INFO");
      chunk(info, "ifil", [](std::vector<uint8_t>& b) {
        u16(b, 2);  // major
        u16(b, 4);  // minor
      });
      chunk(info, "INAM", [](std::vector<uint8_t>& b) {
        const char* name = "sonare test fixture";
        b.insert(b.end(), name, name + std::strlen(name) + 1);
        if (b.size() % 2 != 0) b.push_back(0);
      });
      chunk(out, "LIST",
            [&](std::vector<uint8_t>& b) { b.insert(b.end(), info.begin(), info.end()); });
    }

    // --- sdta ---
    {
      std::vector<uint8_t> sdta;
      append(sdta, "sdta");
      chunk(sdta, "smpl", [&](std::vector<uint8_t>& b) {
        for (int16_t v : pcm_) u16(b, static_cast<uint16_t>(v));
      });
      chunk(out, "LIST",
            [&](std::vector<uint8_t>& b) { b.insert(b.end(), sdta.begin(), sdta.end()); });
    }

    // --- pdta ---
    {
      std::vector<uint8_t> pdta;
      append(pdta, "pdta");

      // Zone tables are built first so headers know their bag indices.
      std::vector<uint8_t> pbag, pmod, pgen, ibag, imod, igen;
      std::vector<uint16_t> preset_bag_index, inst_bag_index;
      uint16_t pgen_count = 0, pmod_count = 0, ibag_count = 0, imod_count = 0, pbag_count = 0,
               igen_count = 0;

      for (const PresetRec& p : presets_) {
        preset_bag_index.push_back(pbag_count);
        for (const ZoneSpec& z : p.zones) {
          u16(pbag, pgen_count);
          u16(pbag, pmod_count);
          ++pbag_count;
          pgen_count += emit_zone_gens(pgen, z, /*terminal_oper=*/41);
          pmod_count += emit_zone_mods(pmod, z);
        }
      }
      for (const InstrumentRec& ins : instruments_) {
        inst_bag_index.push_back(ibag_count);
        for (const ZoneSpec& z : ins.zones) {
          u16(ibag, igen_count);
          u16(ibag, imod_count);
          ++ibag_count;
          igen_count += emit_zone_gens(igen, z, /*terminal_oper=*/53);
          imod_count += emit_zone_mods(imod, z);
        }
      }
      // Terminal bags close the last zone's generator range.
      u16(pbag, pgen_count);
      u16(pbag, pmod_count);
      u16(ibag, igen_count);
      u16(ibag, imod_count);

      chunk(pdta, "phdr", [&](std::vector<uint8_t>& b) {
        for (size_t i = 0; i < presets_.size(); ++i) {
          fixed_name(b, presets_[i].name);
          u16(b, presets_[i].program);
          u16(b, presets_[i].bank);
          u16(b, preset_bag_index[i]);
          u32(b, 0);
          u32(b, 0);
          u32(b, 0);
        }
        fixed_name(b, "EOP");
        u16(b, 0);
        u16(b, 0);
        u16(b, pbag_count);
        u32(b, 0);
        u32(b, 0);
        u32(b, 0);
      });
      chunk(pdta, "pbag", [&](std::vector<uint8_t>& b) { b = pbag; });
      chunk(pdta, "pmod", [&](std::vector<uint8_t>& b) { b = pmod; });
      chunk(pdta, "pgen", [&](std::vector<uint8_t>& b) { b = pgen; });
      chunk(pdta, "inst", [&](std::vector<uint8_t>& b) {
        for (size_t i = 0; i < instruments_.size(); ++i) {
          fixed_name(b, instruments_[i].name);
          u16(b, inst_bag_index[i]);
        }
        fixed_name(b, "EOI");
        u16(b, ibag_count);
      });
      chunk(pdta, "ibag", [&](std::vector<uint8_t>& b) { b = ibag; });
      chunk(pdta, "imod", [&](std::vector<uint8_t>& b) { b = imod; });
      chunk(pdta, "igen", [&](std::vector<uint8_t>& b) { b = igen; });
      chunk(pdta, "shdr", [&](std::vector<uint8_t>& b) {
        for (const SampleRec& s : samples_) {
          fixed_name(b, s.name);
          u32(b, s.start);
          u32(b, s.end);
          u32(b, s.loop_start);
          u32(b, s.loop_end);
          u32(b, s.sample_rate);
          b.push_back(s.original_pitch);
          b.push_back(static_cast<uint8_t>(s.correction));
          u16(b, 0);  // sample link
          u16(b, 1);  // mono
        }
        fixed_name(b, "EOS");
        u32(b, 0);
        u32(b, 0);
        u32(b, 0);
        u32(b, 0);
        u32(b, 0);
        b.push_back(0);
        b.push_back(0);
        u16(b, 0);
        u16(b, 0);
      });

      chunk(out, "LIST",
            [&](std::vector<uint8_t>& b) { b.insert(b.end(), pdta.begin(), pdta.end()); });
    }

    // Patch the RIFF size (everything after the size field).
    const uint32_t riff_size = static_cast<uint32_t>(out.size() - riff_size_pos - 4);
    out[riff_size_pos] = static_cast<uint8_t>(riff_size & 0xFF);
    out[riff_size_pos + 1] = static_cast<uint8_t>((riff_size >> 8) & 0xFF);
    out[riff_size_pos + 2] = static_cast<uint8_t>((riff_size >> 16) & 0xFF);
    out[riff_size_pos + 3] = static_cast<uint8_t>((riff_size >> 24) & 0xFF);
    return out;
  }

 private:
  struct SampleRec {
    std::string name;
    uint32_t start = 0, end = 0, loop_start = 0, loop_end = 0, sample_rate = 0;
    uint8_t original_pitch = 60;
    int8_t correction = 0;
  };
  struct InstrumentRec {
    std::string name;
    std::vector<ZoneSpec> zones;
  };
  struct PresetRec {
    std::string name;
    uint16_t bank = 0;
    uint16_t program = 0;
    std::vector<ZoneSpec> zones;
  };

  static void append(std::vector<uint8_t>& b, const char* fourcc) {
    b.insert(b.end(), fourcc, fourcc + 4);
  }
  static void u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  }
  static void u32(std::vector<uint8_t>& b, uint32_t v) {
    u16(b, static_cast<uint16_t>(v & 0xFFFF));
    u16(b, static_cast<uint16_t>((v >> 16) & 0xFFFF));
  }
  static void fixed_name(std::vector<uint8_t>& b, const std::string& name) {
    for (size_t i = 0; i < 20; ++i)
      b.push_back(i < name.size() ? static_cast<uint8_t>(name[i]) : 0);
  }

  template <typename Fill>
  static void chunk(std::vector<uint8_t>& out, const char* fourcc, Fill fill) {
    append(out, fourcc);
    const size_t size_pos = out.size();
    u32(out, 0);
    std::vector<uint8_t> body;
    fill(body);
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t size = static_cast<uint32_t>(body.size());
    out[size_pos] = static_cast<uint8_t>(size & 0xFF);
    out[size_pos + 1] = static_cast<uint8_t>((size >> 8) & 0xFF);
    out[size_pos + 2] = static_cast<uint8_t>((size >> 16) & 0xFF);
    out[size_pos + 3] = static_cast<uint8_t>((size >> 24) & 0xFF);
    if (size % 2 != 0) out.push_back(0);  // RIFF word alignment
  }

  /// Emits one zone's generators: ranges first (spec order), then extra gens,
  /// then the terminal instrument/sampleID gen. Returns the generator count.
  static uint16_t emit_zone_gens(std::vector<uint8_t>& b, const ZoneSpec& z,
                                 uint16_t terminal_oper) {
    uint16_t count = 0;
    if (z.key_lo != 0 || z.key_hi != 127) {
      u16(b, 43);  // keyRange
      b.push_back(z.key_lo);
      b.push_back(z.key_hi);
      ++count;
    }
    if (z.vel_lo != 0 || z.vel_hi != 127) {
      u16(b, 44);  // velRange
      b.push_back(z.vel_lo);
      b.push_back(z.vel_hi);
      ++count;
    }
    for (const GenValue& g : z.gens) {
      u16(b, g.oper);
      u16(b, static_cast<uint16_t>(g.amount));
      ++count;
    }
    if (z.target >= 0) {
      u16(b, terminal_oper);
      u16(b, static_cast<uint16_t>(z.target));
      ++count;
    }
    return count;
  }

  static uint16_t emit_zone_mods(std::vector<uint8_t>& b, const ZoneSpec& z) {
    uint16_t count = 0;
    for (const ModValue& m : z.mods) {
      u16(b, m.src_oper);
      u16(b, m.dest_oper);
      u16(b, static_cast<uint16_t>(m.amount));
      u16(b, m.amount_src_oper);
      u16(b, m.transform_oper);
      ++count;
    }
    return count;
  }

  std::vector<int16_t> pcm_;
  std::vector<SampleRec> samples_;
  std::vector<InstrumentRec> instruments_;
  std::vector<PresetRec> presets_;
};

}  // namespace sonare::test
