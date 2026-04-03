#include "audit.h"
#include "core/common/logging.h"
#include "core/common/timer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uuid/uuid.h>

/* Simple hash (djb2-based, not cryptographic — sufficient for config comparison) */
static uint64_t hash_string(const char *str) {
    uint64_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + (uint64_t)c;
    return hash;
}

static void snapshot_add(AuditSnapshot *snap, const char *key, const char *value) {
    if (snap->count >= snap->capacity) {
        snap->capacity = snap->capacity ? snap->capacity * 2 : 64;
        snap->params = realloc(snap->params, snap->capacity * sizeof(AuditParam));
    }
    AuditParam *p = &snap->params[snap->count++];
    strncpy(p->key, key, sizeof(p->key) - 1);
    strncpy(p->value, value, sizeof(p->value) - 1);
}

AuditRun *audit_run_create(const JobContext *job) {
    AuditRun *run = calloc(1, sizeof(AuditRun));
    if (!run) return NULL;

    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, run->audit_run_id);

    strncpy(run->job_id, job->job_id, sizeof(run->job_id) - 1);
    run->job_snapshot     = *job;
    run->profile_snapshot = job->profile;
    run->engine_snapshot  = job->engine_info;
    run->runtime_snapshot = job->runtime_env;
    run->started_at       = vp_now_ms();

    /* Freeze and hash config */
    AuditSnapshot snap = {0};
    audit_snapshot_freeze(job, &snap);
    audit_compute_hash(&snap, run->config_hash);
    audit_snapshot_free(&snap);

    LOG_INFO(job->job_id, "audit", "Audit run created: %s (hash=%s)",
             run->audit_run_id, run->config_hash);
    return run;
}

VPError audit_snapshot_freeze(const JobContext *job, AuditSnapshot *snap) {
    if (!job || !snap) return VP_ERR_INVALID_ARG;

    memset(snap, 0, sizeof(AuditSnapshot));

    char buf[256];

    snapshot_add(snap, "source_uri", job->source_uri);
    snapshot_add(snap, "model_path", job->model_path);
    snapshot_add(snap, "labels_path", job->labels_path);
    snapshot_add(snap, "profile.name", job->profile.name);

    snprintf(buf, sizeof(buf), "%dx%d", job->profile.input_width, job->profile.input_height);
    snapshot_add(snap, "profile.resolution", buf);

    snprintf(buf, sizeof(buf), "%d", job->profile.sampling_policy);
    snapshot_add(snap, "profile.sampling_policy", buf);

    snprintf(buf, sizeof(buf), "%d", job->profile.sampling_interval);
    snapshot_add(snap, "profile.sampling_interval", buf);

    snprintf(buf, sizeof(buf), "%.4f", job->profile.confidence_threshold);
    snapshot_add(snap, "profile.confidence_threshold", buf);

    snprintf(buf, sizeof(buf), "%.4f", job->profile.iou_threshold);
    snapshot_add(snap, "profile.iou_threshold", buf);

    snprintf(buf, sizeof(buf), "%d", job->profile.batch_size);
    snapshot_add(snap, "profile.batch_size", buf);

    snapshot_add(snap, "profile.preferred_engine", job->profile.preferred_engine);

    snprintf(buf, sizeof(buf), "%d", job->profile.tracking_enabled);
    snapshot_add(snap, "profile.tracking_enabled", buf);

    snprintf(buf, sizeof(buf), "%d", job->profile.track_max_age);
    snapshot_add(snap, "profile.track_max_age", buf);

    snapshot_add(snap, "engine.name", job->engine_info.engine_name);
    snapshot_add(snap, "engine.backend_type", job->engine_info.backend_type);
    snapshot_add(snap, "engine.device_type", job->engine_info.device_type);
    snapshot_add(snap, "engine.precision_mode", job->engine_info.precision_mode);

    snapshot_add(snap, "runtime.cpu", job->runtime_env.cpu_model);
    snapshot_add(snap, "runtime.gpu", job->runtime_env.gpu_model);
    snapshot_add(snap, "runtime.os", job->runtime_env.os_version);

    return VP_OK;
}

void audit_compute_hash(const AuditSnapshot *snap, char *hash_out) {
    uint64_t h = 0;
    for (int i = 0; i < snap->count; i++) {
        h ^= hash_string(snap->params[i].key);
        h ^= hash_string(snap->params[i].value);
        h = (h << 13) | (h >> 51);
    }
    snprintf(hash_out, 65, "%016lx", (unsigned long)h);
}

void audit_run_finalize(AuditRun *run, const MetricsCollector *metrics,
                         const Catalog *catalog) {
    if (!run) return;
    run->finished_at = vp_now_ms();
    run->metrics = *metrics; /* shallow copy, caller retains ownership of series data */
    run->catalog = *catalog;
    run->valid = true;

    LOG_INFO(run->job_id, "audit", "Audit run finalized: %s, duration=%.2f ms",
             run->audit_run_id, run->finished_at - run->started_at);
}

VPError audit_run_export_json(const AuditRun *run, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return VP_ERR_EXPORT_FAILED;

    char *metrics_json = metrics_to_json(&run->metrics);

    fprintf(f,
        "{\n"
        "  \"audit_run_id\": \"%s\",\n"
        "  \"job_id\": \"%s\",\n"
        "  \"config_hash\": \"%s\",\n"
        "  \"started_at\": %.2f,\n"
        "  \"finished_at\": %.2f,\n"
        "  \"duration_ms\": %.2f,\n"
        "  \"profile\": {\n"
        "    \"name\": \"%s\",\n"
        "    \"resolution\": \"%dx%d\",\n"
        "    \"confidence\": %.4f,\n"
        "    \"iou\": %.4f,\n"
        "    \"sampling\": %d,\n"
        "    \"interval\": %d,\n"
        "    \"tracking\": %s\n"
        "  },\n"
        "  \"engine\": {\n"
        "    \"name\": \"%s\",\n"
        "    \"backend\": \"%s\",\n"
        "    \"device\": \"%s\",\n"
        "    \"precision\": \"%s\"\n"
        "  },\n"
        "  \"catalog_classes\": %d,\n"
        "  \"metrics\": %s\n"
        "}\n",
        run->audit_run_id, run->job_id, run->config_hash,
        run->started_at, run->finished_at,
        run->finished_at - run->started_at,
        run->profile_snapshot.name,
        run->profile_snapshot.input_width, run->profile_snapshot.input_height,
        run->profile_snapshot.confidence_threshold,
        run->profile_snapshot.iou_threshold,
        run->profile_snapshot.sampling_policy,
        run->profile_snapshot.sampling_interval,
        run->profile_snapshot.tracking_enabled ? "true" : "false",
        run->engine_snapshot.engine_name,
        run->engine_snapshot.backend_type,
        run->engine_snapshot.device_type,
        run->engine_snapshot.precision_mode,
        run->catalog.count,
        metrics_json ? metrics_json : "{}");

    fclose(f);
    free(metrics_json);
    return VP_OK;
}

void audit_run_destroy(AuditRun *run) {
    free(run);
}

void audit_snapshot_free(AuditSnapshot *snap) {
    if (snap) { free(snap->params); snap->params = NULL; snap->count = 0; }
}
