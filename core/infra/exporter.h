#ifndef VP_EXPORTER_H
#define VP_EXPORTER_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/pipeline/metrics_collector.h"

/* Export frame detections to CSV. */
VPError export_detections_csv(const char *path, const Detection *dets, int count);

/* Export catalog summary to JSON. */
VPError export_catalog_json(const char *path, const Catalog *catalog);

/* Export metrics to JSON file. */
VPError export_metrics_json(const char *path, const MetricsCollector *mc);

/* Export full job report (catalog + metrics) as JSON. */
VPError export_job_report_json(const char *path, const JobContext *job,
                                const Catalog *catalog,
                                const MetricsCollector *mc);

#endif /* VP_EXPORTER_H */
