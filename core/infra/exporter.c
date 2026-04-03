#include "exporter.h"
#include "core/common/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VPError export_detections_csv(const char *path, const Detection *dets, int count) {
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_ERROR(NULL, "export", "Cannot open CSV: %s", path);
        return VP_ERR_EXPORT_FAILED;
    }

    fprintf(f, "frame,timestamp_ms,class_id,class_name,confidence,"
               "bbox_x,bbox_y,bbox_w,bbox_h,track_id\n");

    for (int i = 0; i < count; i++) {
        const Detection *d = &dets[i];
        fprintf(f, "%d,%.2f,%d,%s,%.4f,%.1f,%.1f,%.1f,%.1f,%d\n",
                d->frame_number, d->timestamp_ms, d->class_id, d->class_name,
                d->confidence, d->bbox.x, d->bbox.y, d->bbox.w, d->bbox.h,
                d->track_id);
    }

    fclose(f);
    LOG_INFO(NULL, "export", "Detections CSV: %s (%d rows)", path, count);
    return VP_OK;
}

VPError export_catalog_json(const char *path, const Catalog *catalog) {
    FILE *f = fopen(path, "w");
    if (!f) return VP_ERR_EXPORT_FAILED;

    fprintf(f, "{\n  \"catalog\": [\n");
    for (int i = 0; i < catalog->count; i++) {
        const CatalogEntry *e = &catalog->entries[i];
        fprintf(f, "    {\n"
                   "      \"class_id\": %d,\n"
                   "      \"class_name\": \"%s\",\n"
                   "      \"total_detections\": %d,\n"
                   "      \"unique_objects\": %d,\n"
                   "      \"first_seen_ms\": %.2f,\n"
                   "      \"last_seen_ms\": %.2f\n"
                   "    }%s\n",
                e->class_id, e->class_name, e->total_detections,
                e->total_unique_tracks, e->first_seen_ms, e->last_seen_ms,
                (i < catalog->count - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    LOG_INFO(NULL, "export", "Catalog JSON: %s (%d classes)", path, catalog->count);
    return VP_OK;
}

VPError export_metrics_json(const char *path, const MetricsCollector *mc) {
    char *json = metrics_to_json(mc);
    if (!json) return VP_ERR_OUT_OF_MEMORY;

    FILE *f = fopen(path, "w");
    if (!f) { free(json); return VP_ERR_EXPORT_FAILED; }
    fprintf(f, "%s\n", json);
    fclose(f);
    free(json);

    LOG_INFO(NULL, "export", "Metrics JSON: %s", path);
    return VP_OK;
}

VPError export_job_report_json(const char *path, const JobContext *job,
                                const Catalog *catalog,
                                const MetricsCollector *mc) {
    FILE *f = fopen(path, "w");
    if (!f) return VP_ERR_EXPORT_FAILED;

    char *metrics_json = metrics_to_json(mc);

    fprintf(f, "{\n"
               "  \"job_id\": \"%s\",\n"
               "  \"source\": \"%s\",\n"
               "  \"status\": %d,\n"
               "  \"profile\": \"%s\",\n"
               "  \"engine\": \"%s\",\n"
               "  \"model\": \"%s\",\n",
            job->job_id, job->source_uri, job->status,
            job->profile.name, job->engine_info.engine_name, job->model_path);

    /* Catalog */
    fprintf(f, "  \"catalog\": [\n");
    for (int i = 0; i < catalog->count; i++) {
        const CatalogEntry *e = &catalog->entries[i];
        fprintf(f, "    {\"class\": \"%s\", \"detections\": %d, \"unique\": %d, "
                   "\"first_ms\": %.2f, \"last_ms\": %.2f}%s\n",
                e->class_name, e->total_detections, e->total_unique_tracks,
                e->first_seen_ms, e->last_seen_ms,
                (i < catalog->count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    /* Metrics */
    fprintf(f, "  \"metrics\": %s\n", metrics_json ? metrics_json : "{}");
    fprintf(f, "}\n");

    fclose(f);
    free(metrics_json);

    LOG_INFO(NULL, "export", "Job report: %s", path);
    return VP_OK;
}
