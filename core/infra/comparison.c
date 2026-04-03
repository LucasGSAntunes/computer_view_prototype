#include "comparison.h"
#include "core/common/logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

/* ── Predefined weights ── */

ScoreWeights weights_low_latency(void) {
    return (ScoreWeights){ .w_latency=0.35f, .w_throughput=0.15f, .w_memory=0.10f,
                           .w_stability=0.15f, .w_robustness=0.15f, .w_consistency=0.10f };
}

ScoreWeights weights_high_throughput(void) {
    return (ScoreWeights){ .w_latency=0.15f, .w_throughput=0.35f, .w_memory=0.10f,
                           .w_stability=0.10f, .w_robustness=0.15f, .w_consistency=0.15f };
}

ScoreWeights weights_cost_efficient(void) {
    return (ScoreWeights){ .w_latency=0.15f, .w_throughput=0.15f, .w_memory=0.25f,
                           .w_stability=0.15f, .w_robustness=0.15f, .w_consistency=0.15f };
}

ScoreWeights weights_balanced(void) {
    return (ScoreWeights){ .w_latency=0.18f, .w_throughput=0.18f, .w_memory=0.16f,
                           .w_stability=0.16f, .w_robustness=0.16f, .w_consistency=0.16f };
}

/* ── Normalization ── */

/* Lower is better (latency, memory) */
static double normalize_lower(double val, double min_val, double max_val) {
    if (max_val <= min_val) return 1.0;
    return (max_val - val) / (max_val - min_val);
}

/* Higher is better (throughput, robustness) */
static double normalize_higher(double val, double min_val, double max_val) {
    if (max_val <= min_val) return 1.0;
    return (val - min_val) / (max_val - min_val);
}

/* ── Fairness validation ── */

bool comparison_validate_fairness(const AuditRun **runs, int count,
                                   ComparisonType type, char *reason, size_t reason_len) {
    if (count < 2) {
        snprintf(reason, reason_len, "Need at least 2 runs to compare");
        return false;
    }

    const AuditRun *ref = runs[0];

    for (int i = 1; i < count; i++) {
        const AuditRun *r = runs[i];

        switch (type) {
        case CMP_PROFILES:
            /* Video, model, engine, hardware must be same */
            if (strcmp(ref->job_snapshot.source_uri, r->job_snapshot.source_uri) != 0) {
                snprintf(reason, reason_len, "Different videos: %s vs %s",
                         ref->job_snapshot.source_uri, r->job_snapshot.source_uri);
                return false;
            }
            if (strcmp(ref->job_snapshot.model_path, r->job_snapshot.model_path) != 0) {
                snprintf(reason, reason_len, "Different models");
                return false;
            }
            if (strcmp(ref->engine_snapshot.backend_type, r->engine_snapshot.backend_type) != 0) {
                snprintf(reason, reason_len, "Different engines");
                return false;
            }
            break;

        case CMP_ENGINES:
            /* Video, model, profile, hardware must be same */
            if (strcmp(ref->job_snapshot.source_uri, r->job_snapshot.source_uri) != 0) {
                snprintf(reason, reason_len, "Different videos");
                return false;
            }
            if (strcmp(ref->profile_snapshot.name, r->profile_snapshot.name) != 0) {
                snprintf(reason, reason_len, "Different profiles");
                return false;
            }
            break;

        case CMP_MODELS:
            if (strcmp(ref->job_snapshot.source_uri, r->job_snapshot.source_uri) != 0) {
                snprintf(reason, reason_len, "Different videos");
                return false;
            }
            if (strcmp(ref->engine_snapshot.backend_type, r->engine_snapshot.backend_type) != 0) {
                snprintf(reason, reason_len, "Different engines");
                return false;
            }
            break;

        case CMP_ENVIRONMENTS:
            if (strcmp(ref->job_snapshot.source_uri, r->job_snapshot.source_uri) != 0) {
                snprintf(reason, reason_len, "Different videos");
                return false;
            }
            if (strcmp(ref->job_snapshot.model_path, r->job_snapshot.model_path) != 0) {
                snprintf(reason, reason_len, "Different models");
                return false;
            }
            break;
        }
    }

    reason[0] = '\0';
    return true;
}

/* ── Comparison execution ── */

VPError comparison_execute(const AuditRun **runs, int count,
                            ComparisonType type, const ScoreWeights *weights,
                            int baseline_idx, ComparisonResult *out) {
    if (!runs || count < 2 || !weights || !out) return VP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(ComparisonResult));
    out->type = type;
    out->count = count;
    out->scored_runs = calloc(count, sizeof(ScoredRun));
    if (!out->scored_runs) return VP_ERR_OUT_OF_MEMORY;

    /* Validate fairness */
    char reason[256] = {0};
    out->fair = comparison_validate_fairness(runs, count, type, reason, sizeof(reason));
    if (!out->fair) {
        strncpy(out->unfair_reason, reason, sizeof(out->unfair_reason) - 1);
        LOG_WARN(NULL, "comparison", "Unfair comparison: %s", reason);
    }

    /* Collect raw metrics */
    double *latencies   = malloc(count * sizeof(double));
    double *throughputs  = malloc(count * sizeof(double));
    double *memories     = malloc(count * sizeof(double));
    double *stabilities  = malloc(count * sizeof(double));
    double *robustness   = malloc(count * sizeof(double));
    double *consistency  = malloc(count * sizeof(double));

    for (int i = 0; i < count; i++) {
        const AuditRun *r = runs[i];
        out->scored_runs[i].run = r;

        latencies[i]  = metrics_avg(&r->metrics.infer_ms);
        throughputs[i] = metrics_effective_fps(&r->metrics);
        memories[i]    = (double)r->metrics.peak_memory_bytes / (1024.0 * 1024.0);
        stabilities[i] = metrics_stddev(&r->metrics.frame_total_ms);
        robustness[i]  = (r->metrics.frames_processed > 0)
            ? 1.0 - ((double)r->metrics.errors / r->metrics.frames_processed)
            : 0.0;
        consistency[i] = (double)r->catalog.count; /* proxy: same class count = consistent */
    }

    /* Find min/max for normalization */
    double lat_min = DBL_MAX, lat_max = -DBL_MAX;
    double thr_min = DBL_MAX, thr_max = -DBL_MAX;
    double mem_min = DBL_MAX, mem_max = -DBL_MAX;
    double sta_min = DBL_MAX, sta_max = -DBL_MAX;
    double rob_min = DBL_MAX, rob_max = -DBL_MAX;
    double con_min = DBL_MAX, con_max = -DBL_MAX;

    for (int i = 0; i < count; i++) {
        if (latencies[i] < lat_min) lat_min = latencies[i];
        if (latencies[i] > lat_max) lat_max = latencies[i];
        if (throughputs[i] < thr_min) thr_min = throughputs[i];
        if (throughputs[i] > thr_max) thr_max = throughputs[i];
        if (memories[i] < mem_min) mem_min = memories[i];
        if (memories[i] > mem_max) mem_max = memories[i];
        if (stabilities[i] < sta_min) sta_min = stabilities[i];
        if (stabilities[i] > sta_max) sta_max = stabilities[i];
        if (robustness[i] < rob_min) rob_min = robustness[i];
        if (robustness[i] > rob_max) rob_max = robustness[i];
        if (consistency[i] < con_min) con_min = consistency[i];
        if (consistency[i] > con_max) con_max = consistency[i];
    }

    /* Normalize and compute scores */
    for (int i = 0; i < count; i++) {
        ScoredRun *s = &out->scored_runs[i];
        s->score_latency     = normalize_lower(latencies[i], lat_min, lat_max);
        s->score_throughput  = normalize_higher(throughputs[i], thr_min, thr_max);
        s->score_memory      = normalize_lower(memories[i], mem_min, mem_max);
        s->score_stability   = normalize_lower(stabilities[i], sta_min, sta_max);
        s->score_robustness  = normalize_higher(robustness[i], rob_min, rob_max);
        s->score_consistency = normalize_higher(consistency[i], con_min, con_max);

        s->composite_score =
            weights->w_latency     * s->score_latency +
            weights->w_throughput  * s->score_throughput +
            weights->w_memory      * s->score_memory +
            weights->w_stability   * s->score_stability +
            weights->w_robustness  * s->score_robustness +
            weights->w_consistency * s->score_consistency;
    }

    /* Sort by composite score descending */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (out->scored_runs[j].composite_score > out->scored_runs[i].composite_score) {
                ScoredRun tmp = out->scored_runs[i];
                out->scored_runs[i] = out->scored_runs[j];
                out->scored_runs[j] = tmp;
            }
        }
    }

    /* Baseline and deltas */
    out->baseline_idx = (baseline_idx >= 0 && baseline_idx < count)
        ? baseline_idx : 0;

    double baseline_score = out->scored_runs[out->baseline_idx].composite_score;
    for (int i = 0; i < count; i++) {
        if (baseline_score > 0) {
            out->scored_runs[i].delta_vs_baseline =
                ((out->scored_runs[i].composite_score - baseline_score) / baseline_score) * 100.0;
        }
    }

    free(latencies); free(throughputs); free(memories);
    free(stabilities); free(robustness); free(consistency);

    return VP_OK;
}

VPError comparison_export_json(const ComparisonResult *result, const char *path) {
    if (!result || !path) return VP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "w");
    if (!f) return VP_ERR_EXPORT_FAILED;

    const char *type_names[] = {"profiles", "engines", "models", "environments"};

    fprintf(f, "{\n"
               "  \"comparison_type\": \"%s\",\n"
               "  \"fair\": %s,\n",
            type_names[result->type],
            result->fair ? "true" : "false");

    if (!result->fair) {
        fprintf(f, "  \"unfair_reason\": \"%s\",\n", result->unfair_reason);
    }

    fprintf(f, "  \"rankings\": [\n");
    for (int i = 0; i < result->count; i++) {
        const ScoredRun *s = &result->scored_runs[i];
        fprintf(f,
            "    {\n"
            "      \"rank\": %d,\n"
            "      \"audit_run_id\": \"%s\",\n"
            "      \"job_id\": \"%s\",\n"
            "      \"profile\": \"%s\",\n"
            "      \"engine\": \"%s\",\n"
            "      \"composite_score\": %.4f,\n"
            "      \"delta_vs_baseline\": %.2f,\n"
            "      \"scores\": {\n"
            "        \"latency\": %.4f,\n"
            "        \"throughput\": %.4f,\n"
            "        \"memory\": %.4f,\n"
            "        \"stability\": %.4f,\n"
            "        \"robustness\": %.4f,\n"
            "        \"consistency\": %.4f\n"
            "      }\n"
            "    }%s\n",
            i + 1,
            s->run->audit_run_id,
            s->run->job_id,
            s->run->profile_snapshot.name,
            s->run->engine_snapshot.engine_name,
            s->composite_score,
            s->delta_vs_baseline,
            s->score_latency, s->score_throughput, s->score_memory,
            s->score_stability, s->score_robustness, s->score_consistency,
            (i < result->count - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");

    fclose(f);
    LOG_INFO(NULL, "comparison", "Comparison report: %s", path);
    return VP_OK;
}

void comparison_result_free(ComparisonResult *result) {
    if (result) {
        free(result->scored_runs);
        memset(result, 0, sizeof(ComparisonResult));
    }
}
