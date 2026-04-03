#ifndef VP_PIPELINE_H
#define VP_PIPELINE_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include "metrics_collector.h"

/* Pipeline result holds all outputs from a job execution. */
typedef struct {
    Detection       *all_detections;
    int              detection_count;
    int              detection_capacity;
    Catalog          catalog;
    EventList        events;
    MetricsCollector metrics;
    int              frames_decoded;
    int              frames_processed;
    bool             accident_detected;
} PipelineResult;

/* Run the full pipeline for a job.
 * Decode -> Sample -> Preprocess -> Infer -> Postprocess -> Track ->
 * Event Detection -> Catalog -> Export.
 * Returns VP_OK on success. */
VPError pipeline_run(JobContext *job, PipelineResult *result);

/* Free all resources in a pipeline result. */
void pipeline_result_free(PipelineResult *result);

#endif /* VP_PIPELINE_H */
