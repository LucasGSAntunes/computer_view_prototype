/*
 * Scheduler / Dispatcher — consumes jobs from queue, resolves profiles,
 * dispatches to worker pools.
 *
 * In this prototype, it runs the pipeline directly (single-process mode).
 * In production, it would fork workers or communicate via IPC.
 */

#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/common/logging.h"
#include "core/infra/database.h"
#include "core/infra/queue_redis.h"
#include "core/pipeline/pipeline.h"
#include "core/infra/exporter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

static volatile int g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void apply_profile(JobContext *job) {
    const char *name = job->profile.name;

    if (strcmp(name, "low_latency") == 0 || strcmp(name, "Low Latency") == 0) {
        job->profile.input_width  = 416;
        job->profile.input_height = 416;
        job->profile.confidence_threshold = 0.3f;
        job->profile.iou_threshold = 0.5f;
        job->profile.sampling_policy = SAMPLING_INTERVAL;
        job->profile.sampling_interval = 3;
        job->profile.batch_size = 1;
        job->profile.tracking_enabled = true;
        job->profile.track_max_age = 20;
    } else if (strcmp(name, "throughput") == 0 || strcmp(name, "High Throughput") == 0) {
        job->profile.input_width  = 640;
        job->profile.input_height = 640;
        job->profile.confidence_threshold = 0.25f;
        job->profile.iou_threshold = 0.45f;
        job->profile.sampling_policy = SAMPLING_ALL;
        job->profile.sampling_interval = 1;
        job->profile.batch_size = 4;
        job->profile.tracking_enabled = true;
        job->profile.track_max_age = 30;
    } else if (strcmp(name, "cost") == 0 || strcmp(name, "Cost Efficient") == 0) {
        job->profile.input_width  = 320;
        job->profile.input_height = 320;
        job->profile.confidence_threshold = 0.4f;
        job->profile.iou_threshold = 0.5f;
        job->profile.sampling_policy = SAMPLING_INTERVAL;
        job->profile.sampling_interval = 5;
        job->profile.batch_size = 1;
        job->profile.tracking_enabled = false;
    } else { /* balanced */
        job->profile.input_width  = 640;
        job->profile.input_height = 640;
        job->profile.confidence_threshold = 0.25f;
        job->profile.iou_threshold = 0.45f;
        job->profile.sampling_policy = SAMPLING_ALL;
        job->profile.sampling_interval = 1;
        job->profile.batch_size = 1;
        job->profile.tracking_enabled = true;
        job->profile.track_max_age = 30;
    }
}

int main(int argc, char *argv[]) {
    const char *db_conninfo   = "host=localhost dbname=vision_platform user=postgres";
    const char *redis_host    = "localhost";
    int         redis_port    = 6379;
    const char *queue_name    = "vp:jobs";
    const char *model_path    = "";
    const char *labels_path   = "";
    const char *output_base   = "./output";
    int         max_retries   = 3;

    int opt;
    while ((opt = getopt(argc, argv, "d:r:p:q:m:l:o:R:h")) != -1) {
        switch (opt) {
        case 'd': db_conninfo = optarg; break;
        case 'r': redis_host = optarg; break;
        case 'p': redis_port = atoi(optarg); break;
        case 'q': queue_name = optarg; break;
        case 'm': model_path = optarg; break;
        case 'l': labels_path = optarg; break;
        case 'o': output_base = optarg; break;
        case 'R': max_retries = atoi(optarg); break;
        case 'h':
            fprintf(stderr,
                "Scheduler / Dispatcher\n"
                "  -d <conninfo>  PostgreSQL connection\n"
                "  -r <host>      Redis host\n"
                "  -p <port>      Redis port\n"
                "  -q <name>      Queue name\n"
                "  -m <model>     Model path (required)\n"
                "  -l <labels>    Labels path\n"
                "  -o <dir>       Output base directory\n"
                "  -R <int>       Max retries per job\n");
            return 0;
        default: return 1;
        }
    }

    if (!model_path[0]) {
        fprintf(stderr, "Error: -m <model> is required\n");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    LOG_INFO(NULL, "scheduler", "=== Scheduler / Dispatcher ===");

    Database *db = db_connect(db_conninfo);
    if (db) db_init_schema(db);

    JobQueue *queue = queue_connect(redis_host, redis_port);
    if (!queue) {
        LOG_ERROR(NULL, "scheduler", "Cannot connect to queue — exiting");
        db_close(db);
        return 1;
    }

    LOG_INFO(NULL, "scheduler", "Listening on queue: %s", queue_name);

    while (g_running) {
        char job_id[64] = {0};
        VPError err = queue_pop(queue, queue_name, job_id, sizeof(job_id), 5);
        if (err == VP_ERR_TIMEOUT) continue;
        if (err != VP_OK) {
            LOG_ERROR(NULL, "scheduler", "Queue pop error");
            continue;
        }

        LOG_INFO(job_id, "scheduler", "Dispatching job");

        /* Load job from DB or create minimal context */
        JobContext job = {0};
        strncpy(job.job_id, job_id, sizeof(job.job_id) - 1);

        if (db) {
            db_get_job(db, job_id, &job);
        }

        /* Ensure model and labels */
        if (!job.model_path[0])
            strncpy(job.model_path, model_path, sizeof(job.model_path) - 1);
        if (!job.labels_path[0] && labels_path[0])
            strncpy(job.labels_path, labels_path, sizeof(job.labels_path) - 1);

        /* Apply profile settings */
        apply_profile(&job);

        /* Output directory per job */
        char job_output[1024];
        snprintf(job_output, sizeof(job_output), "%s/%s", output_base, job.job_id);
        mkdir(job_output, 0755);
        strncpy(job.output_dir, job_output, sizeof(job.output_dir) - 1);

        /* Update status */
        job.status = JOB_STATUS_RUNNING;
        if (db) db_update_job_status(db, job.job_id, JOB_STATUS_RUNNING);

        /* Run pipeline */
        PipelineResult result;
        err = pipeline_run(&job, &result);

        if (err != VP_OK) {
            LOG_ERROR(job_id, "scheduler", "Pipeline failed: %s", vp_error_str(err));

            /* Retry logic */
            job.profile.max_retries--;
            if (job.profile.max_retries > 0) {
                LOG_INFO(job_id, "scheduler", "Retrying (%d left)", job.profile.max_retries);
                job.status = JOB_STATUS_RETRYING;
                if (db) db_update_job_status(db, job.job_id, JOB_STATUS_RETRYING);
                queue_push(queue, queue_name, job.job_id, job.priority + 1);
            } else {
                job.status = JOB_STATUS_FAILED;
                if (db) db_update_job_status(db, job.job_id, JOB_STATUS_FAILED);
                queue_push_dead_letter(queue, job.job_id, vp_error_str(err));
            }
            pipeline_result_free(&result);
            continue;
        }

        /* Export results */
        char path_buf[1280];
        snprintf(path_buf, sizeof(path_buf), "%s/detections.csv", job_output);
        export_detections_csv(path_buf, result.all_detections, result.detection_count);

        snprintf(path_buf, sizeof(path_buf), "%s/catalog.json", job_output);
        export_catalog_json(path_buf, &result.catalog);

        snprintf(path_buf, sizeof(path_buf), "%s/metrics.json", job_output);
        export_metrics_json(path_buf, &result.metrics);

        snprintf(path_buf, sizeof(path_buf), "%s/report.json", job_output);
        export_job_report_json(path_buf, &job, &result.catalog, &result.metrics);

        /* Persist to DB */
        if (db) {
            db_insert_detections(db, job.job_id, result.all_detections, result.detection_count);
            db_insert_catalog(db, job.job_id, &result.catalog);
            db_insert_metrics(db, job.job_id, &result.metrics);
            db_update_job_status(db, job.job_id, JOB_STATUS_COMPLETED);
        }

        LOG_INFO(job_id, "scheduler", "Job completed: %d detections, %.1f FPS",
                 result.detection_count, metrics_effective_fps(&result.metrics));

        pipeline_result_free(&result);
    }

    LOG_INFO(NULL, "scheduler", "Shutting down");
    queue_close(queue);
    db_close(db);
    return 0;
}
