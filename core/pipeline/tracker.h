#ifndef VP_TRACKER_H
#define VP_TRACKER_H

#include "core/common/types.h"
#include "core/common/errors.h"

typedef struct Tracker Tracker;

typedef struct {
    float iou_threshold;     /* min IoU to match detection to track */
    int   max_age;           /* frames without match before track dies */
    int   min_hits;          /* min detections before track is confirmed */
} TrackerConfig;

/* Create tracker with given configuration. */
Tracker *tracker_create(const TrackerConfig *config);

/* Update tracker with detections from current frame.
 * Assigns track_id to each detection in-place.
 * Returns number of active tracks. */
int tracker_update(Tracker *tracker, DetectionList *detections, int frame_number);

/* Get total tracks created so far (unique objects seen). */
int tracker_total_tracks(const Tracker *tracker);

/* Get currently active tracks. */
int tracker_active_tracks(const Tracker *tracker);

/* Reset tracker state. */
void tracker_reset(Tracker *tracker);

void tracker_destroy(Tracker *tracker);

#endif /* VP_TRACKER_H */
