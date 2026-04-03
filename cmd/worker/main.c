#include "core/common/types.h"
#include "core/common/errors.h"
#include "core/common/logging.h"
#include "core/common/timer.h"
#include "core/pipeline/pipeline.h"
#include "core/pipeline/event_detector.h"
#include "core/infra/exporter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <uuid/uuid.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Vision Platform — Worker\n"
        "Usage: %s [options] -i <video> -m <model>\n\n"
        "Required:\n"
        "  -i <path>      Input video file\n"
        "  -m <path>      Model file (.onnx)\n\n"
        "Optional:\n"
        "  -l <path>      Labels file (one class per line)\n"
        "  -o <dir>       Output directory (default: ./output)\n"
        "  -e <engine>    Backend: onnx (default)\n"
        "  -p <profile>   Profile: low_latency, throughput, cost, balanced (default)\n"
        "  -W <int>       Input width  (default: 640)\n"
        "  -H <int>       Input height (default: 640)\n"
        "  -c <float>     Confidence threshold (default: 0.25)\n"
        "  -u <float>     IoU threshold for NMS (default: 0.45)\n"
        "  -s <policy>    Sampling: all, interval, adaptive (default: all)\n"
        "  -n <int>       Sampling interval N (default: 1)\n"
        "  -t             Enable tracking (default: off)\n"
        "  -a <int>       Track max age in frames (default: 30)\n"
        "  -v             Verbose (debug logging)\n"
        "  -h             Show this help\n",
        prog);
}

static void generate_job_id(char *buf, size_t len) {
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, buf);
    if (len < 37) buf[len - 1] = '\0';
}

static void apply_profile_preset(ProcessingProfile *profile, const char *name) {
    if (strcmp(name, "low_latency") == 0) {
        strncpy(profile->name, "Low Latency", sizeof(profile->name) - 1);
        profile->input_width  = 416;
        profile->input_height = 416;
        profile->confidence_threshold = 0.3f;
        profile->iou_threshold = 0.5f;
        profile->batch_size = 1;
        profile->sampling_policy = SAMPLING_INTERVAL;
        profile->sampling_interval = 3;
    } else if (strcmp(name, "throughput") == 0) {
        strncpy(profile->name, "High Throughput", sizeof(profile->name) - 1);
        profile->input_width  = 640;
        profile->input_height = 640;
        profile->confidence_threshold = 0.25f;
        profile->iou_threshold = 0.45f;
        profile->batch_size = 4;
        profile->sampling_policy = SAMPLING_ALL;
        profile->sampling_interval = 1;
    } else if (strcmp(name, "cost") == 0) {
        strncpy(profile->name, "Cost Efficient", sizeof(profile->name) - 1);
        profile->input_width  = 320;
        profile->input_height = 320;
        profile->confidence_threshold = 0.4f;
        profile->iou_threshold = 0.5f;
        profile->batch_size = 1;
        profile->sampling_policy = SAMPLING_INTERVAL;
        profile->sampling_interval = 5;
    } else {
        strncpy(profile->name, "Balanced", sizeof(profile->name) - 1);
        profile->input_width  = 640;
        profile->input_height = 640;
        profile->confidence_threshold = 0.25f;
        profile->iou_threshold = 0.45f;
        profile->batch_size = 1;
        profile->sampling_policy = SAMPLING_ALL;
        profile->sampling_interval = 1;
    }
}

int main(int argc, char *argv[]) {
    /* Default job context */
    JobContext job = {0};
    char output_dir[512] = "./output";
    char profile_name[64] = "balanced";
    bool verbose = false;

    /* Defaults */
    job.profile.input_width  = 640;
    job.profile.input_height = 640;
    job.profile.confidence_threshold = 0.25f;
    job.profile.iou_threshold = 0.45f;
    job.profile.sampling_policy = SAMPLING_ALL;
    job.profile.sampling_interval = 1;
    job.profile.batch_size = 1;
    job.profile.track_max_age = 30;
    job.priority = 5;

    /* Parse args */
    int opt;
    while ((opt = getopt(argc, argv, "i:m:l:o:e:p:W:H:c:u:s:n:ta:vh")) != -1) {
        switch (opt) {
        case 'i': strncpy(job.source_uri, optarg, sizeof(job.source_uri) - 1); break;
        case 'm': strncpy(job.model_path, optarg, sizeof(job.model_path) - 1); break;
        case 'l': strncpy(job.labels_path, optarg, sizeof(job.labels_path) - 1); break;
        case 'o': strncpy(output_dir, optarg, sizeof(output_dir) - 1); break;
        case 'e': strncpy(job.profile.preferred_engine, optarg, sizeof(job.profile.preferred_engine) - 1); break;
        case 'p': strncpy(profile_name, optarg, sizeof(profile_name) - 1); break;
        case 'W': job.profile.input_width = atoi(optarg); break;
        case 'H': job.profile.input_height = atoi(optarg); break;
        case 'c': job.profile.confidence_threshold = (float)atof(optarg); break;
        case 'u': job.profile.iou_threshold = (float)atof(optarg); break;
        case 's':
            if (strcmp(optarg, "all") == 0)      job.profile.sampling_policy = SAMPLING_ALL;
            else if (strcmp(optarg, "interval") == 0) job.profile.sampling_policy = SAMPLING_INTERVAL;
            else if (strcmp(optarg, "adaptive") == 0) job.profile.sampling_policy = SAMPLING_ADAPTIVE;
            break;
        case 'n': job.profile.sampling_interval = atoi(optarg); break;
        case 't': job.profile.tracking_enabled = true; break;
        case 'a': job.profile.track_max_age = atoi(optarg); break;
        case 'v': verbose = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    /* Validate */
    if (!job.source_uri[0] || !job.model_path[0]) {
        fprintf(stderr, "Error: -i <video> and -m <model> are required.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Apply profile preset (CLI overrides take effect after) */
    if (strcmp(profile_name, "balanced") != 0) {
        ProcessingProfile preset = job.profile;
        apply_profile_preset(&preset, profile_name);
        /* Keep any CLI-specified overrides */
        if (job.profile.input_width != 640) preset.input_width = job.profile.input_width;
        if (job.profile.input_height != 640) preset.input_height = job.profile.input_height;
        job.profile = preset;
    } else {
        strncpy(job.profile.name, "Balanced", sizeof(job.profile.name) - 1);
    }

    if (verbose) vp_log_set_level(VP_LOG_DEBUG);

    /* Generate job ID */
    generate_job_id(job.job_id, sizeof(job.job_id));
    strncpy(job.output_dir, output_dir, sizeof(job.output_dir) - 1);
    job.status = JOB_STATUS_RECEIVED;

    /* Create output directory */
    mkdir(output_dir, 0755);

    LOG_INFO(job.job_id, "worker", "=== Vision Platform Worker ===");
    LOG_INFO(job.job_id, "worker", "Job:     %s", job.job_id);
    LOG_INFO(job.job_id, "worker", "Video:   %s", job.source_uri);
    LOG_INFO(job.job_id, "worker", "Model:   %s", job.model_path);
    LOG_INFO(job.job_id, "worker", "Profile: %s (%dx%d, conf=%.2f, iou=%.2f)",
             job.profile.name, job.profile.input_width, job.profile.input_height,
             job.profile.confidence_threshold, job.profile.iou_threshold);
    LOG_INFO(job.job_id, "worker", "Tracking: %s", job.profile.tracking_enabled ? "ON" : "OFF");

    /* ── Run pipeline ── */
    PipelineResult result;
    VPError err = pipeline_run(&job, &result);

    if (err != VP_OK) {
        LOG_ERROR(job.job_id, "worker", "Pipeline failed: %s", vp_error_str(err));
        pipeline_result_free(&result);
        return 1;
    }

    /* ── Export results ── */
    char path_buf[1024];

    snprintf(path_buf, sizeof(path_buf), "%s/detections.csv", output_dir);
    export_detections_csv(path_buf, result.all_detections, result.detection_count);

    snprintf(path_buf, sizeof(path_buf), "%s/catalog.json", output_dir);
    export_catalog_json(path_buf, &result.catalog);

    snprintf(path_buf, sizeof(path_buf), "%s/metrics.json", output_dir);
    export_metrics_json(path_buf, &result.metrics);

    snprintf(path_buf, sizeof(path_buf), "%s/report.json", output_dir);
    export_job_report_json(path_buf, &job, &result.catalog, &result.metrics);

    /* Export events if any */
    if (result.events.count > 0) {
        /* Create a temporary event detector just for export */
        EventDetectorConfig ev_cfg = event_detector_config_default();
        EventDetector *ed_export = event_detector_create(&ev_cfg);
        /* We already have the events in result, export directly */
        snprintf(path_buf, sizeof(path_buf), "%s/events.json", output_dir);
        FILE *ef = fopen(path_buf, "w");
        if (ef) {
            fprintf(ef, "{\n  \"accident_detected\": true,\n  \"total_events\": %d,\n  \"events\": [\n",
                    result.events.count);
            for (int i = 0; i < result.events.count; i++) {
                const AccidentEvent *ev = &result.events.events[i];
                fprintf(ef, "    {\"event_id\": \"%s\", \"type\": \"%s\", \"severity\": \"%s\", "
                           "\"confidence\": %.4f, \"start_frame\": %d, \"end_frame\": %d, "
                           "\"start_ms\": %.2f, \"end_ms\": %.2f, "
                           "\"location\": {\"x\": %.1f, \"y\": %.1f, \"w\": %.1f, \"h\": %.1f}, "
                           "\"involved_tracks\": [",
                        ev->event_id, event_type_str(ev->type), severity_str(ev->severity),
                        ev->confidence, ev->start_frame, ev->end_frame,
                        ev->start_ms, ev->end_ms,
                        ev->location_x, ev->location_y, ev->location_w, ev->location_h);
                for (int j = 0; j < ev->involved_count; j++)
                    fprintf(ef, "%d%s", ev->involved[j].track_id, j < ev->involved_count-1 ? "," : "");
                fprintf(ef, "]}%s\n", i < result.events.count-1 ? "," : "");
            }
            fprintf(ef, "  ]\n}\n");
            fclose(ef);
        }
        event_detector_destroy(ed_export);
    } else {
        snprintf(path_buf, sizeof(path_buf), "%s/events.json", output_dir);
        FILE *ef = fopen(path_buf, "w");
        if (ef) { fprintf(ef, "{\"accident_detected\": false, \"total_events\": 0, \"events\": []}\n"); fclose(ef); }
    }

    /* Summary */
    fprintf(stdout, "\n=== Job Complete ===\n");
    fprintf(stdout, "  Job ID:      %s\n", job.job_id);
    fprintf(stdout, "  Frames:      %d decoded, %d processed\n",
            result.frames_decoded, result.frames_processed);
    fprintf(stdout, "  Detections:  %d\n", result.detection_count);
    fprintf(stdout, "  Catalog:     %d classes\n", result.catalog.count);
    fprintf(stdout, "  Events:      %d %s\n", result.events.count,
            result.accident_detected ? "*** ACCIDENT DETECTED ***" : "(no accident)");
    fprintf(stdout, "  Duration:    %.2f s\n",
            metrics_total_duration_ms(&result.metrics) / 1000.0);
    fprintf(stdout, "  FPS:         %.2f\n", metrics_effective_fps(&result.metrics));
    fprintf(stdout, "  Output:      %s/\n", output_dir);

    pipeline_result_free(&result);
    return 0;
}
