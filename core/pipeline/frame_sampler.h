#ifndef VP_FRAME_SAMPLER_H
#define VP_FRAME_SAMPLER_H

#include "core/common/types.h"
#include <stdbool.h>

typedef struct FrameSampler FrameSampler;

/* Create sampler with given policy.
 * For SAMPLING_INTERVAL, interval = every N frames.
 * For SAMPLING_ALL, interval is ignored.
 * For SAMPLING_ADAPTIVE, interval is the base interval. */
FrameSampler *frame_sampler_create(SamplingPolicy policy, int interval);

/* Returns true if this frame should be processed. */
bool frame_sampler_should_process(FrameSampler *sampler, int frame_number,
                                  double timestamp_ms);

/* Notify sampler of detection activity (for adaptive mode).
 * Higher activity may increase sampling rate. */
void frame_sampler_notify_activity(FrameSampler *sampler, int detection_count);

/* Reset sampler state. */
void frame_sampler_reset(FrameSampler *sampler);

/* Stats */
int frame_sampler_processed_count(const FrameSampler *sampler);
int frame_sampler_skipped_count(const FrameSampler *sampler);

void frame_sampler_destroy(FrameSampler *sampler);

#endif /* VP_FRAME_SAMPLER_H */
