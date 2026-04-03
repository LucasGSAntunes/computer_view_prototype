#include "frame_sampler.h"
#include <stdlib.h>

struct FrameSampler {
    SamplingPolicy policy;
    int  interval;            /* base interval for INTERVAL/ADAPTIVE */
    int  current_interval;    /* effective interval (adaptive adjusts this) */
    int  processed;
    int  skipped;
    int  last_activity;       /* detection count from last processed frame */
};

FrameSampler *frame_sampler_create(SamplingPolicy policy, int interval) {
    FrameSampler *s = calloc(1, sizeof(FrameSampler));
    if (!s) return NULL;
    s->policy   = policy;
    s->interval = (interval > 0) ? interval : 1;
    s->current_interval = s->interval;
    return s;
}

bool frame_sampler_should_process(FrameSampler *sampler, int frame_number,
                                  double timestamp_ms) {
    (void)timestamp_ms;
    if (!sampler) return true;

    bool process = false;

    switch (sampler->policy) {
    case SAMPLING_ALL:
        process = true;
        break;

    case SAMPLING_INTERVAL:
        process = (frame_number % sampler->interval == 0);
        break;

    case SAMPLING_ADAPTIVE:
        process = (frame_number % sampler->current_interval == 0);
        break;
    }

    if (process)
        sampler->processed++;
    else
        sampler->skipped++;

    return process;
}

void frame_sampler_notify_activity(FrameSampler *sampler, int detection_count) {
    if (!sampler || sampler->policy != SAMPLING_ADAPTIVE) return;

    /* If activity increased, decrease interval (sample more).
     * If activity decreased, increase interval (sample less). */
    if (detection_count > sampler->last_activity + 2) {
        sampler->current_interval = (sampler->interval > 1) ? sampler->interval / 2 : 1;
    } else if (detection_count == 0 && sampler->last_activity == 0) {
        int doubled = sampler->interval * 2;
        sampler->current_interval = (doubled < 30) ? doubled : 30;
    } else {
        sampler->current_interval = sampler->interval;
    }

    if (sampler->current_interval < 1) sampler->current_interval = 1;
    sampler->last_activity = detection_count;
}

void frame_sampler_reset(FrameSampler *sampler) {
    if (!sampler) return;
    sampler->processed = 0;
    sampler->skipped   = 0;
    sampler->current_interval = sampler->interval;
    sampler->last_activity = 0;
}

int frame_sampler_processed_count(const FrameSampler *sampler) {
    return sampler ? sampler->processed : 0;
}

int frame_sampler_skipped_count(const FrameSampler *sampler) {
    return sampler ? sampler->skipped : 0;
}

void frame_sampler_destroy(FrameSampler *sampler) {
    free(sampler);
}
