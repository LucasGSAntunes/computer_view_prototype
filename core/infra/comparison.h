#ifndef VP_COMPARISON_H
#define VP_COMPARISON_H

#include "audit.h"

/* ── Comparison type ── */
typedef enum {
    CMP_PROFILES = 0,      /* Compare operational profiles */
    CMP_ENGINES,           /* Compare inference engines */
    CMP_MODELS,            /* Compare model versions */
    CMP_ENVIRONMENTS       /* Compare runtime environments */
} ComparisonType;

/* ── Weight distribution for composite score ── */
typedef struct {
    float w_latency;
    float w_throughput;
    float w_memory;
    float w_stability;
    float w_robustness;
    float w_consistency;
} ScoreWeights;

/* ── Scored audit run ── */
typedef struct {
    const AuditRun *run;
    double score_latency;
    double score_throughput;
    double score_memory;
    double score_stability;
    double score_robustness;
    double score_consistency;
    double composite_score;
    double delta_vs_baseline; /* % delta against baseline */
} ScoredRun;

/* ── Comparison result ── */
typedef struct {
    ComparisonType type;
    ScoredRun     *scored_runs;
    int            count;
    int            baseline_idx;
    bool           fair;           /* true if experimental fairness validated */
    char           unfair_reason[256];
} ComparisonResult;

/* Predefined weight profiles */
ScoreWeights weights_low_latency(void);
ScoreWeights weights_high_throughput(void);
ScoreWeights weights_cost_efficient(void);
ScoreWeights weights_balanced(void);

/* Validate experimental fairness before comparing.
 * Returns true if comparison is fair. */
bool comparison_validate_fairness(const AuditRun **runs, int count,
                                   ComparisonType type, char *reason, size_t reason_len);

/* Compare multiple audit runs.
 * baseline_idx = index of the baseline run (-1 for auto = first). */
VPError comparison_execute(const AuditRun **runs, int count,
                            ComparisonType type, const ScoreWeights *weights,
                            int baseline_idx, ComparisonResult *out);

/* Export comparison result to JSON. */
VPError comparison_export_json(const ComparisonResult *result, const char *path);

/* Free comparison result. */
void comparison_result_free(ComparisonResult *result);

#endif /* VP_COMPARISON_H */
