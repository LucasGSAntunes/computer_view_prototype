/*
 * Export Service — queries job results and generates reports.
 *
 * In the prototype, works as a CLI tool that reads from the database
 * or from local JSON files and generates comparison reports.
 *
 * Usage:
 *   vp_export -d <conninfo> -j <job_id>             Export single job
 *   vp_export -d <conninfo> -c <run1,run2,...>       Compare audit runs
 */

#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/common/logging.h"
#include "core/infra/database.h"
#include "core/infra/audit.h"
#include "core/infra/comparison.h"
#include "core/infra/exporter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Export / Report Service\n"
        "Usage: %s [options]\n\n"
        "  -d <conninfo>   PostgreSQL connection\n"
        "  -j <job_id>     Export results for a single job\n"
        "  -o <dir>        Output directory\n"
        "  -w <profile>    Score weights: low_latency, throughput, cost, balanced\n"
        "  -h              Help\n",
        prog);
}

int main(int argc, char *argv[]) {
    const char *db_conninfo = "host=localhost dbname=vision_platform user=postgres";
    const char *job_id = NULL;
    const char *output_dir = "./reports";
    const char *weight_profile = "balanced";

    int opt;
    while ((opt = getopt(argc, argv, "d:j:o:w:h")) != -1) {
        switch (opt) {
        case 'd': db_conninfo = optarg; break;
        case 'j': job_id = optarg; break;
        case 'o': output_dir = optarg; break;
        case 'w': weight_profile = optarg; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    LOG_INFO(NULL, "export", "=== Export / Report Service ===");

    /* Select weight profile */
    ScoreWeights weights;
    if (strcmp(weight_profile, "low_latency") == 0) {
        weights = weights_low_latency();
    } else if (strcmp(weight_profile, "throughput") == 0) {
        weights = weights_high_throughput();
    } else if (strcmp(weight_profile, "cost") == 0) {
        weights = weights_cost_efficient();
    } else {
        weights = weights_balanced();
    }

    fprintf(stdout, "Score weights: latency=%.2f throughput=%.2f memory=%.2f "
                    "stability=%.2f robustness=%.2f consistency=%.2f\n",
            weights.w_latency, weights.w_throughput, weights.w_memory,
            weights.w_stability, weights.w_robustness, weights.w_consistency);

    if (job_id) {
        fprintf(stdout, "Exporting job: %s\n", job_id);

        Database *db = db_connect(db_conninfo);
        if (!db) {
            LOG_ERROR(NULL, "export", "Cannot connect to database");
            return 1;
        }

        JobContext job = {0};
        VPError err = db_get_job(db, job_id, &job);
        if (err != VP_OK) {
            LOG_ERROR(NULL, "export", "Job not found: %s", job_id);
            db_close(db);
            return 1;
        }

        fprintf(stdout, "Job: %s\n  Source: %s\n  Status: %d\n  Profile: %s\n",
                job.job_id, job.source_uri, job.status, job.profile.name);

        db_close(db);
    } else {
        fprintf(stdout, "No job specified. Use -j <job_id> to export results.\n");
        fprintf(stdout, "Use the comparison API programmatically for multi-run comparisons.\n");
    }

    return 0;
}
