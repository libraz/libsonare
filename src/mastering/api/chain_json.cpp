/// @file chain_json.cpp
/// @brief JSON serialization for MasteringChainConfig.

#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/param_field_tables.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/json_schema.h"
#include "util/resource_limits.h"

namespace sonare::mastering::api {
namespace {

using JsonValue = sonare::util::json::Value;
namespace schema = sonare::util::json::schema;

// Tree builders. JSON goes through util::json::dump so number formatting is locale-independent
// and survives a dump -> parse round-trip without drift.

void add_field(sonare::util::json::Object& params, const char* key, bool value) {
  params.emplace(key, JsonValue(value));
}

void add_field(sonare::util::json::Object& params, const char* key, double value) {
  params.emplace(key, JsonValue(value));
}

// Integer config fields (int, std::size_t, etc.) must route to the `double`
// overload — without this template they would implicitly convert to `bool`.
template <typename Int,
          typename = std::enable_if_t<std::is_integral_v<Int> && !std::is_same_v<Int, bool>>>
void add_field(sonare::util::json::Object& params, const char* key, Int value) {
  params.emplace(key, JsonValue(static_cast<double>(value)));
}

// Enum config fields serialize as the name the reader resolves back to the value.
template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>, typename = void>
void add_field(sonare::util::json::Object& params, const char* key, Enum value) {
  params.emplace(key, detail::enum_param_value(key, value));
}

sonare::util::json::Object build_multiband_params(const MasteringChainConfig& cfg) {
  using sonare::mastering::dynamics::CompressorConfig;

  sonare::util::json::Object multiband;
  add_field(multiband, "enabled", cfg.dynamics.multiband_comp.enabled);

  sonare::util::json::Object crossover;
  sonare::util::json::Array cutoffs;
  cutoffs.reserve(cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size());
  for (const float cutoff : cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz) {
    cutoffs.emplace_back(static_cast<double>(cutoff));
  }
  util::json::put(crossover, "cutoffsHz", std::move(cutoffs));
  add_field(crossover, "slope", cfg.dynamics.multiband_comp.config.crossover.slope);
  add_field(crossover, "mode", cfg.dynamics.multiband_comp.config.crossover.mode);
  add_field(crossover, "firKernelSize",
            cfg.dynamics.multiband_comp.config.crossover.fir_kernel_size);
  util::json::put(multiband, "crossover", std::move(crossover));

  sonare::util::json::Array bands;
  bands.reserve(cfg.dynamics.multiband_comp.config.bands.size());
  for (const CompressorConfig& band_config : cfg.dynamics.multiband_comp.config.bands) {
    sonare::util::json::Object band;
#define X(key, member, meta) add_field(band, key, band_config.member);
    SONARE_FIELDS_COMPRESSOR_BEFORE_HIGH_PASS(X)
    if (band_config.sidechain_high_pass_hz > 0.0f) {
      add_field(band, "sidechainHighPassHz", band_config.sidechain_high_pass_hz);
    }
    SONARE_FIELDS_COMPRESSOR_AFTER_HIGH_PASS(X)
#undef X
    bands.emplace_back(JsonValue(std::move(band)));
  }
  util::json::put(multiband, "bands", std::move(bands));
  return multiband;
}

sonare::util::json::Object build_chain_params(const MasteringChainConfig& cfg) {
  sonare::util::json::Object params;

  add_field(params, "repair.declick.enabled", cfg.repair.declick.enabled);
  add_field(params, "repair.declick.threshold", cfg.repair.declick.config.threshold);
  add_field(params, "repair.declick.neighborRatio", cfg.repair.declick.config.neighbor_ratio);
  add_field(params, "repair.declick.maxClickSamples", cfg.repair.declick.config.max_click_samples);
  add_field(params, "repair.declick.lpcOrder", cfg.repair.declick.config.lpc_order);
  add_field(params, "repair.declick.residualRatio", cfg.repair.declick.config.residual_ratio);

  add_field(params, "repair.declip.enabled", cfg.repair.declip.enabled);
  add_field(params, "repair.declip.clipThresholdDb",
            sonare::linear_to_db(cfg.repair.declip.config.clip_threshold));
  add_field(params, "repair.declip.lpcOrder", cfg.repair.declip.config.lpc_order);
  add_field(params, "repair.declip.iterations", cfg.repair.declip.config.iterations);
  add_field(params, "repair.declip.linkChannels", cfg.repair.declip.config.link_channels);

  add_field(params, "repair.decrackle.enabled", cfg.repair.decrackle.enabled);
  add_field(params, "repair.decrackle.threshold", cfg.repair.decrackle.config.threshold);
  add_field(params, "repair.decrackle.mode", cfg.repair.decrackle.config.mode);
  add_field(params, "repair.decrackle.levels", cfg.repair.decrackle.config.levels);

  add_field(params, "repair.dehum.enabled", cfg.repair.dehum.enabled);
  add_field(params, "repair.dehum.fundamentalHz", cfg.repair.dehum.config.fundamental_hz);
  add_field(params, "repair.dehum.harmonics", cfg.repair.dehum.config.harmonics);
  add_field(params, "repair.dehum.q", cfg.repair.dehum.config.q);
  add_field(params, "repair.dehum.adaptive", cfg.repair.dehum.config.adaptive);
  add_field(params, "repair.dehum.searchRangeHz", cfg.repair.dehum.config.search_range_hz);
  add_field(params, "repair.dehum.adaptation", cfg.repair.dehum.config.adaptation);
  add_field(params, "repair.dehum.frameSize", cfg.repair.dehum.config.frame_size);
  add_field(params, "repair.dehum.pllBandwidth", cfg.repair.dehum.config.pll_bandwidth);
  add_field(params, "repair.dehum.mode", cfg.repair.dehum.config.mode);

  add_field(params, "repair.dereverb.enabled", cfg.repair.dereverb.enabled);
  add_field(params, "repair.dereverb.threshold", cfg.repair.dereverb.config.threshold);
  add_field(params, "repair.dereverb.attenuation", cfg.repair.dereverb.config.attenuation);
  add_field(params, "repair.dereverb.nFft", cfg.repair.dereverb.config.n_fft);
  add_field(params, "repair.dereverb.hopLength", cfg.repair.dereverb.config.hop_length);
  add_field(params, "repair.dereverb.t60Sec", cfg.repair.dereverb.config.t60_sec);
  add_field(params, "repair.dereverb.lateDelayMs", cfg.repair.dereverb.config.late_delay_ms);
  add_field(params, "repair.dereverb.overSubtraction", cfg.repair.dereverb.config.over_subtraction);
  add_field(params, "repair.dereverb.spectralFloor", cfg.repair.dereverb.config.spectral_floor);
  add_field(params, "repair.dereverb.wpeEnabled", cfg.repair.dereverb.config.wpe_enabled);
  add_field(params, "repair.dereverb.wpeIterations", cfg.repair.dereverb.config.wpe_iterations);
  add_field(params, "repair.dereverb.wpeTaps", cfg.repair.dereverb.config.wpe_taps);
  add_field(params, "repair.dereverb.wpeStrength", cfg.repair.dereverb.config.wpe_strength);

  add_field(params, "repair.denoise.enabled", cfg.repair.denoise.enabled);
  add_field(params, "repair.denoise.mode", cfg.repair.denoise.config.mode);
  add_field(params, "repair.denoise.noiseEstimator", cfg.repair.denoise.config.noise_estimator);
  add_field(params, "repair.denoise.nFft", cfg.repair.denoise.config.n_fft);
  add_field(params, "repair.denoise.hopLength", cfg.repair.denoise.config.hop_length);
  add_field(params, "repair.denoise.ddAlpha", cfg.repair.denoise.config.dd_alpha);
  add_field(params, "repair.denoise.reductionDb", cfg.repair.denoise.config.reduction_db);
  add_field(params, "repair.denoise.overSubtraction", cfg.repair.denoise.config.over_subtraction);
  add_field(params, "repair.denoise.spectralFloor", cfg.repair.denoise.config.spectral_floor);
  add_field(params, "repair.denoise.noiseEstimationQuantile",
            cfg.repair.denoise.config.noise_estimation_quantile);
  add_field(params, "repair.denoise.speechPresenceGain",
            cfg.repair.denoise.config.speech_presence_gain);
  add_field(params, "repair.denoise.gainSmoothing", cfg.repair.denoise.config.gain_smoothing);

  add_field(params, "eq.tilt.enabled", cfg.eq.tilt.enabled);
#define X(key, member, meta) add_field(params, "eq.tilt." key, cfg.eq.tilt.member);
  SONARE_FIELDS_EQ_TILT(X)
#undef X

  add_field(params, "dynamics.deesser.enabled", cfg.dynamics.deesser.enabled);
#define X(key, member, meta) \
  add_field(params, "dynamics.deesser." key, cfg.dynamics.deesser.config.member);
  SONARE_FIELDS_DEESSER(X)
#undef X

  add_field(params, "dynamics.transientShaper.enabled", cfg.dynamics.transient_shaper.enabled);
#define X(key, member, meta) \
  add_field(params, "dynamics.transientShaper." key, cfg.dynamics.transient_shaper.config.member);
  SONARE_FIELDS_TRANSIENT_SHAPER(X)
#undef X

  add_field(params, "dynamics.compressor.enabled", cfg.dynamics.compressor.enabled);
#define X(key, member, meta) \
  add_field(params, "dynamics.compressor." key, cfg.dynamics.compressor.config.member);
  SONARE_FIELDS_COMPRESSOR_BEFORE_HIGH_PASS(X)
  if (cfg.dynamics.compressor.config.sidechain_high_pass_hz > 0.0f) {
    add_field(params, "dynamics.compressor.sidechainHighPassHz",
              cfg.dynamics.compressor.config.sidechain_high_pass_hz);
  }
  SONARE_FIELDS_COMPRESSOR_AFTER_HIGH_PASS(X)
#undef X

  add_field(params, "saturation.tape.enabled", cfg.saturation.tape.enabled);
#define X(key, member, meta) \
  add_field(params, "saturation.tape." key, cfg.saturation.tape.config.member);
  SONARE_FIELDS_TAPE(X)
#undef X

  add_field(params, "saturation.exciter.enabled", cfg.saturation.exciter.enabled);
#define X(key, member, meta) \
  add_field(params, "saturation.exciter." key, cfg.saturation.exciter.config.member);
  SONARE_FIELDS_EXCITER(X)
#undef X

  add_field(params, "spectral.airBand.enabled", cfg.spectral.air_band.enabled);
#define X(key, member, meta) \
  add_field(params, "spectral.airBand." key, cfg.spectral.air_band.config.member);
  SONARE_FIELDS_AIR_BAND(X)
#undef X

  add_field(params, "stereo.imager.enabled", cfg.stereo.imager.enabled);
#define X(key, member, meta) \
  add_field(params, "stereo.imager." key, cfg.stereo.imager.config.member);
  SONARE_FIELDS_IMAGER(X)
#undef X

  add_field(params, "stereo.monoMaker.enabled", cfg.stereo.mono_maker.enabled);
#define X(key, member, meta) \
  add_field(params, "stereo.monoMaker." key, cfg.stereo.mono_maker.config.member);
  SONARE_FIELDS_MONO_MAKER(X)
#undef X

  add_field(params, "maximizer.truePeakLimiter.enabled", cfg.maximizer.true_peak_limiter.enabled);
#define X(key, member, meta)                          \
  add_field(params, "maximizer.truePeakLimiter." key, \
            cfg.maximizer.true_peak_limiter.config.member);
  SONARE_FIELDS_TRUE_PEAK_LIMITER(X)
#undef X

  add_field(params, "loudness.enabled", cfg.loudness.enabled);
#define X(key, member, meta) add_field(params, "loudness." key, cfg.loudness.member);
  SONARE_FIELDS_LOUDNESS(X)
#undef X

  return params;
}

// A chain document is tiny in normal use (even a 64-band v2 document is only
// a few dozen KiB), so cap the encoded input before building the generic JSON
// tree.  This keeps the existing strict parser/API intact while ensuring a
// caller cannot make this convenience API retain an arbitrarily large string.
constexpr std::size_t kMaxChainJsonBytes = 1u * 1024u * 1024u;

// Node and string-byte axes take the project-import ceiling; the byte axis
// stays kMaxChainJsonBytes.
constexpr sonare::resource::ProjectImportResourceLimits kChainJsonLimits{
    kMaxChainJsonBytes,
    sonare::resource::kDefaultProjectImportResourceLimits.max_json_nodes,
    sonare::resource::kDefaultProjectImportResourceLimits.max_entities,
    sonare::resource::kDefaultProjectImportResourceLimits.max_string_bytes,
    sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes,
};

[[noreturn]] void throw_invalid_json(const std::string& message) {
  throw SonareException(ErrorCode::InvalidFormat, message);
}

constexpr int kChainDocumentVersion = 2;
constexpr const char* kParamsKey = "params";

// `dynamics.multibandComp.cutoff<i>Hz` and `...crossover.cutoffsHz.<i>` name one value.
std::string duplicate_identity(const std::string& path) {
  std::string storage;
  std::string canonical = canonical_chain_param_key(path, &storage);
  constexpr std::string_view kScalarCutoff = "dynamics.multibandComp.cutoff";
  if (canonical.size() > kScalarCutoff.size() + 2 &&
      canonical.compare(0, kScalarCutoff.size(), kScalarCutoff) == 0 &&
      canonical.compare(canonical.size() - 2, 2, "Hz") == 0) {
    const std::string digits =
        canonical.substr(kScalarCutoff.size(), canonical.size() - kScalarCutoff.size() - 2);
    if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
      return "dynamics.multibandComp.crossover.cutoffsHz." + digits;
    }
  }
  return canonical;
}

// Walks a `params` block into flat leaves: object members as `.<key>`, array elements as
// `.<index>`, so a dotted key and the nested spelling of the same path are one parameter.
class ParamsWalker {
 public:
  std::vector<ChainFlatParam> walk(const JsonValue& params) {
    for (const auto& [key, value] : params.as_object()) {
      if (sonare::util::json::is_annotation_key(key)) continue;
      append(key, value);
    }
    return std::move(out_);
  }

 private:
  void append(const std::string& path, const JsonValue& value) {
    if (value.is_object()) {
      for (const auto& [key, member] : value.as_object()) {
        if (sonare::util::json::is_annotation_key(key)) continue;
        append(schema::child_path(path, key), member);
      }
      return;
    }
    if (value.is_array()) {
      const auto& elements = value.as_array();
      if (elements.empty()) {
        throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path,
                                                "must not be an empty list");
      }
      for (std::size_t index = 0; index < elements.size(); ++index) {
        append(schema::catalog_index_path(path, index), elements[index]);
      }
      return;
    }
    if (!seen_.insert(duplicate_identity(path)).second) {
      throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, path,
                                               "must not be given twice");
    }
    const std::optional<std::string> names = mastering_enum_names("", path);
    if (value.is_null()) {
      throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, path,
                                               "must be a number, a boolean or a token");
    }
    if (value.is_string()) {
      if (!names) {
        throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, path,
                                                 "must be a number or a boolean");
      }
      out_.push_back(
          ChainFlatParam{path, *mastering_enum_value("", path, value.as_string()), false});
      return;
    }
    if (names) {
      throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, path,
                                               "must be one of " + *names);
    }
    if (value.is_bool()) {
      out_.push_back(ChainFlatParam{path, value.as_bool() ? 1.0 : 0.0, true});
      return;
    }
    // The declip threshold's dB domain, refused enabled or not (repair::clip_threshold_from_db).
    if (path == "repair.declip.clipThresholdDb" &&
        !(value.as_number() >= kMinPositiveGainDb && value.as_number() <= 0.0)) {
      throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path,
                                              schema::range_predicate(kMinPositiveGainDb, 0.0));
    }
    out_.push_back(ChainFlatParam{path, value.as_number(), false});
  }

  std::vector<ChainFlatParam> out_;
  std::set<std::string> seen_;
};

// A refusal from the params walker or the flat-param parser, rooted and coded for @p origin.
[[noreturn]] void refuse_from(const ChainParamsOrigin& origin, const SonareParameterRefusal& e) {
  if (e.code() != ErrorCode::InvalidParameter) throw e;
  schema::rethrow_under(e, origin.root, origin.code);
}

[[noreturn]] void refuse_from(const ChainParamsOrigin& origin, const SonareException& e) {
  if (e.code() != ErrorCode::InvalidParameter) throw e;
  throw SonareException(origin.code, e.what());
}

// The flat leaves of a chain document's params block, refused as @p origin reads them.
std::vector<ChainFlatParam> read_chain_document(const std::string& text,
                                                const ChainParamsOrigin& origin) {
  try {
    const auto root = sonare::util::json::admit_strict(text, kChainJsonLimits);
    if (!root.is_object()) throw_invalid_json(schema::root_must_be_object());
    for (const auto& [key, value] : root.as_object()) {
      if (key == "version" || key == "params" || sonare::util::json::is_annotation_key(key)) {
        continue;
      }
      throw_invalid_json(schema::unknown_key(schema::child_path("", key)));
    }
    if (const JsonValue* version = root.find("version")) {
      if (!version->is_number() || version->as_number() != kChainDocumentVersion) {
        const std::string refusal = schema::refusal("version", "must be 2, or left out");
        if (version->is_number() && std::floor(version->as_number()) == version->as_number()) {
          throw SonareException(ErrorCode::InvalidFormat, refusal,
                                ErrorReason::UnsupportedSchemaVersion);
        }
        throw_invalid_json(refusal);
      }
    }
    const JsonValue* params = root.find("params");
    if (params == nullptr) throw_invalid_json(schema::missing_key(kParamsKey));
    if (!params->is_object()) {
      throw_invalid_json(schema::must_be(kParamsKey, schema::Kind::Object));
    }
    return ParamsWalker{}.walk(*params);
  } catch (const sonare::util::json::JsonResourceError& e) {
    throw SonareException(ErrorCode::InvalidFormat, e.what());
  } catch (const sonare::util::json::JsonError& e) {
    throw SonareException(ErrorCode::InvalidFormat, e.what(), ErrorReason::MalformedJson);
  } catch (const SonareParameterRefusal& e) {
    refuse_from(origin, e);
  } catch (const SonareException& e) {
    refuse_from(origin, e);
  }
}

}  // namespace

std::vector<ChainFlatParam> chain_config_flat_params(const std::string& chain_config_json) {
  return read_chain_document(chain_config_json, kChainDocument);
}

std::optional<double> chain_config_parameter_value(const MasteringChainConfig& config,
                                                   const std::string& key) {
  std::string canonical_storage;
  const std::string& canonical = canonical_chain_param_key(key, &canonical_storage);
  // Multiband fields are read from the struct, so every band and cutoff index is reachable.
  if (canonical.rfind("dynamics.multibandComp.", 0) == 0) {
    return multiband_parameter_value(config.dynamics.multiband_comp, canonical);
  }
  // Shares the writer's flat table, but skips document validation.
  const auto params = build_chain_params(config);
  const auto it = params.find(canonical);
  if (it == params.end()) return std::nullopt;
  if (it->second.is_bool()) return it->second.as_bool() ? 1.0 : 0.0;
  if (it->second.is_string()) return mastering_enum_value("", canonical, it->second.as_string());
  return it->second.as_number();
}

std::string chain_config_to_json(const MasteringChainConfig& config) {
  // Validated here so the writer never emits a document chain_config_from_json refuses.
  validate_chain_multiband_config(config.dynamics.multiband_comp.config);
  sonare::util::json::Object root;
  util::json::put(root, "version", kChainDocumentVersion);
  auto params = build_chain_params(config);
  util::json::put(params, "dynamics.multibandComp", build_multiband_params(config));
  util::json::put(root, "params", std::move(params));
  return sonare::util::json::dump(JsonValue(std::move(root)));
}

namespace {

std::vector<Param> document_params(const std::string& document, const ChainParamsOrigin& origin) {
  const std::vector<ChainFlatParam> flat = read_chain_document(document, origin);
  std::vector<Param> params;
  params.reserve(flat.size());
  for (const ChainFlatParam& leaf : flat) params.push_back(Param{leaf.key, leaf.value});
  return params;
}

// The flat-param parser and the config validator are shared with the key/value and struct entry
// points, where a refusal is the caller's argument. Here the values come from @p origin.
template <typename Build>
MasteringChainConfig from_document(const ChainParamsOrigin& origin, Build&& build) {
  try {
    MasteringChainConfig config = build();
    validate_mastering_chain_config(config);
    return config;
  } catch (const SonareParameterRefusal& e) {
    refuse_from(origin, e);
  } catch (const SonareException& e) {
    refuse_from(origin, e);
  }
}

}  // namespace

std::string chain_params_document(const std::string& params_json, std::string_view root) {
  JsonValue params;
  try {
    params = sonare::util::json::admit_strict(params_json, kChainJsonLimits);
  } catch (const sonare::util::json::JsonError& e) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, std::string(root),
                                            std::string("must be valid JSON: ") + e.what());
  }
  if (!params.is_object()) {
    throw SonareParameterRefusal::wrong_type(
        ErrorCode::InvalidParameter, std::string(root),
        std::string("must be ") + schema::kind_text(schema::Kind::Object));
  }
  sonare::util::json::Object document;
  util::json::put(document, "params", std::move(params));
  return sonare::util::json::dump(JsonValue(std::move(document)));
}

MasteringChainConfig chain_config_from_json(const std::string& json,
                                            const ChainParamsOrigin& origin) {
  const std::vector<Param> params = document_params(json, origin);
  return from_document(origin,
                       [&] { return parse_chain_config_params(params.data(), params.size()); });
}

MasteringChainConfig streaming_chain_config_from_json(const std::string& json,
                                                      const ChainParamsOrigin& origin) {
  const std::vector<Param> params = document_params(json, origin);
  return from_document(
      origin, [&] { return parse_streaming_chain_config_params(params.data(), params.size()); });
}

MasteringChainConfig chain_config_overrides_from_json(MasteringChainConfig base,
                                                      const std::string& json,
                                                      const ChainParamsOrigin& origin) {
  const std::vector<Param> params = document_params(json, origin);
  return from_document(origin, [&] {
    apply_chain_config_overrides(base, params.data(), params.size());
    return std::move(base);
  });
}

}  // namespace sonare::mastering::api
