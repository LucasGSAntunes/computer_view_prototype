#ifndef VP_LOGGING_H
#define VP_LOGGING_H

#include <stdio.h>
#include <time.h>
#include <string.h>

typedef enum {
    VP_LOG_DEBUG = 0,
    VP_LOG_INFO,
    VP_LOG_WARN,
    VP_LOG_ERROR
} VPLogLevel;

/* Global log level — set via vp_log_set_level() */
extern VPLogLevel g_vp_log_level;

void vp_log_set_level(VPLogLevel level);

static inline const char *vp_log_level_str(VPLogLevel lvl) {
    switch (lvl) {
        case VP_LOG_DEBUG: return "DEBUG";
        case VP_LOG_INFO:  return "INFO";
        case VP_LOG_WARN:  return "WARN";
        case VP_LOG_ERROR: return "ERROR";
    }
    return "?";
}

#define VP_LOG(level, job_id, stage, fmt, ...) \
    do { \
        if ((level) >= g_vp_log_level) { \
            struct timespec _ts; \
            clock_gettime(CLOCK_REALTIME, &_ts); \
            struct tm _tm; \
            gmtime_r(&_ts.tv_sec, &_tm); \
            char _tbuf[32]; \
            strftime(_tbuf, sizeof(_tbuf), "%Y-%m-%dT%H:%M:%S", &_tm); \
            fprintf(stderr, "{\"ts\":\"%s.%03ldZ\",\"level\":\"%s\"," \
                    "\"job\":\"%s\",\"stage\":\"%s\",\"msg\":\"" fmt "\"}\n", \
                    _tbuf, _ts.tv_nsec / 1000000, \
                    vp_log_level_str(level), \
                    (job_id) ? (job_id) : "-", \
                    (stage) ? (stage) : "-", \
                    ##__VA_ARGS__); \
        } \
    } while (0)

#define LOG_DEBUG(job, stage, fmt, ...) VP_LOG(VP_LOG_DEBUG, job, stage, fmt, ##__VA_ARGS__)
#define LOG_INFO(job, stage, fmt, ...)  VP_LOG(VP_LOG_INFO,  job, stage, fmt, ##__VA_ARGS__)
#define LOG_WARN(job, stage, fmt, ...)  VP_LOG(VP_LOG_WARN,  job, stage, fmt, ##__VA_ARGS__)
#define LOG_ERROR(job, stage, fmt, ...) VP_LOG(VP_LOG_ERROR, job, stage, fmt, ##__VA_ARGS__)

#endif /* VP_LOGGING_H */
