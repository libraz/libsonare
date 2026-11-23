#include <sonare/sonare_c.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "c_api/sonare_c_internal.h"
#include "core/audio_encode.h"
#include "core/audio_tags.h"
#include "util/parameter_checks.h"

using namespace sonare_c_detail;

namespace {

const std::array<CName, 3> kEncodeOptionNames{{{"format", "options.format"},
                                               {"bit_depth", "options.bit_depth"},
                                               {"quality", "options.quality"}}};

sonare::AudioTags to_audio_tags(const SonareAudioTags* tags) {
  sonare::AudioTags result;
  if (tags == nullptr || tags->count == 0) return result;
  if (tags->keys == nullptr) {
    sonare::util::check::refuse("tags.keys", "must be non-null when tags.count is positive");
  }
  if (tags->values == nullptr) {
    sonare::util::check::refuse("tags.values", "must be non-null when tags.count is positive");
  }
  for (size_t i = 0; i < tags->count; ++i) {
    const std::string subject = "tags.keys[" + std::to_string(i) + "]";
    if (tags->keys[i] == nullptr) {
      sonare::util::check::refuse(subject.c_str(), "must be non-null");
    }
    if (tags->keys[i][0] == '\0') {
      sonare::util::check::refuse(subject.c_str(), sonare::util::refusal::kNonEmptyPredicate);
    }
    if (tags->values[i] == nullptr) {
      sonare::util::check::refuse(("tags.values[" + std::to_string(i) + "]").c_str(),
                                  "must be non-null");
    }
    result[tags->keys[i]] = tags->values[i];
  }
  return result;
}

}  // namespace

void sonare_free_audio_tags(SonareAudioTags* tags) {
  if (tags == nullptr) return;
  if (tags->keys != nullptr) {
    for (size_t i = 0; i < tags->count; ++i) delete[] tags->keys[i];
    delete[] tags->keys;
  }
  if (tags->values != nullptr) {
    for (size_t i = 0; i < tags->count; ++i) delete[] tags->values[i];
    delete[] tags->values;
  }
  *tags = {};
}

SonareError sonare_read_audio_tags(const uint8_t* data, size_t size, SonareAudioTags* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(data, size);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  const sonare::AudioTags tags = sonare::read_audio_tags(data, size);
  StagedOutput<SonareAudioTags, &sonare_free_audio_tags> staged;
  if (!tags.empty()) {
    staged->keys = new char* [tags.size()] {};
    staged->values = new char* [tags.size()] {};
    staged->count = tags.size();
    size_t index = 0;
    for (const auto& [key, value] : tags) {
      staged->keys[index] = copy_string(key);
      staged->values[index] = copy_string(value);
      ++index;
    }
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_encode_options_init(SonareEncodeOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->format = "wav";
  options->quality = std::numeric_limits<float>::quiet_NaN();
}

SonareError sonare_encode_audio(const float* samples, size_t frames, int num_channels,
                                int sample_rate, const SonareEncodeOptions* options, uint8_t** out,
                                size_t* out_size) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_size);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_size);

  SONARE_C_TRY
  SonareEncodeOptions defaults;
  sonare_encode_options_init(&defaults);
  const SonareEncodeOptions& resolved = options != nullptr ? *options : defaults;
  require_struct_size(resolved.struct_size, sizeof(SonareEncodeOptions), "options",
                      "sonare_encode_options_init");
  if (resolved.format == nullptr) {
    sonare::util::check::refuse("options.format", "must be non-null");
  }

  sonare::AudioEncodeOptions cpp_options;
  cpp_options.format = resolved.format;
  if (resolved.bit_depth != 0) cpp_options.bit_depth = resolved.bit_depth;
  if (!std::isnan(resolved.quality)) cpp_options.quality = resolved.quality;
  cpp_options.tags = to_audio_tags(resolved.tags);

  const std::vector<uint8_t> encoded = with_c_names(kEncodeOptionNames, [&] {
    return sonare::encode_audio(samples, frames, num_channels, sample_rate, cpp_options);
  });
  std::unique_ptr<uint8_t[]> staged(copy_vector(encoded));
  *out_size = encoded.size();
  *out = staged.release();
  return SONARE_OK;
  SONARE_C_CATCH
}
