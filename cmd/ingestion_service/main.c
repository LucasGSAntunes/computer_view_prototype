/*
 * Ingestion Service — receives video submissions, validates, creates jobs,
 * and publishes to the job queue.
 *
 * Simple stdin/line-based interface for the prototype.
 * In production, this would be an HTTP/gRPC server.
 *
 * Input format (one per line):
 *   <video_path> [profile_name] [priority] [model_path] [labels_path]
 */

#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/common/logging.h"
#include "core/infra/database.h"
#include "core/infra/queue_redis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <signal.h>

static volatile int g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void generate_job_id(char *buf, size_t len) {
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, buf);
    if (len < 37) buf[len - 1] = '\0';
}

static bool validate_video(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (st.st_size == 0) return false;
    /* Basic extension check */
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    return (strcmp(ext, ".mp4") == 0 || strcmp(ext, ".avi") == 0 ||
            strcmp(ext, ".mkv") == 0 || strcmp(ext, ".mov") == 0 ||
            strcmp(ext, ".webm") == 0);
}

int main(int argc, char *argv[]) {
    const char *db_conninfo   = "host=localhost dbname=vision_platform user=postgres";
    const char *redis_host    = "localhost";
    int         redis_port    = 6379;
    const char *queue_name    = "vp:jobs";
    const char *default_model = "";
    const char *default_labels = "";

    /* Parse args */
    int opt;
    while ((opt = getopt(argc, argv, "d:r:p:q:m:l:h")) != -1) {
        switch (opt) {
        case 'd': db_conninfo = optarg; break;
        case 'r': redis_host = optarg; break;
        case 'p': redis_port = atoi(optarg); break;
        case 'q': queue_name = optarg; break;
        case 'm': default_model = optarg; break;
        case 'l': default_labels = optarg; break;
        case 'h':
            fprintf(stderr,
                "Ingestion Service\n"
                "  -d <conninfo>  PostgreSQL connection\n"
                "  -r <host>      Redis host\n"
                "  -p <port>      Redis port\n"
                "  -q <name>      Queue name\n"
                "  -m <path>      Default model path\n"
                "  -l <path>      Default labels path\n"
                "Input: <video_path> [profile] [priority]\n");
            return 0;
        default: return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    LOG_INFO(NULL, "ingestion", "=== Ingestion Service ===");

    /* Connect to DB */
    Database *db = db_connect(db_conninfo);
    if (db) {
        db_init_schema(db);
        LOG_INFO(NULL, "ingestion", "Database connected");
    } else {
        LOG_WARN(NULL, "ingestion", "Running without database (jobs logged to stdout only)");
    }

    /* Connect to queue */
    JobQueue *queue = queue_connect(redis_host, redis_port);
    if (queue) {
        LOG_INFO(NULL, "ingestion", "Queue connected");
    } else {
        LOG_WARN(NULL, "ingestion", "Running without queue (jobs not dispatched)");
    }

    LOG_INFO(NULL, "ingestion", "Ready — enter jobs: <video_path> [profile] [priority]");

    /* Main loop */
    char line[2048];
    while (g_running && fgets(line, sizeof(line), stdin)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        /* Parse fields */
        char video_path[512] = {0};
        char profile_name[64] = "balanced";
        int  priority = 5;

        sscanf(line, "%511s %63s %d", video_path, profile_name, &priority);

        /* Validate */
        if (!validate_video(video_path)) {
            LOG_ERROR(NULL, "ingestion", "Invalid video: %s", video_path);
            fprintf(stdout, "ERROR: invalid video %s\n", video_path);
            continue;
        }

        /* Create job */
        JobContext job = {0};
        generate_job_id(job.job_id, sizeof(job.job_id));
        strncpy(job.source_uri, video_path, sizeof(job.source_uri) - 1);
        strncpy(job.profile.name, profile_name, sizeof(job.profile.name) - 1);
        job.priority = priority;
        job.status = JOB_STATUS_RECEIVED;

        if (default_model[0])
            strncpy(job.model_path, default_model, sizeof(job.model_path) - 1);
        if (default_labels[0])
            strncpy(job.labels_path, default_labels, sizeof(job.labels_path) - 1);

        LOG_INFO(job.job_id, "ingestion", "Job created: video=%s profile=%s priority=%d",
                 video_path, profile_name, priority);

        /* Persist to DB */
        if (db) {
            db_insert_job(db, &job);
        }

        /* Publish to queue */
        if (queue) {
            VPError err = queue_push(queue, queue_name, job.job_id, priority);
            if (err == VP_OK) {
                job.status = JOB_STATUS_QUEUED;
                if (db) db_update_job_status(db, job.job_id, JOB_STATUS_QUEUED);
                LOG_INFO(job.job_id, "ingestion", "Queued in %s", queue_name);
            } else {
                LOG_ERROR(job.job_id, "ingestion", "Queue push failed");
            }
        }

        fprintf(stdout, "JOB_ID=%s STATUS=%d\n", job.job_id, job.status);
        fflush(stdout);
    }

    LOG_INFO(NULL, "ingestion", "Shutting down");
    queue_close(queue);
    db_close(db);
    return 0;
}
