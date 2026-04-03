#include "tracker.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int   track_id;
    int   class_id;
    BBox  bbox;
    float confidence;
    int   age;          /* frames since last match */
    int   hits;         /* total matched frames */
    bool  active;
} Track;

struct Tracker {
    TrackerConfig config;
    Track        *tracks;
    int           track_count;
    int           track_capacity;
    int           next_id;
    int           active_count;
};

static float iou(const BBox *a, const BBox *b) {
    float x1 = fmaxf(a->x, b->x);
    float y1 = fmaxf(a->y, b->y);
    float x2 = fminf(a->x + a->w, b->x + b->w);
    float y2 = fminf(a->y + a->h, b->y + b->h);
    float iw = fmaxf(0, x2 - x1);
    float ih = fmaxf(0, y2 - y1);
    float inter = iw * ih;
    float a_area = a->w * a->h;
    float b_area = b->w * b->h;
    float u = a_area + b_area - inter;
    return (u > 0) ? inter / u : 0;
}

Tracker *tracker_create(const TrackerConfig *config) {
    Tracker *t = calloc(1, sizeof(Tracker));
    if (!t) return NULL;
    t->config = *config;
    t->track_capacity = 256;
    t->tracks = calloc(t->track_capacity, sizeof(Track));
    t->next_id = 1;
    return t;
}

int tracker_update(Tracker *tracker, DetectionList *detections, int frame_number) {
    (void)frame_number;
    if (!tracker || !detections) return 0;

    int n_det = detections->count;
    int n_trk = tracker->track_count;

    /* Boolean arrays for matched status */
    bool *det_matched = calloc(n_det, sizeof(bool));
    bool *trk_matched = calloc(n_trk, sizeof(bool));

    /* Greedy matching: for each detection, find best matching active track */
    for (int d = 0; d < n_det; d++) {
        float best_iou = tracker->config.iou_threshold;
        int   best_trk = -1;

        for (int t = 0; t < n_trk; t++) {
            if (!tracker->tracks[t].active || trk_matched[t]) continue;
            if (tracker->tracks[t].class_id != detections->items[d].class_id) continue;

            float score = iou(&tracker->tracks[t].bbox, &detections->items[d].bbox);
            if (score > best_iou) {
                best_iou = score;
                best_trk = t;
            }
        }

        if (best_trk >= 0) {
            /* Update existing track */
            Track *trk = &tracker->tracks[best_trk];
            trk->bbox       = detections->items[d].bbox;
            trk->confidence = detections->items[d].confidence;
            trk->age        = 0;
            trk->hits++;
            trk_matched[best_trk] = true;
            det_matched[d] = true;

            detections->items[d].track_id = trk->track_id;
        }
    }

    /* Age unmatched tracks */
    for (int t = 0; t < n_trk; t++) {
        if (!tracker->tracks[t].active) continue;
        if (!trk_matched[t]) {
            tracker->tracks[t].age++;
            if (tracker->tracks[t].age > tracker->config.max_age) {
                tracker->tracks[t].active = false;
                tracker->active_count--;
            }
        }
    }

    /* Create new tracks for unmatched detections */
    for (int d = 0; d < n_det; d++) {
        if (det_matched[d]) continue;

        /* Grow track array if needed */
        if (tracker->track_count >= tracker->track_capacity) {
            tracker->track_capacity *= 2;
            tracker->tracks = realloc(tracker->tracks,
                                       tracker->track_capacity * sizeof(Track));
        }

        Track new_trk = {0};
        new_trk.track_id   = tracker->next_id++;
        new_trk.class_id   = detections->items[d].class_id;
        new_trk.bbox       = detections->items[d].bbox;
        new_trk.confidence = detections->items[d].confidence;
        new_trk.age        = 0;
        new_trk.hits       = 1;
        new_trk.active     = true;

        tracker->tracks[tracker->track_count++] = new_trk;
        tracker->active_count++;

        detections->items[d].track_id = new_trk.track_id;
    }

    free(det_matched);
    free(trk_matched);

    return tracker->active_count;
}

int tracker_total_tracks(const Tracker *tracker) {
    return tracker ? (tracker->next_id - 1) : 0;
}

int tracker_active_tracks(const Tracker *tracker) {
    return tracker ? tracker->active_count : 0;
}

void tracker_reset(Tracker *tracker) {
    if (!tracker) return;
    tracker->track_count  = 0;
    tracker->active_count = 0;
    tracker->next_id      = 1;
}

void tracker_destroy(Tracker *tracker) {
    if (!tracker) return;
    free(tracker->tracks);
    free(tracker);
}
