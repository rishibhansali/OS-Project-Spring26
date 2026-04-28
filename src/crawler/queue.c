#include "crawler/queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int url_queue_init(url_queue_t *q, int capacity)
{
    if (capacity <= 0) capacity = QUEUE_DEFAULT_CAPACITY;

    q->buf = malloc((size_t)capacity * sizeof(queue_entry_t));
    if (!q->buf) return -1;

    q->head     = 0;
    q->tail     = 0;
    q->size     = 0;
    q->capacity = capacity;
    q->max_seen = 0;
    q->shutdown = 0;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_full,  NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

void url_queue_destroy(url_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);

    /* Free any URLs still sitting in the ring buffer. */
    for (int i = 0; i < q->size; i++) {
        int idx = (q->head + i) % q->capacity;
        free(q->buf[idx].url);
    }
    free(q->buf);
    q->buf  = NULL;
    q->size = 0;

    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
}

int url_queue_push(url_queue_t *q, const char *url, int depth)
{
    pthread_mutex_lock(&q->mutex);

    /* Drop the URL if the queue is full or shutting down — never block.
     * All threads are both producers and consumers; blocking here when
     * the queue is full causes a deadlock if every thread is mid-push. */
    if (q->shutdown || q->size == q->capacity) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    char *copy = strdup(url);
    if (!copy) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->buf[q->tail].url   = copy;
    q->buf[q->tail].depth = depth;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;

    if (q->size > q->max_seen)
        q->max_seen = q->size;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int url_queue_pop(url_queue_t *q, char **url_out, int *depth_out)
{
    pthread_mutex_lock(&q->mutex);

    /* Block while empty, unless shutdown has been requested. */
    while (q->size == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->size == 0) {
        /* shutdown + empty => no more items will ever arrive */
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    *url_out   = q->buf[q->head].url;
    *depth_out = q->buf[q->head].depth;
    q->head    = (q->head + 1) % q->capacity;
    q->size--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

void url_queue_shutdown(url_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

int url_queue_size(url_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    int s = q->size;
    pthread_mutex_unlock(&q->mutex);
    return s;
}

int url_queue_max_depth(url_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    int m = q->max_seen;
    pthread_mutex_unlock(&q->mutex);
    return m;
}
