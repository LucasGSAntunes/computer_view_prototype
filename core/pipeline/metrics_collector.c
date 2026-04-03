#include "metrics_collector.h"
#include "core/common/timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

static void series_init(MetricSeries *s) {
    memset(s, 0, sizeof(MetricSeries));
    s->capacity = 1024;
    s->values = malloc(s->capacity * sizeof(double));
    s->min = DBL_MAX;
    s->max = -DBL_MAX;
}

static void series_free(MetricSeries *s) {
    free(s->values);
    memset(s, 0, sizeof(MetricSeries));
}

void metrics_init(MetricsCollector *mc) {
    memset(mc, 0, sizeof(MetricsCollector));
    series_init(&mc->decode_ms);
    series_init(&mc->preprocess_ms);
    series_init(&mc->infer_ms);
    series_init(&mc->postprocess_ms);
    series_init(&mc->track_ms);
    series_init(&mc->frame_total_ms);
    mc->job_start_ms = vp_now_ms();
}

void metrics_record(MetricSeries *series, double value_ms) {
    if (!series) return;
    if (series->count >= series->capacity) {
        series->capacity *= 2;
        series->values = realloc(series->values, series->capacity * sizeof(double));
    }
    series->values[series->count++] = value_ms;
    series->sum += value_ms;
    if (value_ms < series->min) series->min = value_ms;
    if (value_ms > series->max) series->max = value_ms;
}

double metrics_avg(const MetricSeries *series) {
    if (!series || series->count == 0) return 0;
    return series->sum / series->count;
}

/* Compare for qsort */
static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(const MetricSeries *series, double p) {
    if (!series || series->count == 0) return 0;
    double *sorted = malloc(series->count * sizeof(double));
    memcpy(sorted, series->values, series->count * sizeof(double));
    qsort(sorted, series->count, sizeof(double), cmp_double);
    int idx = (int)(p / 100.0 * (series->count - 1));
    if (idx >= series->count) idx = series->count - 1;
    double val = sorted[idx];
    free(sorted);
    return val;
}

double metrics_p95(const MetricSeries *series) { return percentile(series, 95.0); }
double metrics_p99(const MetricSeries *series) { return percentile(series, 99.0); }

double metrics_stddev(const MetricSeries *series) {
    if (!series || series->count < 2) return 0;
    double mean = metrics_avg(series);
    double sum_sq = 0;
    for (int i = 0; i < series->count; i++) {
        double diff = series->values[i] - mean;
        sum_sq += diff * diff;
    }
    return sqrt(sum_sq / (series->count - 1));
}

double metrics_effective_fps(const MetricsCollector *mc) {
    double dur = metrics_total_duration_ms(mc);
    if (dur <= 0) return 0;
    return (double)mc->frames_processed / (dur / 1000.0);
}

double metrics_total_duration_ms(const MetricsCollector *mc) {
    return mc->job_end_ms - mc->job_start_ms;
}

char *metrics_to_json(const MetricsCollector *mc) {
    char *buf = malloc(4096);
    if (!buf) return NULL;

    snprintf(buf, 4096,
        "{\n"
        "  \"frames_processed\": %d,\n"
        "  \"total_detections\": %d,\n"
        "  \"total_duration_ms\": %.2f,\n"
        "  \"effective_fps\": %.2f,\n"
        "  \"peak_memory_mb\": %.2f,\n"
        "  \"errors\": %d,\n"
        "  \"decode_ms\":      {\"avg\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"std\": %.2f},\n"
        "  \"preprocess_ms\":  {\"avg\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"std\": %.2f},\n"
        "  \"infer_ms\":       {\"avg\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"std\": %.2f},\n"
        "  \"postprocess_ms\": {\"avg\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"std\": %.2f},\n"
        "  \"track_ms\":       {\"avg\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"std\": %.2f},\n"
        "  \"frame_total_ms\": {\"avg\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"std\": %.2f}\n"
        "}",
        mc->frames_processed,
        mc->total_detections,
        metrics_total_duration_ms(mc),
        metrics_effective_fps(mc),
        (double)mc->peak_memory_bytes / (1024.0 * 1024.0),
        mc->errors,
        metrics_avg(&mc->decode_ms), metrics_p95(&mc->decode_ms),
        metrics_p99(&mc->decode_ms), metrics_stddev(&mc->decode_ms),
        metrics_avg(&mc->preprocess_ms), metrics_p95(&mc->preprocess_ms),
        metrics_p99(&mc->preprocess_ms), metrics_stddev(&mc->preprocess_ms),
        metrics_avg(&mc->infer_ms), metrics_p95(&mc->infer_ms),
        metrics_p99(&mc->infer_ms), metrics_stddev(&mc->infer_ms),
        metrics_avg(&mc->postprocess_ms), metrics_p95(&mc->postprocess_ms),
        metrics_p99(&mc->postprocess_ms), metrics_stddev(&mc->postprocess_ms),
        metrics_avg(&mc->track_ms), metrics_p95(&mc->track_ms),
        metrics_p99(&mc->track_ms), metrics_stddev(&mc->track_ms),
        metrics_avg(&mc->frame_total_ms), metrics_p95(&mc->frame_total_ms),
        metrics_p99(&mc->frame_total_ms), metrics_stddev(&mc->frame_total_ms)
    );
    return buf;
}

void metrics_free(MetricsCollector *mc) {
    if (!mc) return;
    series_free(&mc->decode_ms);
    series_free(&mc->preprocess_ms);
    series_free(&mc->infer_ms);
    series_free(&mc->postprocess_ms);
    series_free(&mc->track_ms);
    series_free(&mc->frame_total_ms);
}
