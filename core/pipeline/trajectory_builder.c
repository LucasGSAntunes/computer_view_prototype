#include "trajectory_builder.h"
#include "core/common/logging.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct TrajectoryBuilder {
    TrajectoryStore store;
    double          fps;
    double          dt;   /* seconds per frame */
};

TrajectoryBuilder *trajectory_builder_create(double fps) {
    TrajectoryBuilder *tb = calloc(1, sizeof(TrajectoryBuilder));
    if (!tb) return NULL;
    tb->fps = fps > 0 ? fps : 30.0;
    tb->dt  = 1.0 / tb->fps;
    tb->store.capacity = 256;
    tb->store.items = calloc(tb->store.capacity, sizeof(ObjectTrajectory));
    return tb;
}

static ObjectTrajectory *find_or_create(TrajectoryBuilder *tb, int track_id,
                                         int class_id, const char *class_name) {
    /* Find existing */
    for (int i = 0; i < tb->store.count; i++) {
        if (tb->store.items[i].track_id == track_id)
            return &tb->store.items[i];
    }

    /* Create new */
    if (tb->store.count >= tb->store.capacity) {
        tb->store.capacity *= 2;
        tb->store.items = realloc(tb->store.items,
                                   tb->store.capacity * sizeof(ObjectTrajectory));
    }

    ObjectTrajectory *traj = &tb->store.items[tb->store.count++];
    memset(traj, 0, sizeof(ObjectTrajectory));
    traj->track_id = track_id;
    traj->class_id = class_id;
    if (class_name)
        strncpy(traj->class_name, class_name, sizeof(traj->class_name) - 1);
    traj->capacity = 128;
    traj->points = calloc(traj->capacity, sizeof(TrajectoryPoint));
    return traj;
}

static void add_point(ObjectTrajectory *traj, const TrajectoryPoint *pt) {
    if (traj->count >= traj->capacity) {
        traj->capacity *= 2;
        traj->points = realloc(traj->points, traj->capacity * sizeof(TrajectoryPoint));
    }
    traj->points[traj->count++] = *pt;
}

VPError trajectory_builder_update(TrajectoryBuilder *tb,
                                   const DetectionList *detections,
                                   int frame_number, double timestamp_ms) {
    if (!tb || !detections) return VP_ERR_INVALID_ARG;

    for (int i = 0; i < detections->count; i++) {
        const Detection *det = &detections->items[i];
        if (det->track_id <= 0) continue;

        ObjectTrajectory *traj = find_or_create(tb, det->track_id,
                                                 det->class_id, det->class_name);

        TrajectoryPoint pt = {0};
        pt.frame_number = frame_number;
        pt.timestamp_ms = timestamp_ms;
        pt.cx = det->bbox.x + det->bbox.w / 2.0f;
        pt.cy = det->bbox.y + det->bbox.h / 2.0f;
        pt.w  = det->bbox.w;
        pt.h  = det->bbox.h;

        /* Compute instantaneous velocity from previous point */
        if (traj->count > 0) {
            TrajectoryPoint *prev = &traj->points[traj->count - 1];
            double dt_ms = pt.timestamp_ms - prev->timestamp_ms;
            if (dt_ms > 0) {
                double dt_s = dt_ms / 1000.0;
                pt.vx = (float)((pt.cx - prev->cx) / dt_s);
                pt.vy = (float)((pt.cy - prev->cy) / dt_s);
                pt.speed = sqrtf(pt.vx * pt.vx + pt.vy * pt.vy);
                pt.direction = atan2f(pt.vy, pt.vx);

                /* Acceleration */
                pt.ax = (float)((pt.vx - prev->vx) / dt_s);
                pt.ay = (float)((pt.vy - prev->vy) / dt_s);
            }
        }

        add_point(traj, &pt);
    }

    return VP_OK;
}

void trajectory_builder_compute_metrics(TrajectoryBuilder *tb) {
    if (!tb) return;

    for (int i = 0; i < tb->store.count; i++) {
        ObjectTrajectory *traj = &tb->store.items[i];
        if (traj->count < 2) {
            traj->is_stationary = true;
            continue;
        }

        float sum_speed = 0;
        float max_speed = 0;
        float total_dist = 0;
        int speed_count = 0;

        for (int j = 1; j < traj->count; j++) {
            TrajectoryPoint *pt = &traj->points[j];
            TrajectoryPoint *prev = &traj->points[j - 1];

            float dx = pt->cx - prev->cx;
            float dy = pt->cy - prev->cy;
            float dist = sqrtf(dx * dx + dy * dy);
            total_dist += dist;

            if (pt->speed > 0) {
                sum_speed += pt->speed;
                speed_count++;
                if (pt->speed > max_speed)
                    max_speed = pt->speed;
            }
        }

        traj->avg_speed = speed_count > 0 ? sum_speed / speed_count : 0;
        traj->max_speed = max_speed;
        traj->total_distance = total_dist;
        traj->is_stationary = (traj->avg_speed < 3.0f && total_dist < 20.0f);
    }
}

const ObjectTrajectory *trajectory_builder_get(const TrajectoryBuilder *tb, int track_id) {
    if (!tb) return NULL;
    for (int i = 0; i < tb->store.count; i++) {
        if (tb->store.items[i].track_id == track_id)
            return &tb->store.items[i];
    }
    return NULL;
}

const TrajectoryStore *trajectory_builder_store(const TrajectoryBuilder *tb) {
    return tb ? &tb->store : NULL;
}

float trajectory_get_current_speed(const TrajectoryBuilder *tb, int track_id) {
    const ObjectTrajectory *t = trajectory_builder_get(tb, track_id);
    if (!t || t->count == 0) return 0;
    return t->points[t->count - 1].speed;
}

float trajectory_get_current_accel(const TrajectoryBuilder *tb, int track_id) {
    const ObjectTrajectory *t = trajectory_builder_get(tb, track_id);
    if (!t || t->count == 0) return 0;
    TrajectoryPoint *last = &t->points[t->count - 1];
    return sqrtf(last->ax * last->ax + last->ay * last->ay);
}

void trajectory_builder_reset(TrajectoryBuilder *tb) {
    if (!tb) return;
    for (int i = 0; i < tb->store.count; i++)
        free(tb->store.items[i].points);
    tb->store.count = 0;
}

void trajectory_builder_destroy(TrajectoryBuilder *tb) {
    if (!tb) return;
    for (int i = 0; i < tb->store.count; i++)
        free(tb->store.items[i].points);
    free(tb->store.items);
    free(tb);
}
