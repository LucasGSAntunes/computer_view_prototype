#include "event_detector.h"
#include "core/common/logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <uuid/uuid.h>

/* ── Internal candidate event (before consolidation) ── */
typedef struct {
    EventType type;
    int       frame;
    double    timestamp_ms;
    int       track_a;
    int       track_b;    /* -1 if single-object event */
    float     score;
    float     overlap;
    float     speed_delta;
    float     direction_delta;
    float     proximity;
    float     loc_x, loc_y, loc_w, loc_h;
} CandidateEvent;

struct EventDetector {
    EventDetectorConfig config;
    CandidateEvent     *candidates;
    int                 cand_count;
    int                 cand_capacity;
    EventList           events;
    bool                finalized;
};

/* ── Utility ── */

const char *event_type_str(EventType type) {
    switch (type) {
    case EVENT_NONE:            return "none";
    case EVENT_COLLISION:       return "collision";
    case EVENT_ROLLOVER:        return "rollover";
    case EVENT_STOPPED_VEHICLE: return "stopped_vehicle";
    case EVENT_NEAR_MISS:       return "near_miss";
    case EVENT_DEBRIS:          return "debris";
    case EVENT_SUDDEN_BRAKE:    return "sudden_brake";
    case EVENT_WRONG_WAY:       return "wrong_way";
    }
    return "unknown";
}

const char *severity_str(SeverityLevel level) {
    switch (level) {
    case SEVERITY_LOW:      return "low";
    case SEVERITY_MEDIUM:   return "medium";
    case SEVERITY_HIGH:     return "high";
    case SEVERITY_CRITICAL: return "critical";
    }
    return "unknown";
}

/* ── Config presets ── */

EventDetectorConfig event_detector_config_default(void) {
    return (EventDetectorConfig){
        .enabled                   = true,
        .collision_iou_threshold   = 0.01f,
        .collision_speed_drop      = 0.30f,
        .near_miss_distance        = 15.0f,
        .stopped_speed_threshold   = 1.0f,
        .stopped_min_frames        = 120,
        .sudden_brake_decel        = 99999.0f,  /* disabled — too noisy with IoU tracker */
        .event_confidence_threshold = 0.55f,
        .temporal_window           = 10,
        .w_overlap                 = 0.45f,
        .w_speed_change            = 0.25f,
        .w_direction_change        = 0.10f,
        .w_proximity               = 0.20f,
    };
}

EventDetectorConfig event_detector_config_safety_critical(void) {
    return (EventDetectorConfig){
        .enabled                   = true,
        .collision_iou_threshold   = 0.01f,
        .collision_speed_drop      = 0.25f,
        .near_miss_distance        = 60.0f,
        .stopped_speed_threshold   = 5.0f,
        .stopped_min_frames        = 30,
        .sudden_brake_decel        = 100.0f,
        .event_confidence_threshold = 0.15f,
        .temporal_window           = 20,
        .w_overlap                 = 0.25f,
        .w_speed_change            = 0.30f,
        .w_direction_change        = 0.20f,
        .w_proximity               = 0.25f,
    };
}

/* ── Create / Destroy ── */

EventDetector *event_detector_create(const EventDetectorConfig *config) {
    EventDetector *ed = calloc(1, sizeof(EventDetector));
    if (!ed) return NULL;
    ed->config = *config;
    ed->cand_capacity = 512;
    ed->candidates = calloc(ed->cand_capacity, sizeof(CandidateEvent));
    ed->events.capacity = 64;
    ed->events.events = calloc(ed->events.capacity, sizeof(AccidentEvent));
    return ed;
}

static void add_candidate(EventDetector *ed, const CandidateEvent *c) {
    if (ed->cand_count >= ed->cand_capacity) {
        ed->cand_capacity *= 2;
        ed->candidates = realloc(ed->candidates,
                                  ed->cand_capacity * sizeof(CandidateEvent));
    }
    ed->candidates[ed->cand_count++] = *c;
}

/* ── IoU between two bboxes ── */

static float bbox_iou(float ax, float ay, float aw, float ah,
                       float bx, float by, float bw, float bh) {
    float x1 = fmaxf(ax, bx);
    float y1 = fmaxf(ay, by);
    float x2 = fminf(ax + aw, bx + bw);
    float y2 = fminf(ay + ah, by + bh);
    float iw = fmaxf(0, x2 - x1);
    float ih = fmaxf(0, y2 - y1);
    float inter = iw * ih;
    float u = aw * ah + bw * bh - inter;
    return u > 0 ? inter / u : 0;
}

/* Distance between bbox centers */
static float center_distance(float ax, float ay, float aw, float ah,
                               float bx, float by, float bw, float bh) {
    float dx = (ax + aw / 2) - (bx + bw / 2);
    float dy = (ay + ah / 2) - (by + bh / 2);
    return sqrtf(dx * dx + dy * dy);
}

/* ── Vehicle class check ── */
static bool is_vehicle(int class_id) {
    /* COCO: 2=car, 3=motorcycle, 5=bus, 7=truck */
    return class_id == 2 || class_id == 3 || class_id == 5 || class_id == 7;
}

/* ── Per-frame analysis ── */

VPError event_detector_analyze(EventDetector *ed,
                                const TrajectoryBuilder *tb,
                                const DetectionList *detections,
                                int frame_number, double timestamp_ms) {
    if (!ed || !tb || !detections || !ed->config.enabled) return VP_OK;

    const TrajectoryStore *store = trajectory_builder_store(tb);
    if (!store) return VP_OK;

    /* === Check 1: Collision / Near-miss between pairs === */
    for (int i = 0; i < detections->count; i++) {
        const Detection *a = &detections->items[i];
        if (a->track_id <= 0 || !is_vehicle(a->class_id)) continue;

        const ObjectTrajectory *traj_a = trajectory_builder_get(tb, a->track_id);
        if (!traj_a || traj_a->count < 3) continue;

        for (int j = i + 1; j < detections->count; j++) {
            const Detection *b = &detections->items[j];
            if (b->track_id <= 0 || !is_vehicle(b->class_id)) continue;

            const ObjectTrajectory *traj_b = trajectory_builder_get(tb, b->track_id);
            if (!traj_b || traj_b->count < 3) continue;

            float iou_val = bbox_iou(a->bbox.x, a->bbox.y, a->bbox.w, a->bbox.h,
                                      b->bbox.x, b->bbox.y, b->bbox.w, b->bbox.h);
            float dist = center_distance(a->bbox.x, a->bbox.y, a->bbox.w, a->bbox.h,
                                          b->bbox.x, b->bbox.y, b->bbox.w, b->bbox.h);

            /* Current speeds */
            float speed_a = traj_a->count > 0 ? traj_a->points[traj_a->count - 1].speed : 0;
            float speed_b = traj_b->count > 0 ? traj_b->points[traj_b->count - 1].speed : 0;

            /* Speed drop: compare current to average */
            float drop_a = (traj_a->avg_speed > 0) ? 1.0f - (speed_a / traj_a->avg_speed) : 0;
            float drop_b = (traj_b->avg_speed > 0) ? 1.0f - (speed_b / traj_b->avg_speed) : 0;
            float max_drop = fmaxf(drop_a, drop_b);

            /* Direction difference */
            float dir_a = traj_a->count > 0 ? traj_a->points[traj_a->count - 1].direction : 0;
            float dir_b = traj_b->count > 0 ? traj_b->points[traj_b->count - 1].direction : 0;
            float dir_diff = fabsf(dir_a - dir_b);
            if (dir_diff > M_PI) dir_diff = 2.0f * (float)M_PI - dir_diff;

            /* === Collision detection === */
            /* Collision detection: IoU/proximity + speed drop + post-event stop
             * The key insight: in a real collision, at least one vehicle STOPS
             * or dramatically slows down after the overlap event.
             * In normal traffic, vehicles maintain speed after passing. */
            float max_dist = fmaxf(traj_a->total_distance, traj_b->total_distance);
            float min_bbox_dim = fminf(
                fminf(a->bbox.w, a->bbox.h),
                fminf(b->bbox.w, b->bbox.h));
            float rel_dist = (min_bbox_dim > 0) ? dist / min_bbox_dim : dist;
            bool close_proximity = (rel_dist < 2.5f);
            bool has_overlap = (iou_val >= ed->config.collision_iou_threshold);
            bool has_speed_drop = (max_drop >= ed->config.collision_speed_drop);
            bool has_movement = (max_dist > 30.0f);

            /* Check if either vehicle is near-stopped (post-collision behavior) */
            bool post_stop_a = (speed_a < 5.0f && traj_a->avg_speed > 10.0f);
            bool post_stop_b = (speed_b < 5.0f && traj_b->avg_speed > 10.0f);
            bool has_post_stop = post_stop_a || post_stop_b;

            if ((has_overlap || close_proximity) && has_speed_drop && has_movement) {
                /* Boost score if post-collision stop is observed */
                float stop_bonus = has_post_stop ? 0.20f : 0.0f;

                float proximity_score = fmaxf(0, 1.0f - rel_dist / 5.0f);
                float score = ed->config.w_overlap * fminf(iou_val * 5.0f, 1.0f) +
                              ed->config.w_speed_change * fminf(max_drop, 1.0f) +
                              ed->config.w_direction_change * fminf(dir_diff / (float)M_PI, 1.0f) +
                              ed->config.w_proximity * proximity_score +
                              stop_bonus;

                if (score >= ed->config.event_confidence_threshold) {
                    CandidateEvent c = {0};
                    c.type = EVENT_COLLISION;
                    c.frame = frame_number;
                    c.timestamp_ms = timestamp_ms;
                    c.track_a = a->track_id;
                    c.track_b = b->track_id;
                    c.score = score;
                    c.overlap = iou_val;
                    c.speed_delta = max_drop;
                    c.direction_delta = dir_diff;
                    c.proximity = dist;
                    /* Event region: union of both bboxes */
                    c.loc_x = fminf(a->bbox.x, b->bbox.x);
                    c.loc_y = fminf(a->bbox.y, b->bbox.y);
                    c.loc_w = fmaxf(a->bbox.x + a->bbox.w, b->bbox.x + b->bbox.w) - c.loc_x;
                    c.loc_h = fmaxf(a->bbox.y + a->bbox.h, b->bbox.y + b->bbox.h) - c.loc_y;
                    add_candidate(ed, &c);
                }
            }

            /* === Near-miss detection === */
            if (dist < ed->config.near_miss_distance &&
                iou_val < ed->config.collision_iou_threshold &&
                (speed_a > 20.0f || speed_b > 20.0f)) {

                float score = ed->config.w_proximity * fmaxf(0, 1.0f - dist / ed->config.near_miss_distance) +
                              ed->config.w_speed_change * fminf(fmaxf(speed_a, speed_b) / 200.0f, 1.0f) +
                              ed->config.w_direction_change * fminf(dir_diff / (float)M_PI, 1.0f);

                if (score >= ed->config.event_confidence_threshold) {
                    CandidateEvent c = {0};
                    c.type = EVENT_NEAR_MISS;
                    c.frame = frame_number;
                    c.timestamp_ms = timestamp_ms;
                    c.track_a = a->track_id;
                    c.track_b = b->track_id;
                    c.score = score;
                    c.proximity = dist;
                    c.loc_x = fminf(a->bbox.x, b->bbox.x);
                    c.loc_y = fminf(a->bbox.y, b->bbox.y);
                    c.loc_w = fmaxf(a->bbox.x + a->bbox.w, b->bbox.x + b->bbox.w) - c.loc_x;
                    c.loc_h = fmaxf(a->bbox.y + a->bbox.h, b->bbox.y + b->bbox.h) - c.loc_y;
                    add_candidate(ed, &c);
                }
            }
        }

        /* === Sudden brake detection (single vehicle) === */
        if (traj_a->count >= 3) {
            TrajectoryPoint *last = &traj_a->points[traj_a->count - 1];
            float decel = sqrtf(last->ax * last->ax + last->ay * last->ay);
            /* Deceleration = negative acceleration in direction of motion */
            float speed_dot_accel = last->vx * last->ax + last->vy * last->ay;

            if (decel > ed->config.sudden_brake_decel && speed_dot_accel < 0) {
                float score = fminf(decel / (ed->config.sudden_brake_decel * 3.0f), 1.0f);
                if (score >= ed->config.event_confidence_threshold) {
                    CandidateEvent c = {0};
                    c.type = EVENT_SUDDEN_BRAKE;
                    c.frame = frame_number;
                    c.timestamp_ms = timestamp_ms;
                    c.track_a = a->track_id;
                    c.track_b = -1;
                    c.score = score;
                    c.speed_delta = decel;
                    c.loc_x = a->bbox.x;
                    c.loc_y = a->bbox.y;
                    c.loc_w = a->bbox.w;
                    c.loc_h = a->bbox.h;
                    add_candidate(ed, &c);
                }
            }
        }
    }

    /* === Check 2: Stopped vehicle === */
    for (int i = 0; i < store->count; i++) {
        const ObjectTrajectory *traj = &store->items[i];
        if (!is_vehicle(traj->class_id)) continue;
        if (traj->count < ed->config.stopped_min_frames) continue;

        /* Check recent N frames for stationarity */
        int window = ed->config.stopped_min_frames;
        int start = traj->count - window;
        if (start < 0) start = 0;

        bool all_slow = true;
        for (int j = start; j < traj->count; j++) {
            if (traj->points[j].speed > ed->config.stopped_speed_threshold) {
                all_slow = false;
                break;
            }
        }

        if (all_slow && traj->points[traj->count - 1].frame_number == frame_number) {
            float score = fminf((float)window / (float)(ed->config.stopped_min_frames * 2), 1.0f);
            CandidateEvent c = {0};
            c.type = EVENT_STOPPED_VEHICLE;
            c.frame = frame_number;
            c.timestamp_ms = timestamp_ms;
            c.track_a = traj->track_id;
            c.track_b = -1;
            c.score = score;
            TrajectoryPoint *last = &traj->points[traj->count - 1];
            c.loc_x = last->cx - last->w / 2;
            c.loc_y = last->cy - last->h / 2;
            c.loc_w = last->w;
            c.loc_h = last->h;
            add_candidate(ed, &c);
        }
    }

    return VP_OK;
}

/* ── Consolidation: merge consecutive candidates into single events ── */

static void generate_event_id(char *buf, size_t len) {
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, buf);
    if (len < 37) buf[len - 1] = '\0';
}

static SeverityLevel score_to_severity(float score) {
    if (score >= 0.75f) return SEVERITY_CRITICAL;
    if (score >= 0.50f) return SEVERITY_HIGH;
    if (score >= 0.30f) return SEVERITY_MEDIUM;
    return SEVERITY_LOW;
}

static void add_event(EventList *list, const AccidentEvent *ev) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 32;
        list->events = realloc(list->events, list->capacity * sizeof(AccidentEvent));
    }
    list->events[list->count++] = *ev;
}

VPError event_detector_finalize(EventDetector *ed) {
    if (!ed) return VP_ERR_INVALID_ARG;

    /* Sort candidates by frame */
    for (int i = 0; i < ed->cand_count - 1; i++) {
        for (int j = i + 1; j < ed->cand_count; j++) {
            if (ed->candidates[j].frame < ed->candidates[i].frame) {
                CandidateEvent tmp = ed->candidates[i];
                ed->candidates[i] = ed->candidates[j];
                ed->candidates[j] = tmp;
            }
        }
    }

    /* Group consecutive candidates by (type, track_a, track_b) */
    bool *used = calloc(ed->cand_count, sizeof(bool));

    for (int i = 0; i < ed->cand_count; i++) {
        if (used[i]) continue;

        CandidateEvent *seed = &ed->candidates[i];

        /* Collect consecutive frames with same type and objects */
        int start_frame = seed->frame;
        int end_frame = seed->frame;
        double start_ms = seed->timestamp_ms;
        double end_ms = seed->timestamp_ms;
        float max_score = seed->score;
        float sum_overlap = seed->overlap;
        float sum_speed = seed->speed_delta;
        float sum_dir = seed->direction_delta;
        float sum_prox = seed->proximity;
        int group_count = 1;
        used[i] = true;

        /* Region union */
        float rx = seed->loc_x, ry = seed->loc_y;
        float rx2 = seed->loc_x + seed->loc_w;
        float ry2 = seed->loc_y + seed->loc_h;

        for (int j = i + 1; j < ed->cand_count; j++) {
            if (used[j]) continue;
            CandidateEvent *c = &ed->candidates[j];

            /* Same type and same objects, within temporal window */
            if (c->type == seed->type &&
                c->track_a == seed->track_a &&
                c->track_b == seed->track_b &&
                (c->frame - end_frame) <= ed->config.temporal_window) {

                end_frame = c->frame;
                end_ms = c->timestamp_ms;
                if (c->score > max_score) max_score = c->score;
                sum_overlap += c->overlap;
                sum_speed += c->speed_delta;
                sum_dir += c->direction_delta;
                sum_prox += c->proximity;
                group_count++;
                used[j] = true;

                if (c->loc_x < rx) rx = c->loc_x;
                if (c->loc_y < ry) ry = c->loc_y;
                if (c->loc_x + c->loc_w > rx2) rx2 = c->loc_x + c->loc_w;
                if (c->loc_y + c->loc_h > ry2) ry2 = c->loc_y + c->loc_h;
            }
        }

        /* Require collision events to span at least 2 frames to confirm */
        if (seed->type == EVENT_COLLISION && group_count < 2)
            continue;

        /* Build consolidated event */
        AccidentEvent ev = {0};
        generate_event_id(ev.event_id, sizeof(ev.event_id));
        ev.type         = seed->type;
        ev.confidence   = max_score;
        ev.severity     = score_to_severity(max_score);
        ev.start_frame  = start_frame;
        ev.end_frame    = end_frame;
        ev.start_ms     = start_ms;
        ev.end_ms       = end_ms;
        ev.location_x   = rx;
        ev.location_y   = ry;
        ev.location_w   = rx2 - rx;
        ev.location_h   = ry2 - ry;
        ev.score_overlap         = group_count > 0 ? sum_overlap / group_count : 0;
        ev.score_speed_change    = group_count > 0 ? sum_speed / group_count : 0;
        ev.score_direction_change = group_count > 0 ? sum_dir / group_count : 0;
        ev.score_proximity       = group_count > 0 ? sum_prox / group_count : 0;
        ev.composite_score       = max_score;

        /* Involved objects */
        ev.involved[0].track_id = seed->track_a;
        ev.involved[0].class_id = -1; /* filled from trajectory if available */
        ev.involved_count = 1;
        if (seed->track_b > 0) {
            ev.involved[1].track_id = seed->track_b;
            ev.involved[1].class_id = -1;
            ev.involved_count = 2;
        }

        add_event(&ed->events, &ev);

        LOG_INFO(NULL, "event", "%s detected: frames %d-%d, score=%.2f, severity=%s, tracks=[%d%s%d]",
                 event_type_str(ev.type), ev.start_frame, ev.end_frame,
                 ev.composite_score, severity_str(ev.severity),
                 seed->track_a, seed->track_b > 0 ? "," : "", seed->track_b > 0 ? seed->track_b : 0);
    }

    free(used);
    ed->finalized = true;
    return VP_OK;
}

/* ── Getters ── */

const EventList *event_detector_events(const EventDetector *ed) {
    return ed ? &ed->events : NULL;
}

int event_detector_total_events(const EventDetector *ed) {
    return ed ? ed->events.count : 0;
}

int event_detector_events_by_type(const EventDetector *ed, EventType type) {
    if (!ed) return 0;
    int count = 0;
    for (int i = 0; i < ed->events.count; i++) {
        if (ed->events.events[i].type == type) count++;
    }
    return count;
}

/* ── Export JSON ── */

VPError event_detector_export_json(const EventDetector *ed, const char *path) {
    if (!ed || !path) return VP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return VP_ERR_EXPORT_FAILED;

    fprintf(f, "{\n  \"total_events\": %d,\n", ed->events.count);
    fprintf(f, "  \"by_type\": {\n");
    fprintf(f, "    \"collision\": %d,\n", event_detector_events_by_type(ed, EVENT_COLLISION));
    fprintf(f, "    \"near_miss\": %d,\n", event_detector_events_by_type(ed, EVENT_NEAR_MISS));
    fprintf(f, "    \"stopped_vehicle\": %d,\n", event_detector_events_by_type(ed, EVENT_STOPPED_VEHICLE));
    fprintf(f, "    \"sudden_brake\": %d,\n", event_detector_events_by_type(ed, EVENT_SUDDEN_BRAKE));
    fprintf(f, "    \"rollover\": %d\n", event_detector_events_by_type(ed, EVENT_ROLLOVER));
    fprintf(f, "  },\n");
    fprintf(f, "  \"events\": [\n");

    for (int i = 0; i < ed->events.count; i++) {
        const AccidentEvent *ev = &ed->events.events[i];
        fprintf(f, "    {\n"
                   "      \"event_id\": \"%s\",\n"
                   "      \"type\": \"%s\",\n"
                   "      \"severity\": \"%s\",\n"
                   "      \"confidence\": %.4f,\n"
                   "      \"start_frame\": %d,\n"
                   "      \"end_frame\": %d,\n"
                   "      \"start_ms\": %.2f,\n"
                   "      \"end_ms\": %.2f,\n"
                   "      \"duration_ms\": %.2f,\n"
                   "      \"location\": {\"x\": %.1f, \"y\": %.1f, \"w\": %.1f, \"h\": %.1f},\n"
                   "      \"scores\": {\"overlap\": %.4f, \"speed_change\": %.4f, \"direction\": %.4f, \"proximity\": %.4f},\n"
                   "      \"involved_tracks\": [",
                ev->event_id, event_type_str(ev->type), severity_str(ev->severity),
                ev->confidence, ev->start_frame, ev->end_frame,
                ev->start_ms, ev->end_ms, ev->end_ms - ev->start_ms,
                ev->location_x, ev->location_y, ev->location_w, ev->location_h,
                ev->score_overlap, ev->score_speed_change,
                ev->score_direction_change, ev->score_proximity);

        for (int j = 0; j < ev->involved_count; j++) {
            fprintf(f, "%d%s", ev->involved[j].track_id,
                    j < ev->involved_count - 1 ? ", " : "");
        }
        fprintf(f, "]\n    }%s\n", i < ed->events.count - 1 ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);
    LOG_INFO(NULL, "event", "Events JSON: %s (%d events)", path, ed->events.count);
    return VP_OK;
}

VPError event_detector_export_csv(const EventDetector *ed, const char *path) {
    if (!ed || !path) return VP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return VP_ERR_EXPORT_FAILED;

    fprintf(f, "event_id,type,severity,confidence,start_frame,end_frame,"
               "start_ms,end_ms,duration_ms,loc_x,loc_y,loc_w,loc_h,involved_tracks\n");

    for (int i = 0; i < ed->events.count; i++) {
        const AccidentEvent *ev = &ed->events.events[i];
        fprintf(f, "%s,%s,%s,%.4f,%d,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,",
                ev->event_id, event_type_str(ev->type), severity_str(ev->severity),
                ev->confidence, ev->start_frame, ev->end_frame,
                ev->start_ms, ev->end_ms, ev->end_ms - ev->start_ms,
                ev->location_x, ev->location_y, ev->location_w, ev->location_h);
        for (int j = 0; j < ev->involved_count; j++) {
            fprintf(f, "%d%s", ev->involved[j].track_id,
                    j < ev->involved_count - 1 ? ";" : "");
        }
        fprintf(f, "\n");
    }

    fclose(f);
    return VP_OK;
}

/* ── Reset / Destroy ── */

void event_detector_reset(EventDetector *ed) {
    if (!ed) return;
    ed->cand_count = 0;
    ed->events.count = 0;
    ed->finalized = false;
}

void event_detector_destroy(EventDetector *ed) {
    if (!ed) return;
    free(ed->candidates);
    free(ed->events.events);
    free(ed);
}
