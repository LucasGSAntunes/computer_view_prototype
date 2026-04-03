-- Vision Platform — Database Schema
-- Run: psql -d vision_platform -f setup_db.sql

CREATE DATABASE vision_platform;
\c vision_platform;

-- ══════════════════════════════════════
-- Core tables
-- ══════════════════════════════════════

CREATE TABLE IF NOT EXISTS processing_profile (
    profile_id   SERIAL PRIMARY KEY,
    name         VARCHAR(64) UNIQUE NOT NULL,
    sampling_policy    INT DEFAULT 0,
    input_resolution   VARCHAR(16),
    confidence_threshold REAL DEFAULT 0.25,
    iou_threshold      REAL DEFAULT 0.45,
    batch_size         INT DEFAULT 1,
    tracking_enabled   BOOLEAN DEFAULT TRUE,
    track_max_age      INT DEFAULT 30,
    max_retries        INT DEFAULT 3,
    timeout_ms         INT DEFAULT 60000,
    max_memory_mb      INT DEFAULT 1024,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS inference_engine (
    engine_id    SERIAL PRIMARY KEY,
    name         VARCHAR(64),
    version      VARCHAR(32),
    backend_type VARCHAR(32),
    device_type  VARCHAR(32),
    precision_mode VARCHAR(16),
    UNIQUE(name, version, backend_type)
);

CREATE TABLE IF NOT EXISTS runtime_environment (
    runtime_env_id SERIAL PRIMARY KEY,
    cpu_model      VARCHAR(128),
    gpu_model      VARCHAR(128),
    memory_gb      INT,
    os_version     VARCHAR(64),
    driver_version VARCHAR(64),
    UNIQUE(cpu_model, gpu_model, os_version)
);

CREATE TABLE IF NOT EXISTS video_job (
    job_id       VARCHAR(64) PRIMARY KEY,
    source_uri   TEXT NOT NULL,
    status       INT DEFAULT 0,
    priority     INT DEFAULT 5,
    profile_name VARCHAR(64),
    engine_name  VARCHAR(64),
    model_path   TEXT,
    labels_path  TEXT,
    output_dir   TEXT,
    created_at   TIMESTAMPTZ DEFAULT NOW(),
    updated_at   TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS frame_detection (
    id           BIGSERIAL PRIMARY KEY,
    job_id       VARCHAR(64) REFERENCES video_job(job_id),
    frame_number INT,
    timestamp_ms DOUBLE PRECISION,
    class_id     INT,
    class_name   VARCHAR(64),
    confidence   REAL,
    bbox_x       REAL,
    bbox_y       REAL,
    bbox_w       REAL,
    bbox_h       REAL,
    track_id     INT
);

CREATE TABLE IF NOT EXISTS catalog_summary (
    id                  SERIAL PRIMARY KEY,
    job_id              VARCHAR(64) REFERENCES video_job(job_id),
    class_id            INT,
    class_name          VARCHAR(64),
    total_detections    INT,
    total_unique_tracks INT,
    first_seen_ms       DOUBLE PRECISION,
    last_seen_ms        DOUBLE PRECISION
);

CREATE TABLE IF NOT EXISTS processing_metrics (
    id                SERIAL PRIMARY KEY,
    job_id            VARCHAR(64) REFERENCES video_job(job_id),
    frames_processed  INT,
    total_detections  INT,
    total_duration_ms DOUBLE PRECISION,
    effective_fps     DOUBLE PRECISION,
    decode_avg_ms     DOUBLE PRECISION,
    preprocess_avg_ms DOUBLE PRECISION,
    infer_avg_ms      DOUBLE PRECISION,
    infer_p95_ms      DOUBLE PRECISION,
    infer_p99_ms      DOUBLE PRECISION,
    postprocess_avg_ms DOUBLE PRECISION,
    track_avg_ms      DOUBLE PRECISION,
    peak_memory_mb    DOUBLE PRECISION,
    errors            INT,
    recorded_at       TIMESTAMPTZ DEFAULT NOW()
);

-- ══════════════════════════════════════
-- Audit tables
-- ══════════════════════════════════════

CREATE TABLE IF NOT EXISTS audit_run (
    audit_run_id    VARCHAR(64) PRIMARY KEY,
    job_id          VARCHAR(64) REFERENCES video_job(job_id),
    profile_name    VARCHAR(64),
    engine_name     VARCHAR(64),
    model_path      TEXT,
    runtime_env_id  INT REFERENCES runtime_environment(runtime_env_id),
    config_hash     VARCHAR(65),
    started_at      DOUBLE PRECISION,
    finished_at     DOUBLE PRECISION,
    status          VARCHAR(16) DEFAULT 'running',
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS audit_parameter_snapshot (
    id           BIGSERIAL PRIMARY KEY,
    audit_run_id VARCHAR(64) REFERENCES audit_run(audit_run_id),
    param_key    VARCHAR(64),
    param_value  VARCHAR(256)
);

CREATE TABLE IF NOT EXISTS audit_stage_metrics (
    id           SERIAL PRIMARY KEY,
    audit_run_id VARCHAR(64) REFERENCES audit_run(audit_run_id),
    stage        VARCHAR(32),
    avg_ms       DOUBLE PRECISION,
    p95_ms       DOUBLE PRECISION,
    p99_ms       DOUBLE PRECISION,
    stddev_ms    DOUBLE PRECISION,
    peak_memory_mb DOUBLE PRECISION,
    error_count  INT
);

CREATE TABLE IF NOT EXISTS audit_comparison (
    comparison_id   SERIAL PRIMARY KEY,
    comparison_type VARCHAR(32),
    baseline_run_id VARCHAR(64) REFERENCES audit_run(audit_run_id),
    candidate_run_ids TEXT[],
    fair            BOOLEAN DEFAULT TRUE,
    unfair_reason   TEXT,
    result_json     JSONB,
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- ══════════════════════════════════════
-- Indexes
-- ══════════════════════════════════════

CREATE INDEX IF NOT EXISTS idx_detection_job     ON frame_detection(job_id);
CREATE INDEX IF NOT EXISTS idx_detection_frame   ON frame_detection(job_id, frame_number);
CREATE INDEX IF NOT EXISTS idx_catalog_job       ON catalog_summary(job_id);
CREATE INDEX IF NOT EXISTS idx_job_status        ON video_job(status);
CREATE INDEX IF NOT EXISTS idx_job_created       ON video_job(created_at);
CREATE INDEX IF NOT EXISTS idx_audit_job         ON audit_run(job_id);
CREATE INDEX IF NOT EXISTS idx_audit_hash        ON audit_run(config_hash);
CREATE INDEX IF NOT EXISTS idx_audit_params      ON audit_parameter_snapshot(audit_run_id);
CREATE INDEX IF NOT EXISTS idx_audit_stage       ON audit_stage_metrics(audit_run_id);

-- ══════════════════════════════════════
-- Seed default profiles
-- ══════════════════════════════════════

INSERT INTO processing_profile (name, sampling_policy, input_resolution, confidence_threshold, iou_threshold, batch_size, tracking_enabled, track_max_age)
VALUES
    ('Low Latency',     1, '416x416', 0.30, 0.50, 1, TRUE,  20),
    ('High Throughput', 0, '640x640', 0.25, 0.45, 4, TRUE,  30),
    ('Cost Efficient',  1, '320x320', 0.40, 0.50, 1, FALSE, 0),
    ('Balanced',        0, '640x640', 0.25, 0.45, 1, TRUE,  30)
ON CONFLICT (name) DO NOTHING;
