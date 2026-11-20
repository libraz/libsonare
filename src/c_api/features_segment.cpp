#include <array>

#include "c_api/features_internal.h"
#include "feature/segment.h"
#include "util/parameter_checks.h"

namespace {

namespace check = sonare::util::check;

// Refuses a row-major matrix argument by pointer, then each extent, then their product.
SonareError refuse_matrix(const float* values, const char* values_name, int rows,
                          const char* rows_name, int cols, const char* cols_name) {
  if (values == nullptr) return sonare_c_detail::refuse_null(values_name);
  return run_validation([&] {
    check::integer_at_least(rows, 1, rows_name);
    check::integer_at_least(cols, 1, cols_name);
    if (static_cast<size_t>(rows) > sonare_c_detail::kMaxBufferSize / static_cast<size_t>(cols)) {
      check::refuse_relation(
          {rows_name, cols_name},
          "{0} * {1} must be at most " + sonare::util::refusal::bound_text(
                                             static_cast<double>(sonare_c_detail::kMaxBufferSize)));
    }
  });
}

constexpr std::array<int, 2> kFlagValues{0, 1};

SonareError fill_matrix(std::vector<float> values, int rows, int cols, SonareSegmentMatrix* out) {
  if (rows <= 0 || cols <= 0 || values.size() != static_cast<size_t>(rows) * cols) {
    return SONARE_ERROR_UNKNOWN;
  }
  std::unique_ptr<float[]> copy(new float[values.size()]);
  std::memcpy(copy.get(), values.data(), values.size() * sizeof(float));
  out->rows = rows;
  out->cols = cols;
  out->values = release_array(copy);
  return SONARE_OK;
}

SonareError fill_indices(std::vector<int> values, SonareSegmentIndices* out) {
  std::unique_ptr<int[]> copy(values.empty() ? nullptr : new int[values.size()]);
  if (!values.empty()) std::memcpy(copy.get(), values.data(), values.size() * sizeof(int));
  out->values = release_array(copy);
  out->count = values.size();
  return SONARE_OK;
}

}  // namespace

SonareError sonare_segment_cross_similarity(const float* x, int x_rows, int x_cols, const float* y,
                                            int y_rows, int y_cols, int k, const char* metric,
                                            const char* mode, SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  if (const SonareError err = refuse_matrix(x, "x", x_rows, "x_rows", x_cols, "x_cols");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = refuse_matrix(y, "y", y_rows, "y_rows", y_cols, "y_cols");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        if (x_rows != y_rows) check::refuse_relation({"y_rows", "x_rows"}, "{0} must equal {1}");
        check::integer_at_least(k, 0, "k");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(metric);
  SONARE_C_REQUIRE_NONNULL(mode);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const std::string metric_name(metric);
  const std::string mode_name(mode);
  const int rows = mode_name == "affinity" ? y_cols : x_cols;
  const int cols = mode_name == "affinity" ? x_cols : y_cols;
  return fill_matrix(
      cross_similarity(x, x_rows, x_cols, y, y_rows, y_cols, k, metric_name, mode_name), rows, cols,
      out);
  SONARE_C_CATCH
}

SonareError sonare_segment_recurrence_matrix(const float* data, int rows, int cols, int k,
                                             int width, int sym, const char* metric,
                                             const char* mode, SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  if (const SonareError err = refuse_matrix(data, "data", rows, "rows", cols, "cols");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        check::integer_at_least(k, 0, "k");
        check::integer_at_least(width, 0, "width");
        check::one_of(sym, kFlagValues, "sym");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(metric);
  SONARE_C_REQUIRE_NONNULL(mode);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_matrix(recurrence_matrix(data, rows, cols, k, width, sym != 0, metric, mode), cols,
                     cols, out);
  SONARE_C_CATCH
}

SonareError sonare_segment_recurrence_to_lag(const float* recurrence, int n, int pad,
                                             SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  if (const SonareError err = refuse_matrix(recurrence, "recurrence", n, "n", n, "n");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { check::one_of(pad, kFlagValues, "pad"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_matrix(recurrence_to_lag(recurrence, n, pad != 0), pad ? 2 * n : n, n, out);
  SONARE_C_CATCH
}

SonareError sonare_segment_lag_to_recurrence(const float* lag, int n_rows, int n_lags,
                                             SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  // The product is symmetric, so the extents are refused in parameter order.
  if (const SonareError err = refuse_matrix(lag, "lag", n_rows, "n_rows", n_lags, "n_lags");
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_matrix(lag_to_recurrence(lag, n_rows, n_lags), n_rows, n_rows, out);
  SONARE_C_CATCH
}

SonareError sonare_segment_subsegment(const float* data, int rows, int cols, const int* boundaries,
                                      size_t boundary_count, int n_segments,
                                      SonareSegmentIndices* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  if (const SonareError err = refuse_matrix(data, "data", rows, "rows", cols, "cols");
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(boundaries, boundary_count);
  if (const SonareError err =
          run_validation([&] { check::integer_at_least(n_segments, 1, "n_segments"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  std::vector<int> points;
  if (boundary_count) points.assign(boundaries, boundaries + boundary_count);
  return fill_indices(subsegment(data, rows, cols, points, n_segments), out);
  SONARE_C_CATCH
}

SonareError sonare_segment_agglomerative(const float* data, int rows, int cols, int k,
                                         const char* linkage, SonareSegmentIndices* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  if (const SonareError err = refuse_matrix(data, "data", rows, "rows", cols, "cols");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { check::integer_at_least(k, 1, "k"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(linkage);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_indices(agglomerative(data, rows, cols, k, linkage), out);
  SONARE_C_CATCH
}

SonareError sonare_segment_path_enhance(const float* recurrence, int n, int win, int max_ratio,
                                        int min_ratio, int n_filters, SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
  if (const SonareError err = refuse_matrix(recurrence, "recurrence", n, "n", n, "n");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        check::integer_at_least(win, 1, "win");
        check::integer_at_least(max_ratio, 1, "max_ratio");
        check::integer_at_least(min_ratio, 0, "min_ratio");
        check::integer_at_least(n_filters, 1, "n_filters");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  return fill_matrix(path_enhance(recurrence, n, win, max_ratio, min_ratio, n_filters), n, n, out);
  SONARE_C_CATCH
}

void sonare_free_segment_matrix(SonareSegmentMatrix* result) {
  if (!result) return;
  delete[] result->values;
  *result = {};
}

void sonare_free_segment_indices(SonareSegmentIndices* result) {
  if (!result) return;
  delete[] result->values;
  *result = {};
}
