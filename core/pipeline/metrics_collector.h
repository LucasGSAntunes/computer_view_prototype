#ifndef VP_METRICS_COLLECTOR_H
#define VP_METRICS_COLLECTOR_H

#include "core/common/types.h"
#include <stddef.h>

typedef struct {
    double *values;
    int     count;
    int     capacity;
    double  sum;
    double  min;
    double  max;
} MetricSeries;

typedef struct {
    MetricSeries decode_ms;
    MetricSeries preprocess_ms;
    MetricSeries infer_ms;
    MetricSeries postprocess_ms;
    MetricSeries track_ms;
    MetricSeries frame_total_ms;
    double       job_start_ms;
    double       job_end_ms;
    int          frames_processed;
    int          total_detections;
    size_t       peak_memory_bytes;
    int          errors;
} MetricsCollector;

/* Initialize collector. */
void metrics_init(MetricsCollector *mc);

/* Record a timing for a stage. */
void metrics_record(MetricSeries *series, double value_ms);

/* Compute statistics from a series. */
double metrics_avg(const MetricSeries *series);
double metrics_p95(const MetricSeries *series);
double metrics_p99(const MetricSeries *series);
double metrics_stddev(const MetricSeries *series);

/* Effective FPS over job duration. */
double metrics_effective_fps(const MetricsCollector *mc);

/* Total job duration. */
double metrics_total_duration_ms(const MetricsCollector *mc);

/* Export metrics to JSON string (caller must free). */
char *metrics_to_json(const MetricsCollector *mc);

/* Free all series data. */
void metrics_free(MetricsCollector *mc);

#endif /* VP_METRICS_COLLECTOR_H */
