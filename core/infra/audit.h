#ifndef VP_AUDIT_H
#define VP_AUDIT_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/pipeline/metrics_collector.h"

/* ── Audit run — represents one auditable execution ── */
typedef struct {
    char               audit_run_id[64];
    char               job_id[64];
    JobContext          job_snapshot;
    ProcessingProfile  profile_snapshot;
    EngineInfo         engine_snapshot;
    RuntimeEnvironment runtime_snapshot;
    MetricsCollector   metrics;
    Catalog            catalog;
    char               config_hash[65];   /* SHA256 hex of frozen config */
    double             started_at;
    double             finished_at;
    bool               valid;
} AuditRun;

/* ── Parameter snapshot (key-value pairs) ── */
typedef struct {
    char key[64];
    char value[256];
} AuditParam;

typedef struct {
    AuditParam *params;
    int         count;
    int         capacity;
} AuditSnapshot;

/* Create a new audit run. Freezes all parameters. */
AuditRun *audit_run_create(const JobContext *job);

/* Freeze parameters into snapshot. */
VPError audit_snapshot_freeze(const JobContext *job, AuditSnapshot *snap);

/* Compute config hash from snapshot. */
void audit_compute_hash(const AuditSnapshot *snap, char *hash_out);

/* Finalize audit run with results. */
void audit_run_finalize(AuditRun *run, const MetricsCollector *metrics,
                         const Catalog *catalog);

/* Export audit run to JSON file. */
VPError audit_run_export_json(const AuditRun *run, const char *path);

/* Free resources. */
void audit_run_destroy(AuditRun *run);
void audit_snapshot_free(AuditSnapshot *snap);

#endif /* VP_AUDIT_H */
