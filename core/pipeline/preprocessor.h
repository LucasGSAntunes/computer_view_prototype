#ifndef VP_PREPROCESSOR_H
#define VP_PREPROCESSOR_H

#include "core/common/types.h"
#include "core/common/errors.h"

typedef struct {
    int   target_width;
    int   target_height;
    bool  letterbox;       /* true = preserve aspect ratio with padding */
    float scale;           /* pixel scale: 1.0/255.0 for 0-1 normalization */
    float mean[3];         /* per-channel mean subtraction (after scale) */
    float std[3];          /* per-channel std division (after scale) */
    bool  chw_order;       /* true = output CHW; false = HWC */
    /* Letterbox state (filled after preprocess) */
    float pad_x;
    float pad_y;
    float ratio;
} PreprocessConfig;

/* Create default config for YOLO-style models (640x640, 0-1, CHW). */
PreprocessConfig preprocess_config_default(int target_w, int target_h);

/* Preprocess frame into tensor.
 * tensor->data must be pre-allocated with enough space, or will be allocated.
 * Returns VP_OK on success. */
VPError preprocess_frame(const FrameBuffer *frame, Tensor *tensor,
                         PreprocessConfig *config);

#endif /* VP_PREPROCESSOR_H */
