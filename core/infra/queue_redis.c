#include "queue_redis.h"
#include "core/common/logging.h"

#ifdef VP_HAS_REDIS
#include <hiredis/hiredis.h>
#endif

#include <stdlib.h>
#include <string.h>

struct JobQueue {
#ifdef VP_HAS_REDIS
    redisContext *ctx;
#endif
    bool connected;
};

#ifdef VP_HAS_REDIS

JobQueue *queue_connect(const char *host, int port) {
    JobQueue *q = calloc(1, sizeof(JobQueue));
    if (!q) return NULL;

    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    q->ctx = redisConnectWithTimeout(host, port, timeout);
    if (!q->ctx || q->ctx->err) {
        LOG_ERROR(NULL, "queue", "Redis connect failed: %s",
                  q->ctx ? q->ctx->errstr : "allocation error");
        if (q->ctx) redisFree(q->ctx);
        free(q);
        return NULL;
    }

    q->connected = true;
    LOG_INFO(NULL, "queue", "Connected to Redis %s:%d", host, port);
    return q;
}

VPError queue_push(JobQueue *q, const char *queue_name, const char *job_id, int priority) {
    if (!q || !q->connected) return VP_ERR_QUEUE_FAILED;

    /* Use sorted set with priority as score for priority queuing */
    redisReply *reply = redisCommand(q->ctx, "ZADD %s %d %s",
                                      queue_name, priority, job_id);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        if (reply) freeReplyObject(reply);
        return VP_ERR_QUEUE_FAILED;
    }
    freeReplyObject(reply);
    LOG_DEBUG(NULL, "queue", "Pushed %s to %s (priority=%d)", job_id, queue_name, priority);
    return VP_OK;
}

VPError queue_pop(JobQueue *q, const char *queue_name, char *out_buf,
                   size_t buf_size, int timeout_sec) {
    if (!q || !q->connected) return VP_ERR_QUEUE_FAILED;

    /* BZPOPMIN for priority-aware blocking pop */
    redisReply *reply = redisCommand(q->ctx, "BZPOPMIN %s %d",
                                      queue_name, timeout_sec);
    if (!reply) return VP_ERR_QUEUE_FAILED;

    if (reply->type == REDIS_REPLY_NIL || reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return VP_ERR_TIMEOUT;
    }

    /* BZPOPMIN returns [queue_name, member, score] */
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
        strncpy(out_buf, reply->element[1]->str, buf_size - 1);
        out_buf[buf_size - 1] = '\0';
        freeReplyObject(reply);
        return VP_OK;
    }

    freeReplyObject(reply);
    return VP_ERR_TIMEOUT;
}

int queue_depth(JobQueue *q, const char *queue_name) {
    if (!q || !q->connected) return -1;
    redisReply *reply = redisCommand(q->ctx, "ZCARD %s", queue_name);
    if (!reply) return -1;
    int count = (int)reply->integer;
    freeReplyObject(reply);
    return count;
}

VPError queue_push_dead_letter(JobQueue *q, const char *job_id, const char *reason) {
    if (!q || !q->connected) return VP_ERR_QUEUE_FAILED;

    redisReply *reply = redisCommand(q->ctx, "HSET vp:dead_letter %s %s",
                                      job_id, reason);
    if (reply) freeReplyObject(reply);
    LOG_WARN(NULL, "queue", "Dead letter: %s — %s", job_id, reason);
    return VP_OK;
}

void queue_close(JobQueue *q) {
    if (!q) return;
    if (q->ctx) redisFree(q->ctx);
    free(q);
}

#else /* no Redis */

JobQueue *queue_connect(const char *h, int p) {
    (void)h; (void)p;
    LOG_WARN(NULL, "queue", "Redis support not compiled");
    return NULL;
}
VPError queue_push(JobQueue *q, const char *n, const char *id, int p) { (void)q; (void)n; (void)id; (void)p; return VP_ERR_QUEUE_FAILED; }
VPError queue_pop(JobQueue *q, const char *n, char *b, size_t s, int t) { (void)q; (void)n; (void)b; (void)s; (void)t; return VP_ERR_QUEUE_FAILED; }
int queue_depth(JobQueue *q, const char *n) { (void)q; (void)n; return -1; }
VPError queue_push_dead_letter(JobQueue *q, const char *id, const char *r) { (void)q; (void)id; (void)r; return VP_ERR_QUEUE_FAILED; }
void queue_close(JobQueue *q) { free(q); }

#endif
