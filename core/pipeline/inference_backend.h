#ifndef VP_INFERENCE_BACKEND_H
#define VP_INFERENCE_BACKEND_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include <stdlib.h>

/*
 * Uniform inference backend interface.
 * The pipeline does NOT know which engine runs underneath.
 * Each backend implements these function pointers.
 */

typedef struct InferenceBackend InferenceBackend;

typedef struct {
    int (*init)(InferenceBackend *backend, const char *model_path, void *config);
    int (*infer)(InferenceBackend *backend, const float *input, float *output);
    int (*get_input_shape)(InferenceBackend *backend, int *shape, int max_dims);
    int (*get_output_shape)(InferenceBackend *backend, int *shape, int max_dims);
    size_t (*get_output_size)(InferenceBackend *backend);
    void (*destroy)(InferenceBackend *backend);
} InferenceBackendOps;

struct InferenceBackend {
    InferenceBackendOps ops;
    EngineInfo          info;
    void               *state;   /* backend-specific opaque state */
    bool                ready;
};

/* Factory: create backend by name ("onnx", "tensorrt", "openvino"). */
InferenceBackend *inference_backend_create(const char *backend_type);

/* Convenience wrappers */
static inline VPError inference_init(InferenceBackend *b, const char *model, void *cfg) {
    if (!b || !b->ops.init) return VP_ERR_BACKEND_INIT_FAILED;
    return b->ops.init(b, model, cfg) == 0 ? VP_OK : VP_ERR_BACKEND_INIT_FAILED;
}

static inline VPError inference_run(InferenceBackend *b, const float *in, float *out) {
    if (!b || !b->ops.infer) return VP_ERR_INFERENCE_FAILED;
    return b->ops.infer(b, in, out) == 0 ? VP_OK : VP_ERR_INFERENCE_FAILED;
}

static inline void inference_get_input_shape(InferenceBackend *b, int *shape, int max) {
    if (b && b->ops.get_input_shape) b->ops.get_input_shape(b, shape, max);
}

static inline void inference_get_output_shape(InferenceBackend *b, int *shape, int max) {
    if (b && b->ops.get_output_shape) b->ops.get_output_shape(b, shape, max);
}

static inline size_t inference_get_output_size(InferenceBackend *b) {
    if (b && b->ops.get_output_size) return b->ops.get_output_size(b);
    return 0;
}

static inline void inference_destroy(InferenceBackend *b) {
    if (b) {
        if (b->ops.destroy) b->ops.destroy(b);
        free(b);
    }
}

#endif /* VP_INFERENCE_BACKEND_H */
