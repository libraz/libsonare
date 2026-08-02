#include "c_api/features_internal.h"
#include "feature/segment.h"

namespace {

bool valid_matrix(const float* values, int rows, int cols) {
  return values != nullptr && rows > 0 && cols > 0 &&
         static_cast<size_t>(rows) <= sonare_c_detail::kMaxBufferSize / static_cast<size_t>(cols);
}

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
  if (!out || !valid_matrix(x, x_rows, x_cols) || !valid_matrix(y, y_rows, y_cols) ||
      x_rows != y_rows || k < 0 || !metric || !mode)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
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
  if (!out || !valid_matrix(data, rows, cols) || k < 0 || width < 0 || (sym != 0 && sym != 1) ||
      !metric || !mode)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  SONARE_C_TRY
  return fill_matrix(recurrence_matrix(data, rows, cols, k, width, sym != 0, metric, mode), cols,
                     cols, out);
  SONARE_C_CATCH
}

SonareError sonare_segment_recurrence_to_lag(const float* recurrence, int n, int pad,
                                             SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (!out || !valid_matrix(recurrence, n, n) || (pad != 0 && pad != 1)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = {};
  SONARE_C_TRY
  return fill_matrix(recurrence_to_lag(recurrence, n, pad != 0), n, pad ? 2 * n - 1 : n, out);
  SONARE_C_CATCH
}

SonareError sonare_segment_lag_to_recurrence(const float* lag, int n_rows, int n_lags,
                                             SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (!out || !valid_matrix(lag, n_rows, n_lags)) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  SONARE_C_TRY
  return fill_matrix(lag_to_recurrence(lag, n_rows, n_lags), n_rows, n_rows, out);
  SONARE_C_CATCH
}

SonareError sonare_segment_subsegment(const float* data, int rows, int cols, const int* boundaries,
                                      size_t boundary_count, int n_segments,
                                      SonareSegmentIndices* out) {
  SONARE_C_API_ENTRY;
  if (!out || !valid_matrix(data, rows, cols) || (!boundaries && boundary_count > 0) ||
      n_segments <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  SONARE_C_TRY
  std::vector<int> points;
  if (boundary_count) points.assign(boundaries, boundaries + boundary_count);
  return fill_indices(subsegment(data, rows, cols, points, n_segments), out);
  SONARE_C_CATCH
}

SonareError sonare_segment_agglomerative(const float* data, int rows, int cols, int k,
                                         const char* linkage, SonareSegmentIndices* out) {
  SONARE_C_API_ENTRY;
  if (!out || !valid_matrix(data, rows, cols) || k <= 0 || !linkage) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = {};
  SONARE_C_TRY
  return fill_indices(agglomerative(data, rows, cols, k, linkage), out);
  SONARE_C_CATCH
}

SonareError sonare_segment_path_enhance(const float* recurrence, int n, int win, int max_ratio,
                                        int min_ratio, int n_filters, SonareSegmentMatrix* out) {
  SONARE_C_API_ENTRY;
  if (!out || !valid_matrix(recurrence, n, n) || win <= 0 || max_ratio <= 0 || min_ratio < 0 ||
      n_filters <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
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
