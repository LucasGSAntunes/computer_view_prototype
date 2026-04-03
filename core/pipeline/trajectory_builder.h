#ifndef VP_TRAJECTORY_BUILDER_H
#define VP_TRAJECTORY_BUILDER_H

#include "core/common/types.h"
#include "core/common/errors.h"

typedef struct TrajectoryBuilder TrajectoryBuilder;

/* Create builder. fps is needed to convert frame deltas to time-based velocity. */
TrajectoryBuilder *trajectory_builder_create(double fps);

/* Feed detections from a frame. Must be called in frame order.
 * Detections should already have track_id assigned by tracker. */
VPError trajectory_builder_update(TrajectoryBuilder *tb,
                                   const DetectionList *detections,
                                   int frame_number, double timestamp_ms);

/* Get trajectory for a specific track_id. Returns NULL if not found. */
const ObjectTrajectory *trajectory_builder_get(const TrajectoryBuilder *tb, int track_id);

/* Get full store (all trajectories). */
const TrajectoryStore *trajectory_builder_store(const TrajectoryBuilder *tb);

/* Compute derived metrics (speed, accel, direction) for all trajectories.
 * Call after all frames are processed, or periodically. */
void trajectory_builder_compute_metrics(TrajectoryBuilder *tb);

/* Get current speed of a track (px/s). Returns 0 if unknown. */
float trajectory_get_current_speed(const TrajectoryBuilder *tb, int track_id);

/* Get current acceleration of a track (px/s^2). */
float trajectory_get_current_accel(const TrajectoryBuilder *tb, int track_id);

void trajectory_builder_reset(TrajectoryBuilder *tb);
void trajectory_builder_destroy(TrajectoryBuilder *tb);

#endif /* VP_TRAJECTORY_BUILDER_H */
