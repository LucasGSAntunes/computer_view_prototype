#include "backend_onnx.h"
#include "core/common/logging.h"

#ifdef VP_HAS_ONNX
#include <onnxruntime_c_api.h>
#endif

#include <stdlib.h>
#include <string.h>

typedef struct {
#ifdef VP_HAS_ONNX
    const OrtApi       *api;
    OrtEnv             *env;
    OrtSessionOptions  *session_opts;
    OrtSession         *session;
    OrtMemoryInfo      *memory_info;
    char               *input_name;
    char               *output_name;
#endif
    int    input_shape[4];    /* NCHW */
    int    output_shape[3];   /* [1, num_outputs, detection_dim] or similar */
    size_t output_size;       /* total output floats */
} OnnxState;

#ifdef VP_HAS_ONNX

static int onnx_init(InferenceBackend *backend, const char *model_path, void *config) {
    (void)config;
    OnnxState *st = calloc(1, sizeof(OnnxState));
    if (!st) return -1;
    backend->state = st;

    st->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!st->api) {
        LOG_ERROR(NULL, "onnx", "Failed to get ONNX Runtime API");
        return -1;
    }

    OrtStatus *status = st->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "vp_onnx", &st->env);
    if (status) {
        LOG_ERROR(NULL, "onnx", "CreateEnv failed: %s", st->api->GetErrorMessage(status));
        st->api->ReleaseStatus(status);
        return -1;
    }

    status = st->api->CreateSessionOptions(&st->session_opts);
    if (status) { st->api->ReleaseStatus(status); return -1; }

    st->api->SetIntraOpNumThreads(st->session_opts, 4);
    st->api->SetSessionGraphOptimizationLevel(st->session_opts, ORT_ENABLE_ALL);

    status = st->api->CreateSession(st->env, model_path, st->session_opts, &st->session);
    if (status) {
        LOG_ERROR(NULL, "onnx", "CreateSession failed: %s", st->api->GetErrorMessage(status));
        st->api->ReleaseStatus(status);
        return -1;
    }

    /* Get input info */
    OrtAllocator *allocator;
    st->api->GetAllocatorWithDefaultOptions(&allocator);

    char *in_name = NULL;
    st->api->SessionGetInputName(st->session, 0, allocator, &in_name);
    st->input_name = strdup(in_name);
    allocator->Free(allocator, in_name);

    OrtTypeInfo *in_type_info;
    st->api->SessionGetInputTypeInfo(st->session, 0, &in_type_info);
    const OrtTensorTypeAndShapeInfo *in_tensor_info;
    st->api->CastTypeInfoToTensorInfo(in_type_info, &in_tensor_info);

    size_t in_ndims;
    st->api->GetDimensionsCount(in_tensor_info, &in_ndims);
    int64_t in_dims[4];
    st->api->GetDimensions(in_tensor_info, in_dims, in_ndims);
    for (size_t i = 0; i < in_ndims && i < 4; i++) {
        st->input_shape[i] = (int)(in_dims[i] > 0 ? in_dims[i] : 1);
    }
    st->api->ReleaseTypeInfo(in_type_info);

    /* Get output info */
    char *out_name = NULL;
    st->api->SessionGetOutputName(st->session, 0, allocator, &out_name);
    st->output_name = strdup(out_name);
    allocator->Free(allocator, out_name);

    OrtTypeInfo *out_type_info;
    st->api->SessionGetOutputTypeInfo(st->session, 0, &out_type_info);
    const OrtTensorTypeAndShapeInfo *out_tensor_info;
    st->api->CastTypeInfoToTensorInfo(out_type_info, &out_tensor_info);

    size_t out_ndims;
    st->api->GetDimensionsCount(out_tensor_info, &out_ndims);
    int64_t out_dims[4];
    st->api->GetDimensions(out_tensor_info, out_dims, out_ndims);

    st->output_size = 1;
    for (size_t i = 0; i < out_ndims && i < 3; i++) {
        st->output_shape[i] = (int)(out_dims[i] > 0 ? out_dims[i] : 1);
        st->output_size *= (size_t)st->output_shape[i];
    }
    st->api->ReleaseTypeInfo(out_type_info);

    /* Memory info */
    st->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &st->memory_info);

    /* Fill engine info */
    strncpy(backend->info.engine_name, "ONNX Runtime", sizeof(backend->info.engine_name) - 1);
    strncpy(backend->info.backend_type, "onnx", sizeof(backend->info.backend_type) - 1);
    strncpy(backend->info.device_type, "cpu", sizeof(backend->info.device_type) - 1);
    strncpy(backend->info.precision_mode, "fp32", sizeof(backend->info.precision_mode) - 1);

    backend->ready = true;
    LOG_INFO(NULL, "onnx", "Model loaded: input=[%d,%d,%d,%d] output_size=%zu",
             st->input_shape[0], st->input_shape[1], st->input_shape[2], st->input_shape[3],
             st->output_size);
    return 0;
}

static int onnx_infer(InferenceBackend *backend, const float *input, float *output) {
    OnnxState *st = backend->state;

    /* Create input tensor */
    int64_t in_shape[4] = {
        st->input_shape[0], st->input_shape[1],
        st->input_shape[2], st->input_shape[3]
    };
    size_t in_size = 1;
    for (int i = 0; i < 4; i++) in_size *= (size_t)in_shape[i];

    OrtValue *input_tensor = NULL;
    OrtStatus *status = st->api->CreateTensorWithDataAsOrtValue(
        st->memory_info, (void *)input, in_size * sizeof(float),
        in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor);
    if (status) {
        LOG_ERROR(NULL, "onnx", "CreateTensor failed: %s", st->api->GetErrorMessage(status));
        st->api->ReleaseStatus(status);
        return -1;
    }

    /* Run inference */
    const char *input_names[]  = { st->input_name };
    const char *output_names[] = { st->output_name };
    OrtValue *output_tensor = NULL;

    status = st->api->Run(st->session, NULL,
                          input_names, (const OrtValue *const *)&input_tensor, 1,
                          output_names, 1, &output_tensor);
    st->api->ReleaseValue(input_tensor);

    if (status) {
        LOG_ERROR(NULL, "onnx", "Run failed: %s", st->api->GetErrorMessage(status));
        st->api->ReleaseStatus(status);
        return -1;
    }

    /* Copy output */
    float *out_data = NULL;
    st->api->GetTensorMutableData(output_tensor, (void **)&out_data);
    memcpy(output, out_data, st->output_size * sizeof(float));
    st->api->ReleaseValue(output_tensor);

    return 0;
}

static int onnx_get_input_shape(InferenceBackend *backend, int *shape, int max_dims) {
    OnnxState *st = backend->state;
    int n = max_dims < 4 ? max_dims : 4;
    memcpy(shape, st->input_shape, n * sizeof(int));
    return n;
}

static int onnx_get_output_shape(InferenceBackend *backend, int *shape, int max_dims) {
    OnnxState *st = backend->state;
    int n = max_dims < 3 ? max_dims : 3;
    memcpy(shape, st->output_shape, n * sizeof(int));
    return n;
}

static size_t onnx_get_output_size(InferenceBackend *backend) {
    OnnxState *st = backend->state;
    return st->output_size;
}

static void onnx_destroy(InferenceBackend *backend) {
    OnnxState *st = backend->state;
    if (!st) return;
    if (st->session)      st->api->ReleaseSession(st->session);
    if (st->session_opts) st->api->ReleaseSessionOptions(st->session_opts);
    if (st->memory_info)  st->api->ReleaseMemoryInfo(st->memory_info);
    if (st->env)          st->api->ReleaseEnv(st->env);
    free(st->input_name);
    free(st->output_name);
    free(st);
    backend->state = NULL;
    backend->ready = false;
}

#else /* no ONNX */

static int onnx_init(InferenceBackend *b, const char *m, void *c) {
    (void)b; (void)m; (void)c;
    LOG_ERROR(NULL, "onnx", "ONNX Runtime not compiled — enable VP_WITH_ONNX");
    return -1;
}
static int onnx_infer(InferenceBackend *b, const float *i, float *o) {
    (void)b; (void)i; (void)o; return -1;
}
static int onnx_get_input_shape(InferenceBackend *b, int *s, int m) {
    (void)b; (void)s; (void)m; return 0;
}
static int onnx_get_output_shape(InferenceBackend *b, int *s, int m) {
    (void)b; (void)s; (void)m; return 0;
}
static size_t onnx_get_output_size(InferenceBackend *b) { (void)b; return 0; }
static void onnx_destroy(InferenceBackend *b) { (void)b; }

#endif /* VP_HAS_ONNX */

void backend_onnx_register(InferenceBackend *backend) {
    backend->ops.init             = onnx_init;
    backend->ops.infer            = onnx_infer;
    backend->ops.get_input_shape  = onnx_get_input_shape;
    backend->ops.get_output_shape = onnx_get_output_shape;
    backend->ops.get_output_size  = onnx_get_output_size;
    backend->ops.destroy          = onnx_destroy;
    strncpy(backend->info.backend_type, "onnx", sizeof(backend->info.backend_type) - 1);
}

/* ── Factory ── */
InferenceBackend *inference_backend_create(const char *backend_type) {
    InferenceBackend *b = calloc(1, sizeof(InferenceBackend));
    if (!b) return NULL;

    if (strcmp(backend_type, "onnx") == 0) {
        backend_onnx_register(b);
    } else {
        LOG_ERROR(NULL, "backend", "Unknown backend: %s", backend_type);
        free(b);
        return NULL;
    }

    return b;
}
