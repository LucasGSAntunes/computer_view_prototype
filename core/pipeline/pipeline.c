#include "pipeline.h"
#include "video_decoder.h"
#include "frame_sampler.h"
#include "preprocessor.h"
#include "inference_backend.h"
#include "postprocessor.h"
#include "tracker.h"
#include "trajectory_builder.h"
#include "event_detector.h"
#include "catalog_builder.h"
#include "core/common/logging.h"
#include "core/common/timer.h"
#include "core/common/memory_pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static VPError add_detections(PipelineResult *result, const DetectionList *dets) {
    for (int i = 0; i < dets->count; i++) {
        if (result->detection_count >= result->detection_capacity) {
            int new_cap = result->detection_capacity * 2;
            Detection *tmp = realloc(result->all_detections,
                                      new_cap * sizeof(Detection));
            if (!tmp) return VP_ERR_OUT_OF_MEMORY;
            result->all_detections = tmp;
            result->detection_capacity = new_cap;
        }
        result->all_detections[result->detection_count++] = dets->items[i];
    }
    return VP_OK;
}

VPError pipeline_run(JobContext *job, PipelineResult *result) {
    if (!job || !result) return VP_ERR_INVALID_ARG;

    memset(result, 0, sizeof(PipelineResult));
    result->detection_capacity = 4096;
    result->all_detections = malloc(result->detection_capacity * sizeof(Detection));
    if (!result->all_detections) return VP_ERR_OUT_OF_MEMORY;

    metrics_init(&result->metrics);

    const char *jid = job->job_id;
    LOG_INFO(jid, "pipeline", "Starting job: %s", job->source_uri);
    job->status = JOB_STATUS_RUNNING;

    /* ── 1. Video decoder ── */
    VideoDecoder *decoder = NULL;
    VPError err = video_decoder_open(&decoder, job->source_uri);
    if (err != VP_OK) {
        LOG_ERROR(jid, "pipeline", "Failed to open video: %s", vp_error_str(err));
        job->status = JOB_STATUS_FAILED;
        return err;
    }

    int total_frames = video_decoder_total_frames(decoder);
    double fps = video_decoder_fps(decoder);
    LOG_INFO(jid, "pipeline", "Video: %dx%d, %.2f fps, ~%d frames",
             video_decoder_width(decoder), video_decoder_height(decoder),
             fps, total_frames);

    /* ── 2. Frame sampler ── */
    FrameSampler *sampler = frame_sampler_create(
        job->profile.sampling_policy,
        job->profile.sampling_interval
    );

    /* ── 3. Preprocessor config ── */
    PreprocessConfig pp_cfg = preprocess_config_default(
        job->profile.input_width, job->profile.input_height
    );

    /* ── 4. Inference backend ── */
    const char *backend_type = job->profile.preferred_engine[0]
        ? job->profile.preferred_engine : "onnx";

    InferenceBackend *backend = inference_backend_create(backend_type);
    if (!backend) {
        LOG_ERROR(jid, "pipeline", "Failed to create backend: %s", backend_type);
        video_decoder_close(decoder);
        frame_sampler_destroy(sampler);
        job->status = JOB_STATUS_FAILED;
        return VP_ERR_BACKEND_INIT_FAILED;
    }

    err = inference_init(backend, job->model_path, NULL);
    if (err != VP_OK) {
        LOG_ERROR(jid, "pipeline", "Backend init failed");
        video_decoder_close(decoder);
        frame_sampler_destroy(sampler);
        inference_destroy(backend);
        job->status = JOB_STATUS_FAILED;
        return err;
    }
    job->engine_info = backend->info;

    /* Output buffer */
    size_t output_size = inference_get_output_size(backend);
    float *output_buf = malloc(output_size * sizeof(float));
    if (!output_buf) {
        video_decoder_close(decoder);
        frame_sampler_destroy(sampler);
        inference_destroy(backend);
        return VP_ERR_OUT_OF_MEMORY;
    }

    int output_shape[3];
    inference_get_output_shape(backend, output_shape, 3);

    /* ── 5. Postprocessor config ── */
    PostprocessConfig post_cfg = {0};
    post_cfg.confidence_threshold = job->profile.confidence_threshold;
    post_cfg.iou_threshold        = job->profile.iou_threshold;
    post_cfg.max_detections       = 1000;

    if (job->labels_path[0]) {
        postprocess_load_labels(&post_cfg, job->labels_path);
    } else {
        post_cfg.num_classes = 80; /* COCO default */
    }

    /* ── 6. Tracker ── */
    Tracker *tracker = NULL;
    if (job->profile.tracking_enabled) {
        TrackerConfig trk_cfg = {
            .iou_threshold = job->profile.iou_threshold,
            .max_age       = job->profile.track_max_age > 0 ? job->profile.track_max_age : 30,
            .min_hits      = 3
        };
        tracker = tracker_create(&trk_cfg);
    }

    /* ── 7. Trajectory builder ── */
    TrajectoryBuilder *traj_builder = NULL;
    if (job->profile.tracking_enabled) {
        traj_builder = trajectory_builder_create(fps);
    }

    /* ── 8. Event detector ── */
    EventDetector *event_det = NULL;
    if (job->profile.tracking_enabled) {
        EventDetectorConfig ev_cfg = event_detector_config_default();
        event_det = event_detector_create(&ev_cfg);
        LOG_INFO(jid, "pipeline", "Event detection: ENABLED");
    }

    /* ── 9. Catalog builder ── */
    CatalogBuilder *cat_builder = catalog_builder_create(post_cfg.num_classes);

    /* ── 10. Tensor buffer ── */
    Tensor tensor = {0};

    /* ── Main processing loop ── */
    FrameBuffer frame = {0};
    DetectionList dets = {0};
    detection_list_init(&dets, 256);

    int frame_num = 0;
    LOG_INFO(jid, "pipeline", "Processing started");

    while (1) {
        VPTimer t_frame, t_stage;
        vp_timer_start(&t_frame);

        /* Decode */
        vp_timer_start(&t_stage);
        err = video_decoder_next_frame(decoder, &frame);
        if (err != VP_OK) break; /* EOF */
        double decode_time = vp_timer_elapsed_ms(&t_stage);
        metrics_record(&result->metrics.decode_ms, decode_time);
        result->frames_decoded++;

        /* Sample */
        if (!frame_sampler_should_process(sampler, frame_num, frame.timestamp_ms)) {
            frame_num++;
            continue;
        }

        /* Preprocess */
        vp_timer_start(&t_stage);
        err = preprocess_frame(&frame, &tensor, &pp_cfg);
        double preprocess_time = vp_timer_elapsed_ms(&t_stage);
        metrics_record(&result->metrics.preprocess_ms, preprocess_time);
        if (err != VP_OK) {
            LOG_WARN(jid, "preprocess", "Frame %d failed", frame_num);
            result->metrics.errors++;
            frame_num++;
            continue;
        }

        /* Infer */
        vp_timer_start(&t_stage);
        err = inference_run(backend, tensor.data, output_buf);
        double infer_time = vp_timer_elapsed_ms(&t_stage);
        metrics_record(&result->metrics.infer_ms, infer_time);
        if (err != VP_OK) {
            LOG_WARN(jid, "infer", "Frame %d failed", frame_num);
            result->metrics.errors++;
            frame_num++;
            continue;
        }

        /* Postprocess */
        vp_timer_start(&t_stage);
        dets.count = 0;
        err = postprocess_detections(output_buf, output_shape, &pp_cfg,
                                     &post_cfg, frame_num, frame.timestamp_ms,
                                     &dets);
        double postprocess_time = vp_timer_elapsed_ms(&t_stage);
        metrics_record(&result->metrics.postprocess_ms, postprocess_time);

        /* Track */
        double track_time = 0;
        if (tracker && dets.count > 0) {
            vp_timer_start(&t_stage);
            tracker_update(tracker, &dets, frame_num);
            track_time = vp_timer_elapsed_ms(&t_stage);
        }
        metrics_record(&result->metrics.track_ms, track_time);

        /* Trajectory + Event detection */
        if (traj_builder && dets.count > 0) {
            trajectory_builder_update(traj_builder, &dets, frame_num, frame.timestamp_ms);
            trajectory_builder_compute_metrics(traj_builder);
        }
        if (event_det && dets.count > 0) {
            event_detector_analyze(event_det, traj_builder, &dets,
                                   frame_num, frame.timestamp_ms);
        }

        /* Adaptive sampling feedback */
        frame_sampler_notify_activity(sampler, dets.count);

        /* Catalog */
        if (dets.count > 0) {
            catalog_builder_add(cat_builder, &dets);
            add_detections(result, &dets);
            result->metrics.total_detections += dets.count;
        }

        result->frames_processed++;
        double frame_total = vp_timer_elapsed_ms(&t_frame);
        metrics_record(&result->metrics.frame_total_ms, frame_total);

        /* Progress log every 100 frames */
        if (result->frames_processed % 100 == 0) {
            LOG_INFO(jid, "pipeline", "Progress: %d frames, %d detections, "
                     "avg %.1f ms/frame",
                     result->frames_processed, result->metrics.total_detections,
                     metrics_avg(&result->metrics.frame_total_ms));
        }

        frame_num++;
    }

    result->metrics.job_end_ms = vp_now_ms();
    result->metrics.frames_processed = result->frames_processed;

    /* Build catalog */
    catalog_builder_build(cat_builder, &result->catalog);

    /* Finalize event detection */
    if (event_det) {
        event_detector_finalize(event_det);
        const EventList *evts = event_detector_events(event_det);
        if (evts && evts->count > 0) {
            result->events.count = evts->count;
            result->events.capacity = evts->count;
            result->events.events = malloc(evts->count * sizeof(AccidentEvent));
            memcpy(result->events.events, evts->events, evts->count * sizeof(AccidentEvent));
            result->accident_detected = true;

            LOG_INFO(jid, "pipeline", "EVENTS DETECTED: %d total "
                     "(collisions=%d, near_miss=%d, stopped=%d, brake=%d)",
                     evts->count,
                     event_detector_events_by_type(event_det, EVENT_COLLISION),
                     event_detector_events_by_type(event_det, EVENT_NEAR_MISS),
                     event_detector_events_by_type(event_det, EVENT_STOPPED_VEHICLE),
                     event_detector_events_by_type(event_det, EVENT_SUDDEN_BRAKE));
        } else {
            LOG_INFO(jid, "pipeline", "No accident events detected");
        }
    }

    LOG_INFO(jid, "pipeline", "Completed: %d frames decoded, %d processed, "
             "%d detections, %d catalog classes, %.2f FPS",
             result->frames_decoded, result->frames_processed,
             result->detection_count, result->catalog.count,
             metrics_effective_fps(&result->metrics));

    if (tracker) {
        LOG_INFO(jid, "pipeline", "Tracks: %d total, %d active",
                 tracker_total_tracks(tracker), tracker_active_tracks(tracker));
    }

    job->status = JOB_STATUS_COMPLETED;

    /* Cleanup */
    free(frame.data);
    free(tensor.data);
    free(output_buf);
    detection_list_free(&dets);
    video_decoder_close(decoder);
    frame_sampler_destroy(sampler);
    inference_destroy(backend);
    postprocess_config_free(&post_cfg);
    tracker_destroy(tracker);
    trajectory_builder_destroy(traj_builder);
    event_detector_destroy(event_det);
    catalog_builder_destroy(cat_builder);

    return VP_OK;
}

void pipeline_result_free(PipelineResult *result) {
    if (!result) return;
    free(result->all_detections);
    free(result->events.events);
    catalog_free(&result->catalog);
    metrics_free(&result->metrics);
    memset(result, 0, sizeof(PipelineResult));
}
