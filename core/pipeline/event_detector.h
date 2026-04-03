#ifndef VP_EVENT_DETECTOR_H
#define VP_EVENT_DETECTOR_H

#include "core/common/types.h"
#include "core/common/errors.h"
#include "trajectory_builder.h"

typedef struct EventDetector EventDetector;

/* Create default config tuned for road accident detection. */
EventDetectorConfig event_detector_config_default(void);

/* Config preset for safety-critical profile (lower thresholds, higher sensitivity). */
EventDetectorConfig event_detector_config_safety_critical(void);

/* Create event detector. */
EventDetector *event_detector_create(const EventDetectorConfig *config);

/* Analyze current frame given trajectories and detections.
 * Should be called each processed frame. Accumulates candidate events internally.
 * frame_number and timestamp_ms identify the current frame. */
VPError event_detector_analyze(EventDetector *ed,
                                const TrajectoryBuilder *tb,
                                const DetectionList *detections,
                                int frame_number, double timestamp_ms);

/* Finalize: consolidate overlapping/consecutive candidate events into
 * distinct events. Call after processing all frames. */
VPError event_detector_finalize(EventDetector *ed);

/* Get detected events. */
const EventList *event_detector_events(const EventDetector *ed);

/* Export events to JSON. */
VPError event_detector_export_json(const EventDetector *ed, const char *path);

/* Export events to CSV. */
VPError event_detector_export_csv(const EventDetector *ed, const char *path);

/* Stats */
int event_detector_total_events(const EventDetector *ed);
int event_detector_events_by_type(const EventDetector *ed, EventType type);

void event_detector_reset(EventDetector *ed);
void event_detector_destroy(EventDetector *ed);

/* Utility: event type to string */
const char *event_type_str(EventType type);
const char *severity_str(SeverityLevel level);

#endif /* VP_EVENT_DETECTOR_H */
