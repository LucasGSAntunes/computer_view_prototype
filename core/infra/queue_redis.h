#ifndef VP_QUEUE_REDIS_H
#define VP_QUEUE_REDIS_H

#include "core/common/types.h"
#include "core/common/errors.h"

typedef struct JobQueue JobQueue;

/* Connect to Redis. */
JobQueue *queue_connect(const char *host, int port);

/* Push a job ID to a named queue. */
VPError queue_push(JobQueue *q, const char *queue_name, const char *job_id, int priority);

/* Blocking pop from queue (timeout_sec=0 means infinite wait).
 * Writes job_id to out_buf. Returns VP_OK or VP_ERR_TIMEOUT. */
VPError queue_pop(JobQueue *q, const char *queue_name, char *out_buf,
                   size_t buf_size, int timeout_sec);

/* Get queue depth. */
int queue_depth(JobQueue *q, const char *queue_name);

/* Push to dead-letter queue. */
VPError queue_push_dead_letter(JobQueue *q, const char *job_id, const char *reason);

void queue_close(JobQueue *q);

#endif /* VP_QUEUE_REDIS_H */
