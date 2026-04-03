#include "preprocessor.h"
#include "core/common/logging.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

PreprocessConfig preprocess_config_default(int target_w, int target_h) {
    PreprocessConfig cfg = {0};
    cfg.target_width  = target_w;
    cfg.target_height = target_h;
    cfg.letterbox     = true;
    cfg.scale         = 1.0f / 255.0f;
    cfg.mean[0] = cfg.mean[1] = cfg.mean[2] = 0.0f;
    cfg.std[0]  = cfg.std[1]  = cfg.std[2]  = 1.0f;
    cfg.chw_order = true;
    return cfg;
}

/* Bilinear interpolation for resize */
static inline float bilinear_sample(const uint8_t *src, int sw, int sh,
                                     int channels, float fx, float fy, int c) {
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= sw) x1 = sw - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= sh) y1 = sh - 1;

    float dx = fx - (float)(int)fx;
    float dy = fy - (float)(int)fy;

    float v00 = src[(y0 * sw + x0) * channels + c];
    float v01 = src[(y0 * sw + x1) * channels + c];
    float v10 = src[(y1 * sw + x0) * channels + c];
    float v11 = src[(y1 * sw + x1) * channels + c];

    return (1 - dy) * ((1 - dx) * v00 + dx * v01) +
           dy       * ((1 - dx) * v10 + dx * v11);
}

VPError preprocess_frame(const FrameBuffer *frame, Tensor *tensor,
                         PreprocessConfig *config) {
    if (!frame || !tensor || !config) return VP_ERR_INVALID_ARG;

    int tw = config->target_width;
    int th = config->target_height;
    size_t tensor_size = (size_t)tw * th * 3;

    /* Allocate tensor if needed */
    if (!tensor->data) {
        tensor->data = malloc(tensor_size * sizeof(float));
        if (!tensor->data) return VP_ERR_OUT_OF_MEMORY;
    }
    tensor->size = tensor_size;
    tensor->ndims = 4;
    tensor->dims[0] = 1;  /* N */
    tensor->dims[1] = 3;  /* C */
    tensor->dims[2] = th; /* H */
    tensor->dims[3] = tw; /* W */

    int sw = frame->width;
    int sh = frame->height;
    const uint8_t *src = frame->data;

    float ratio_w = (float)tw / sw;
    float ratio_h = (float)th / sh;
    float ratio;
    float pad_x = 0, pad_y = 0;

    if (config->letterbox) {
        ratio = (ratio_w < ratio_h) ? ratio_w : ratio_h;
        int new_w = (int)(sw * ratio);
        int new_h = (int)(sh * ratio);
        pad_x = (tw - new_w) / 2.0f;
        pad_y = (th - new_h) / 2.0f;
    } else {
        ratio = 1.0f; /* just direct resize */
    }

    config->pad_x = pad_x;
    config->pad_y = pad_y;
    config->ratio = ratio;

    /* Fill tensor with padding value (0.5 gray for letterbox) */
    float pad_val = config->letterbox ? 0.5f : 0.0f;
    for (size_t i = 0; i < tensor_size; i++) {
        tensor->data[i] = pad_val;
    }

    /* Resize and normalize */
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            float src_x, src_y;

            if (config->letterbox) {
                src_x = (x - pad_x) / ratio;
                src_y = (y - pad_y) / ratio;
            } else {
                src_x = (float)x / ratio_w * 1.0f;
                src_y = (float)y / ratio_h * 1.0f;
            }

            if (src_x < 0 || src_x >= sw - 1 || src_y < 0 || src_y >= sh - 1)
                continue;

            for (int c = 0; c < 3; c++) {
                float val = bilinear_sample(src, sw, sh, 3, src_x, src_y, c);
                val = val * config->scale;
                val = (val - config->mean[c]) / config->std[c];

                if (config->chw_order) {
                    /* CHW layout: [c][y][x] */
                    tensor->data[c * th * tw + y * tw + x] = val;
                } else {
                    /* HWC layout: [y][x][c] */
                    tensor->data[(y * tw + x) * 3 + c] = val;
                }
            }
        }
    }

    return VP_OK;
}
