#ifndef VP_POSTPROCESSOR_H
#define VP_POSTPROCESSOR_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include "preprocessor.h"

typedef struct {
    float  confidence_threshold;
    float  iou_threshold;
    int    max_detections;
    int    num_classes;
    char **class_names;       /* array of class name strings */
    int    class_names_count;
} PostprocessConfig;

/* Load class names from a text file (one name per line).
 * Returns number of classes loaded, or -1 on error. */
int postprocess_load_labels(PostprocessConfig *config, const char *labels_path);

/* Postprocess raw YOLO output into detection list.
 * Applies confidence filter, NMS, and class mapping.
 * raw_output: pointer to model output tensor
 * output_shape: [batch, num_predictions, dim] e.g. [1, 8400, 84]
 * preprocess_cfg: for coordinate correction (letterbox) */
VPError postprocess_detections(const float *raw_output, const int *output_shape,
                               const PreprocessConfig *preprocess_cfg,
                               const PostprocessConfig *config,
                               int frame_number, double timestamp_ms,
                               DetectionList *out);

/* Free class names in config. */
void postprocess_config_free(PostprocessConfig *config);

/* Free detection list items. */
void detection_list_free(DetectionList *list);

/* Init detection list with given capacity. */
VPError detection_list_init(DetectionList *list, int capacity);

#endif /* VP_POSTPROCESSOR_H */
