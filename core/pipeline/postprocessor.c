#include "postprocessor.h"
#include "core/common/logging.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ── Detection list ── */

VPError detection_list_init(DetectionList *list, int capacity) {
    list->items = calloc(capacity, sizeof(Detection));
    if (!list->items) return VP_ERR_OUT_OF_MEMORY;
    list->count    = 0;
    list->capacity = capacity;
    return VP_OK;
}

void detection_list_free(DetectionList *list) {
    if (list) { free(list->items); list->items = NULL; list->count = 0; }
}

static VPError detection_list_add(DetectionList *list, const Detection *det) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity * 2;
        Detection *tmp = realloc(list->items, new_cap * sizeof(Detection));
        if (!tmp) return VP_ERR_OUT_OF_MEMORY;
        list->items    = tmp;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *det;
    return VP_OK;
}

/* ── Label loading ── */

int postprocess_load_labels(PostprocessConfig *config, const char *labels_path) {
    FILE *f = fopen(labels_path, "r");
    if (!f) {
        LOG_ERROR(NULL, "postprocess", "Cannot open labels: %s", labels_path);
        return -1;
    }

    int capacity = 128;
    config->class_names = malloc(capacity * sizeof(char *));
    config->class_names_count = 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        if (config->class_names_count >= capacity) {
            capacity *= 2;
            config->class_names = realloc(config->class_names, capacity * sizeof(char *));
        }
        config->class_names[config->class_names_count++] = strdup(line);
    }
    fclose(f);

    config->num_classes = config->class_names_count;
    LOG_INFO(NULL, "postprocess", "Loaded %d class labels from %s",
             config->num_classes, labels_path);
    return config->num_classes;
}

void postprocess_config_free(PostprocessConfig *config) {
    if (config && config->class_names) {
        for (int i = 0; i < config->class_names_count; i++)
            free(config->class_names[i]);
        free(config->class_names);
        config->class_names = NULL;
    }
}

/* ── IoU calculation ── */

static float iou(const BBox *a, const BBox *b) {
    float x1 = fmaxf(a->x, b->x);
    float y1 = fmaxf(a->y, b->y);
    float x2 = fminf(a->x + a->w, b->x + b->w);
    float y2 = fminf(a->y + a->h, b->y + b->h);

    float inter_w = fmaxf(0, x2 - x1);
    float inter_h = fmaxf(0, y2 - y1);
    float inter   = inter_w * inter_h;

    float area_a = a->w * a->h;
    float area_b = b->w * b->h;
    float union_area = area_a + area_b - inter;

    return (union_area > 0) ? inter / union_area : 0;
}

/* ── NMS ── */

static void nms(Detection *dets, int count, float iou_thresh, bool *keep) {
    /* Sort by confidence descending (simple selection) */
    for (int i = 0; i < count - 1; i++) {
        int max_idx = i;
        for (int j = i + 1; j < count; j++) {
            if (dets[j].confidence > dets[max_idx].confidence) max_idx = j;
        }
        if (max_idx != i) {
            Detection tmp = dets[i];
            dets[i] = dets[max_idx];
            dets[max_idx] = tmp;
        }
    }

    for (int i = 0; i < count; i++) keep[i] = true;

    for (int i = 0; i < count; i++) {
        if (!keep[i]) continue;
        for (int j = i + 1; j < count; j++) {
            if (!keep[j]) continue;
            if (dets[i].class_id == dets[j].class_id &&
                iou(&dets[i].bbox, &dets[j].bbox) > iou_thresh) {
                keep[j] = false;
            }
        }
    }
}

/* ── Main postprocessing ──
 *
 * YOLOv8 output format: [1, 84, 8400] transposed to [8400, 84]
 *   columns 0-3: cx, cy, w, h
 *   columns 4-83: class scores
 *
 * YOLOv5 output format: [1, 25200, 85]
 *   columns 0-3: cx, cy, w, h
 *   column 4: objectness
 *   columns 5-84: class scores
 */

VPError postprocess_detections(const float *raw_output, const int *output_shape,
                               const PreprocessConfig *preprocess_cfg,
                               const PostprocessConfig *config,
                               int frame_number, double timestamp_ms,
                               DetectionList *out) {
    if (!raw_output || !output_shape || !config || !out)
        return VP_ERR_INVALID_ARG;

    /* Determine format: shape [1, num_preds, dim] or [1, dim, num_preds] */
    int dim1 = output_shape[1];
    int dim2 = output_shape[2];

    int num_preds, pred_dim;
    bool transposed = false;

    if (dim1 > dim2) {
        /* [1, 8400, 85] — YOLOv5 style */
        num_preds = dim1;
        pred_dim  = dim2;
    } else {
        /* [1, 84, 8400] — YOLOv8 style, needs transpose */
        num_preds = dim2;
        pred_dim  = dim1;
        transposed = true;
    }

    bool has_objectness = (pred_dim == config->num_classes + 5);
    int class_offset = has_objectness ? 5 : 4;

    int max_dets = config->max_detections > 0 ? config->max_detections : 1000;
    Detection *candidates = malloc(max_dets * sizeof(Detection));
    if (!candidates) return VP_ERR_OUT_OF_MEMORY;
    int n_cand = 0;

    for (int i = 0; i < num_preds && n_cand < max_dets; i++) {
        float cx, cy, w, h;

        if (transposed) {
            cx = raw_output[0 * num_preds + i];
            cy = raw_output[1 * num_preds + i];
            w  = raw_output[2 * num_preds + i];
            h  = raw_output[3 * num_preds + i];
        } else {
            cx = raw_output[i * pred_dim + 0];
            cy = raw_output[i * pred_dim + 1];
            w  = raw_output[i * pred_dim + 2];
            h  = raw_output[i * pred_dim + 3];
        }

        float objectness = 1.0f;
        if (has_objectness) {
            objectness = transposed
                ? raw_output[4 * num_preds + i]
                : raw_output[i * pred_dim + 4];
        }

        /* Find best class */
        int   best_cls = 0;
        float best_score = -1.0f;
        for (int c = 0; c < config->num_classes; c++) {
            float score;
            if (transposed)
                score = raw_output[(class_offset + c) * num_preds + i];
            else
                score = raw_output[i * pred_dim + class_offset + c];

            if (score > best_score) {
                best_score = score;
                best_cls   = c;
            }
        }

        float confidence = objectness * best_score;
        if (confidence < config->confidence_threshold)
            continue;

        /* Convert center-format to top-left format and undo letterbox */
        float x1 = cx - w / 2.0f;
        float y1 = cy - h / 2.0f;

        if (preprocess_cfg) {
            x1 = (x1 - preprocess_cfg->pad_x) / preprocess_cfg->ratio;
            y1 = (y1 - preprocess_cfg->pad_y) / preprocess_cfg->ratio;
            w  = w / preprocess_cfg->ratio;
            h  = h / preprocess_cfg->ratio;
        }

        Detection det = {0};
        det.frame_number = frame_number;
        det.timestamp_ms = timestamp_ms;
        det.class_id     = best_cls;
        det.confidence   = confidence;
        det.bbox.x       = x1;
        det.bbox.y       = y1;
        det.bbox.w       = w;
        det.bbox.h       = h;
        det.track_id     = -1;

        if (best_cls < config->class_names_count && config->class_names[best_cls]) {
            strncpy(det.class_name, config->class_names[best_cls],
                    sizeof(det.class_name) - 1);
        }

        candidates[n_cand++] = det;
    }

    /* Apply NMS */
    if (n_cand > 0) {
        bool *keep = malloc(n_cand * sizeof(bool));
        if (!keep) { free(candidates); return VP_ERR_OUT_OF_MEMORY; }

        nms(candidates, n_cand, config->iou_threshold, keep);

        for (int i = 0; i < n_cand; i++) {
            if (keep[i]) {
                detection_list_add(out, &candidates[i]);
            }
        }
        free(keep);
    }

    free(candidates);
    return VP_OK;
}
