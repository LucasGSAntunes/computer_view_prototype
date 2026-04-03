#ifndef VP_TIMER_H
#define VP_TIMER_H

#include <time.h>

/* High-resolution timer for stage profiling */

typedef struct {
    struct timespec start;
} VPTimer;

static inline void vp_timer_start(VPTimer *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->start);
}

/* Returns elapsed milliseconds since vp_timer_start */
static inline double vp_timer_elapsed_ms(const VPTimer *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double sec  = (double)(now.tv_sec  - t->start.tv_sec);
    double nsec = (double)(now.tv_nsec - t->start.tv_nsec);
    return sec * 1000.0 + nsec / 1e6;
}

/* Returns current wall-clock ms (for timestamps) */
static inline double vp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#endif /* VP_TIMER_H */
