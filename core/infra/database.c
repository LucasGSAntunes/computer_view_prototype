#include "database.h"
#include "core/common/logging.h"

#ifdef VP_HAS_POSTGRES
#include <libpq-fe.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct Database {
#ifdef VP_HAS_POSTGRES
    PGconn *conn;
#endif
    bool connected;
};

#ifdef VP_HAS_POSTGRES

Database *db_connect(const char *conninfo) {
    Database *db = calloc(1, sizeof(Database));
    if (!db) return NULL;

    db->conn = PQconnectdb(conninfo);
    if (PQstatus(db->conn) != CONNECTION_OK) {
        LOG_ERROR(NULL, "db", "Connection failed: %s", PQerrorMessage(db->conn));
        PQfinish(db->conn);
        free(db);
        return NULL;
    }

    db->connected = true;
    LOG_INFO(NULL, "db", "Connected to PostgreSQL");
    return db;
}

VPError db_init_schema(Database *db) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    const char *schema =
        "CREATE TABLE IF NOT EXISTS processing_profile ("
        "  profile_id SERIAL PRIMARY KEY,"
        "  name VARCHAR(64) UNIQUE NOT NULL,"
        "  sampling_policy INT DEFAULT 0,"
        "  input_resolution VARCHAR(16),"
        "  confidence_threshold REAL DEFAULT 0.25,"
        "  iou_threshold REAL DEFAULT 0.45,"
        "  batch_size INT DEFAULT 1,"
        "  created_at TIMESTAMPTZ DEFAULT NOW()"
        ");"

        "CREATE TABLE IF NOT EXISTS inference_engine ("
        "  engine_id SERIAL PRIMARY KEY,"
        "  name VARCHAR(64),"
        "  version VARCHAR(32),"
        "  backend_type VARCHAR(32),"
        "  device_type VARCHAR(32),"
        "  precision_mode VARCHAR(16),"
        "  UNIQUE(name, version, backend_type)"
        ");"

        "CREATE TABLE IF NOT EXISTS runtime_environment ("
        "  runtime_env_id SERIAL PRIMARY KEY,"
        "  cpu_model VARCHAR(128),"
        "  gpu_model VARCHAR(128),"
        "  memory_gb INT,"
        "  os_version VARCHAR(64),"
        "  driver_version VARCHAR(64),"
        "  UNIQUE(cpu_model, gpu_model, os_version)"
        ");"

        "CREATE TABLE IF NOT EXISTS video_job ("
        "  job_id VARCHAR(64) PRIMARY KEY,"
        "  source_uri TEXT NOT NULL,"
        "  status INT DEFAULT 0,"
        "  priority INT DEFAULT 5,"
        "  profile_name VARCHAR(64),"
        "  engine_name VARCHAR(64),"
        "  model_path TEXT,"
        "  created_at TIMESTAMPTZ DEFAULT NOW(),"
        "  updated_at TIMESTAMPTZ DEFAULT NOW()"
        ");"

        "CREATE TABLE IF NOT EXISTS frame_detection ("
        "  id BIGSERIAL PRIMARY KEY,"
        "  job_id VARCHAR(64) REFERENCES video_job(job_id),"
        "  frame_number INT,"
        "  timestamp_ms DOUBLE PRECISION,"
        "  class_id INT,"
        "  class_name VARCHAR(64),"
        "  confidence REAL,"
        "  bbox_x REAL, bbox_y REAL, bbox_w REAL, bbox_h REAL,"
        "  track_id INT"
        ");"

        "CREATE TABLE IF NOT EXISTS catalog_summary ("
        "  id SERIAL PRIMARY KEY,"
        "  job_id VARCHAR(64) REFERENCES video_job(job_id),"
        "  class_id INT,"
        "  class_name VARCHAR(64),"
        "  total_detections INT,"
        "  total_unique_tracks INT,"
        "  first_seen_ms DOUBLE PRECISION,"
        "  last_seen_ms DOUBLE PRECISION"
        ");"

        "CREATE TABLE IF NOT EXISTS processing_metrics ("
        "  id SERIAL PRIMARY KEY,"
        "  job_id VARCHAR(64) REFERENCES video_job(job_id),"
        "  frames_processed INT,"
        "  total_detections INT,"
        "  total_duration_ms DOUBLE PRECISION,"
        "  effective_fps DOUBLE PRECISION,"
        "  decode_avg_ms DOUBLE PRECISION,"
        "  infer_avg_ms DOUBLE PRECISION,"
        "  infer_p95_ms DOUBLE PRECISION,"
        "  infer_p99_ms DOUBLE PRECISION,"
        "  peak_memory_mb DOUBLE PRECISION,"
        "  errors INT"
        ");"

        "CREATE INDEX IF NOT EXISTS idx_detection_job ON frame_detection(job_id);"
        "CREATE INDEX IF NOT EXISTS idx_catalog_job ON catalog_summary(job_id);"
        "CREATE INDEX IF NOT EXISTS idx_job_status ON video_job(status);";

    PGresult *res = PQexec(db->conn, schema);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(NULL, "db", "Schema init failed: %s", PQerrorMessage(db->conn));
        PQclear(res);
        return VP_ERR_DATABASE_FAILED;
    }
    PQclear(res);
    LOG_INFO(NULL, "db", "Schema initialized");
    return VP_OK;
}

VPError db_insert_job(Database *db, const JobContext *job) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    char query[2048];
    snprintf(query, sizeof(query),
        "INSERT INTO video_job (job_id, source_uri, status, priority, profile_name, "
        "engine_name, model_path) VALUES ('%s', '%s', %d, %d, '%s', '%s', '%s') "
        "ON CONFLICT (job_id) DO NOTHING",
        job->job_id, job->source_uri, job->status, job->priority,
        job->profile.name, job->engine_info.engine_name, job->model_path);

    PGresult *res = PQexec(db->conn, query);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

VPError db_update_job_status(Database *db, const char *job_id, JobStatus status) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE video_job SET status = %d, updated_at = NOW() WHERE job_id = '%s'",
        status, job_id);

    PGresult *res = PQexec(db->conn, query);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

VPError db_get_job(Database *db, const char *job_id, JobContext *out) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    char query[256];
    snprintf(query, sizeof(query),
        "SELECT job_id, source_uri, status, priority, profile_name, model_path "
        "FROM video_job WHERE job_id = '%s'", job_id);

    PGresult *res = PQexec(db->conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return VP_ERR_DATABASE_FAILED;
    }

    strncpy(out->job_id, PQgetvalue(res, 0, 0), sizeof(out->job_id) - 1);
    strncpy(out->source_uri, PQgetvalue(res, 0, 1), sizeof(out->source_uri) - 1);
    out->status = atoi(PQgetvalue(res, 0, 2));
    out->priority = atoi(PQgetvalue(res, 0, 3));
    strncpy(out->profile.name, PQgetvalue(res, 0, 4), sizeof(out->profile.name) - 1);
    strncpy(out->model_path, PQgetvalue(res, 0, 5), sizeof(out->model_path) - 1);

    PQclear(res);
    return VP_OK;
}

VPError db_upsert_profile(Database *db, const ProcessingProfile *profile) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;
    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO processing_profile (name, sampling_policy, input_resolution, "
        "confidence_threshold, iou_threshold, batch_size) "
        "VALUES ('%s', %d, '%dx%d', %.4f, %.4f, %d) "
        "ON CONFLICT (name) DO UPDATE SET "
        "sampling_policy=EXCLUDED.sampling_policy, "
        "input_resolution=EXCLUDED.input_resolution",
        profile->name, profile->sampling_policy,
        profile->input_width, profile->input_height,
        profile->confidence_threshold, profile->iou_threshold,
        profile->batch_size);
    PGresult *res = PQexec(db->conn, query);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

VPError db_upsert_engine(Database *db, const EngineInfo *engine) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;
    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO inference_engine (name, version, backend_type, device_type, precision_mode) "
        "VALUES ('%s', '%s', '%s', '%s', '%s') "
        "ON CONFLICT (name, version, backend_type) DO NOTHING",
        engine->engine_name, engine->engine_version, engine->backend_type,
        engine->device_type, engine->precision_mode);
    PGresult *res = PQexec(db->conn, query);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

VPError db_upsert_runtime_env(Database *db, const RuntimeEnvironment *env) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;
    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO runtime_environment (cpu_model, gpu_model, memory_gb, os_version, driver_version) "
        "VALUES ('%s', '%s', %d, '%s', '%s') "
        "ON CONFLICT (cpu_model, gpu_model, os_version) DO NOTHING",
        env->cpu_model, env->gpu_model, env->memory_gb,
        env->os_version, env->driver_version);
    PGresult *res = PQexec(db->conn, query);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

VPError db_insert_detections(Database *db, const char *job_id,
                              const Detection *dets, int count) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    /* Use COPY for bulk insert */
    PGresult *res = PQexec(db->conn,
        "COPY frame_detection (job_id, frame_number, timestamp_ms, class_id, "
        "class_name, confidence, bbox_x, bbox_y, bbox_w, bbox_h, track_id) "
        "FROM STDIN WITH (FORMAT csv)");
    if (PQresultStatus(res) != PGRES_COPY_IN) {
        PQclear(res);
        return VP_ERR_DATABASE_FAILED;
    }
    PQclear(res);

    char row[512];
    for (int i = 0; i < count; i++) {
        const Detection *d = &dets[i];
        int len = snprintf(row, sizeof(row),
            "%s,%d,%.2f,%d,%s,%.4f,%.1f,%.1f,%.1f,%.1f,%d\n",
            job_id, d->frame_number, d->timestamp_ms, d->class_id,
            d->class_name, d->confidence,
            d->bbox.x, d->bbox.y, d->bbox.w, d->bbox.h, d->track_id);
        PQputCopyData(db->conn, row, len);
    }

    PQputCopyEnd(db->conn, NULL);
    res = PQgetResult(db->conn);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

VPError db_insert_catalog(Database *db, const char *job_id, const Catalog *catalog) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    for (int i = 0; i < catalog->count; i++) {
        const CatalogEntry *e = &catalog->entries[i];
        char query[512];
        snprintf(query, sizeof(query),
            "INSERT INTO catalog_summary (job_id, class_id, class_name, "
            "total_detections, total_unique_tracks, first_seen_ms, last_seen_ms) "
            "VALUES ('%s', %d, '%s', %d, %d, %.2f, %.2f)",
            job_id, e->class_id, e->class_name, e->total_detections,
            e->total_unique_tracks, e->first_seen_ms, e->last_seen_ms);
        PGresult *res = PQexec(db->conn, query);
        PQclear(res);
    }
    return VP_OK;
}

VPError db_insert_metrics(Database *db, const char *job_id,
                           const MetricsCollector *mc) {
    if (!db || !db->connected) return VP_ERR_DATABASE_FAILED;

    char query[1024];
    snprintf(query, sizeof(query),
        "INSERT INTO processing_metrics (job_id, frames_processed, total_detections, "
        "total_duration_ms, effective_fps, decode_avg_ms, infer_avg_ms, "
        "infer_p95_ms, infer_p99_ms, peak_memory_mb, errors) "
        "VALUES ('%s', %d, %d, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %d)",
        job_id, mc->frames_processed, mc->total_detections,
        metrics_total_duration_ms(mc), metrics_effective_fps(mc),
        metrics_avg(&mc->decode_ms), metrics_avg(&mc->infer_ms),
        metrics_p95(&mc->infer_ms), metrics_p99(&mc->infer_ms),
        (double)mc->peak_memory_bytes / (1024.0 * 1024.0), mc->errors);

    PGresult *res = PQexec(db->conn, query);
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK) ? VP_OK : VP_ERR_DATABASE_FAILED;
}

int db_count_jobs_by_status(Database *db, JobStatus status) {
    if (!db || !db->connected) return -1;
    char query[128];
    snprintf(query, sizeof(query),
        "SELECT COUNT(*) FROM video_job WHERE status = %d", status);
    PGresult *res = PQexec(db->conn, query);
    int count = (PQresultStatus(res) == PGRES_TUPLES_OK) ? atoi(PQgetvalue(res, 0, 0)) : -1;
    PQclear(res);
    return count;
}

void db_close(Database *db) {
    if (!db) return;
    if (db->conn) PQfinish(db->conn);
    free(db);
}

#else /* no Postgres */

Database *db_connect(const char *conninfo) {
    (void)conninfo;
    LOG_WARN(NULL, "db", "PostgreSQL support not compiled");
    return NULL;
}
VPError db_init_schema(Database *db) { (void)db; return VP_ERR_DATABASE_FAILED; }
VPError db_insert_job(Database *db, const JobContext *j) { (void)db; (void)j; return VP_ERR_DATABASE_FAILED; }
VPError db_update_job_status(Database *db, const char *id, JobStatus s) { (void)db; (void)id; (void)s; return VP_ERR_DATABASE_FAILED; }
VPError db_get_job(Database *db, const char *id, JobContext *o) { (void)db; (void)id; (void)o; return VP_ERR_DATABASE_FAILED; }
VPError db_upsert_profile(Database *db, const ProcessingProfile *p) { (void)db; (void)p; return VP_ERR_DATABASE_FAILED; }
VPError db_upsert_engine(Database *db, const EngineInfo *e) { (void)db; (void)e; return VP_ERR_DATABASE_FAILED; }
VPError db_upsert_runtime_env(Database *db, const RuntimeEnvironment *e) { (void)db; (void)e; return VP_ERR_DATABASE_FAILED; }
VPError db_insert_detections(Database *db, const char *id, const Detection *d, int c) { (void)db; (void)id; (void)d; (void)c; return VP_ERR_DATABASE_FAILED; }
VPError db_insert_catalog(Database *db, const char *id, const Catalog *c) { (void)db; (void)id; (void)c; return VP_ERR_DATABASE_FAILED; }
VPError db_insert_metrics(Database *db, const char *id, const MetricsCollector *m) { (void)db; (void)id; (void)m; return VP_ERR_DATABASE_FAILED; }
int db_count_jobs_by_status(Database *db, JobStatus s) { (void)db; (void)s; return -1; }
void db_close(Database *db) { free(db); }

#endif
