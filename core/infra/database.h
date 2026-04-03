#ifndef VP_DATABASE_H
#define VP_DATABASE_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/pipeline/metrics_collector.h"

typedef struct Database Database;

/* Connect to PostgreSQL. conninfo = "host=... dbname=... user=..." */
Database *db_connect(const char *conninfo);

/* Initialize schema (create tables if not exist). */
VPError db_init_schema(Database *db);

/* ── Job operations ── */
VPError db_insert_job(Database *db, const JobContext *job);
VPError db_update_job_status(Database *db, const char *job_id, JobStatus status);
VPError db_get_job(Database *db, const char *job_id, JobContext *out);

/* ── Profile operations ── */
VPError db_upsert_profile(Database *db, const ProcessingProfile *profile);

/* ── Engine operations ── */
VPError db_upsert_engine(Database *db, const EngineInfo *engine);

/* ── Runtime environment ── */
VPError db_upsert_runtime_env(Database *db, const RuntimeEnvironment *env);

/* ── Detections (bulk insert) ── */
VPError db_insert_detections(Database *db, const char *job_id,
                              const Detection *dets, int count);

/* ── Catalog ── */
VPError db_insert_catalog(Database *db, const char *job_id, const Catalog *catalog);

/* ── Metrics ── */
VPError db_insert_metrics(Database *db, const char *job_id,
                           const MetricsCollector *mc);

/* ── Query ── */
int db_count_jobs_by_status(Database *db, JobStatus status);

void db_close(Database *db);

#endif /* VP_DATABASE_H */
