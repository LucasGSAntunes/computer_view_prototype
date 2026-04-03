#ifndef VP_BACKEND_ONNX_H
#define VP_BACKEND_ONNX_H

#include "inference_backend.h"

/* Register ONNX Runtime ops into an InferenceBackend. */
void backend_onnx_register(InferenceBackend *backend);

#endif /* VP_BACKEND_ONNX_H */
