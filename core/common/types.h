#ifndef VP_TYPES_H
#define VP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Bounding box ── */
typedef struct {
    float x;      /* top-left x (pixels) */
    float y;      /* top-left y (pixels) */
    float w;      /* width */
    float h;      /* height */
} BBox;

/* ── Single detection on a frame ── */
typedef struct {
    int      frame_number;
    double   timestamp_ms;
    int      class_id;
    char     class_name[64];
    float    confidence;
    BBox     bbox;
    int      track_id;        /* -1 when tracking disabled */
} Detection;

/* ── Detection list for one frame ── */
typedef struct {
    Detection *items;
    int        count;
    int        capacity;
} DetectionList;

/* ── Catalog entry (aggregated per class) ── */
typedef struct {
    int    class_id;
    char   class_name[64];
    int    total_detections;
    int    total_unique_tracks;
    double first_seen_ms;
    double last_seen_ms;
} CatalogEntry;

/* ── Catalog summary ── */
typedef struct {
    CatalogEntry *entries;
    int           count;
    int           capacity;
} Catalog;

/* ── Tensor descriptor ── */
typedef struct {
    float *data;
    int    dims[4];   /* NCHW */
    int    ndims;
    size_t size;      /* total floats */
} Tensor;

/* ── Frame buffer (decoded) ── */
typedef struct {
    uint8_t *data;
    int      width;
    int      height;
    int      channels;    /* 3 = RGB */
    int      stride;      /* bytes per row */
    int      frame_number;
    double   timestamp_ms;
} FrameBuffer;

/* ── Job status ── */
typedef enum {
    JOB_STATUS_RECEIVED = 0,
    JOB_STATUS_QUEUED,
    JOB_STATUS_RUNNING,
    JOB_STATUS_PARTIAL,
    JOB_STATUS_RETRYING,
    JOB_STATUS_FAILED,
    JOB_STATUS_COMPLETED,
    JOB_STATUS_CANCELLED
} JobStatus;

/* ── Sampling policy ── */
typedef enum {
    SAMPLING_ALL = 0,
    SAMPLING_INTERVAL,
    SAMPLING_ADAPTIVE
} SamplingPolicy;

/* ── Processing profile ── */
typedef struct {
    char           name[64];
    int            input_width;
    int            input_height;
    SamplingPolicy sampling_policy;
    int            sampling_interval;    /* every N frames */
    float          confidence_threshold;
    float          iou_threshold;
    int            batch_size;
    char           preferred_engine[64];
    bool           tracking_enabled;
    int            track_max_age;        /* frames before track dies */
    int            max_retries;
    int            timeout_ms;
    size_t         max_memory_mb;
} ProcessingProfile;

/* ── Inference engine metadata ── */
typedef struct {
    char engine_name[64];
    char engine_version[32];
    char backend_type[32];    /* "onnx", "tensorrt", "openvino" */
    char device_type[32];     /* "cpu", "cuda", "opencl" */
    char precision_mode[16];  /* "fp32", "fp16", "int8" */
} EngineInfo;

/* ── Runtime environment ── */
typedef struct {
    char   cpu_model[128];
    char   gpu_model[128];
    int    memory_gb;
    char   os_version[64];
    char   driver_version[64];
} RuntimeEnvironment;

/* ── Job context ── */
typedef struct {
    char                job_id[64];
    char                source_uri[512];
    JobStatus           status;
    int                 priority;
    ProcessingProfile   profile;
    EngineInfo          engine_info;
    RuntimeEnvironment  runtime_env;
    char                model_path[512];
    char                labels_path[512];
    char                output_dir[512];
} JobContext;

/* ── Stage timing ── */
typedef struct {
    double decode_ms;
    double preprocess_ms;
    double infer_ms;
    double postprocess_ms;
    double track_ms;
    double event_ms;
    double catalog_ms;
    double total_ms;
} StageTiming;

/* ══════════════════════════════════════════
 *  Event / Accident Detection Types
 * ══════════════════════════════════════════ */

/* ── Event types ── */
typedef enum {
    EVENT_NONE = 0,
    EVENT_COLLISION,           /* collision between vehicles */
    EVENT_ROLLOVER,            /* vehicle rollover */
    EVENT_STOPPED_VEHICLE,     /* vehicle stopped on active lane */
    EVENT_NEAR_MISS,           /* near-miss / close call */
    EVENT_DEBRIS,              /* anomalous object on road */
    EVENT_SUDDEN_BRAKE,        /* abrupt deceleration */
    EVENT_WRONG_WAY            /* vehicle going against flow */
} EventType;

/* ── Severity level ── */
typedef enum {
    SEVERITY_LOW = 0,
    SEVERITY_MEDIUM,
    SEVERITY_HIGH,
    SEVERITY_CRITICAL
} SeverityLevel;

/* ── Trajectory point (one position in time) ── */
typedef struct {
    int    frame_number;
    double timestamp_ms;
    float  cx, cy;             /* center of bbox */
    float  w, h;               /* bbox dimensions */
    float  vx, vy;             /* velocity (px/s) */
    float  speed;              /* magnitude of velocity */
    float  ax, ay;             /* acceleration (px/s^2) */
    float  direction;          /* angle in radians */
} TrajectoryPoint;

/* ── Object trajectory (full track history) ── */
typedef struct {
    int              track_id;
    int              class_id;
    char             class_name[64];
    TrajectoryPoint *points;
    int              count;
    int              capacity;
    float            avg_speed;
    float            max_speed;
    float            total_distance;
    bool             is_stationary;
} ObjectTrajectory;

/* ── Trajectory store (all trajectories for a job) ── */
typedef struct {
    ObjectTrajectory *items;
    int               count;
    int               capacity;
} TrajectoryStore;

/* ── Involved object in an event ── */
typedef struct {
    int   track_id;
    int   class_id;
    char  class_name[64];
    float speed_before;
    float speed_after;
    float direction_change;    /* radians */
} InvolvedObject;

/* ── Accident / Event detection ── */
typedef struct {
    char            event_id[64];
    EventType       type;
    SeverityLevel   severity;
    float           confidence;
    int             start_frame;
    int             end_frame;
    double          start_ms;
    double          end_ms;
    float           location_x;     /* center of event region */
    float           location_y;
    float           location_w;     /* bounding region of event */
    float           location_h;
    InvolvedObject  involved[8];    /* max 8 involved objects */
    int             involved_count;
    /* scoring components */
    float           score_overlap;
    float           score_speed_change;
    float           score_direction_change;
    float           score_proximity;
    float           composite_score;
} AccidentEvent;

/* ── Event list ── */
typedef struct {
    AccidentEvent *events;
    int            count;
    int            capacity;
} EventList;

/* ── Event detection config ── */
typedef struct {
    bool  enabled;
    float collision_iou_threshold;     /* min IoU for collision (0.05) */
    float collision_speed_drop;        /* min speed drop ratio (0.5) */
    float near_miss_distance;          /* max distance in px (50) */
    float stopped_speed_threshold;     /* max speed to be "stopped" (2.0 px/s) */
    int   stopped_min_frames;          /* min frames stationary (30) */
    float sudden_brake_decel;          /* min decel for sudden brake (200 px/s^2) */
    float event_confidence_threshold;  /* min confidence to emit event (0.3) */
    int   temporal_window;             /* frames to look back for context (15) */
    /* scoring weights */
    float w_overlap;
    float w_speed_change;
    float w_direction_change;
    float w_proximity;
} EventDetectorConfig;

#endif /* VP_TYPES_H */
