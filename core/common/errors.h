#ifndef VP_ERRORS_H
#define VP_ERRORS_H

/* ── Error codes ── */
typedef enum {
    VP_OK = 0,
    VP_ERR_INVALID_ARG,
    VP_ERR_OUT_OF_MEMORY,
    VP_ERR_FILE_NOT_FOUND,
    VP_ERR_FILE_CORRUPT,
    VP_ERR_CODEC_UNSUPPORTED,
    VP_ERR_DECODE_FAILED,
    VP_ERR_MODEL_LOAD_FAILED,
    VP_ERR_INFERENCE_FAILED,
    VP_ERR_BACKEND_INIT_FAILED,
    VP_ERR_SHAPE_MISMATCH,
    VP_ERR_PREPROCESSING_FAILED,
    VP_ERR_POSTPROCESSING_FAILED,
    VP_ERR_TRACKING_FAILED,
    VP_ERR_EXPORT_FAILED,
    VP_ERR_DATABASE_FAILED,
    VP_ERR_QUEUE_FAILED,
    VP_ERR_TIMEOUT,
    VP_ERR_CANCELLED,
    VP_ERR_INTERNAL
} VPError;

static inline const char *vp_error_str(VPError err) {
    switch (err) {
        case VP_OK:                       return "OK";
        case VP_ERR_INVALID_ARG:          return "Invalid argument";
        case VP_ERR_OUT_OF_MEMORY:        return "Out of memory";
        case VP_ERR_FILE_NOT_FOUND:       return "File not found";
        case VP_ERR_FILE_CORRUPT:         return "File corrupt or unreadable";
        case VP_ERR_CODEC_UNSUPPORTED:    return "Codec not supported";
        case VP_ERR_DECODE_FAILED:        return "Frame decode failed";
        case VP_ERR_MODEL_LOAD_FAILED:    return "Model load failed";
        case VP_ERR_INFERENCE_FAILED:     return "Inference failed";
        case VP_ERR_BACKEND_INIT_FAILED:  return "Backend init failed";
        case VP_ERR_SHAPE_MISMATCH:       return "Tensor shape mismatch";
        case VP_ERR_PREPROCESSING_FAILED: return "Preprocessing failed";
        case VP_ERR_POSTPROCESSING_FAILED:return "Postprocessing failed";
        case VP_ERR_TRACKING_FAILED:      return "Tracking failed";
        case VP_ERR_EXPORT_FAILED:        return "Export failed";
        case VP_ERR_DATABASE_FAILED:      return "Database operation failed";
        case VP_ERR_QUEUE_FAILED:         return "Queue operation failed";
        case VP_ERR_TIMEOUT:              return "Operation timed out";
        case VP_ERR_CANCELLED:            return "Operation cancelled";
        case VP_ERR_INTERNAL:             return "Internal error";
    }
    return "Unknown error";
}

#endif /* VP_ERRORS_H */
