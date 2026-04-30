#ifndef QUEUE_H
#define QUEUE_H

/*
 * queue.h — bounded, thread-safe URL frontier (work queue).
 *
 * A circular ring buffer protected by one mutex and two condition variables.
 *   - Producers block (backpressure) when the queue is full.
 *   - Consumers block when the queue is empty.
 *   - url_queue_shutdown() wakes all blocked threads; they return -1.
 */

#include <pthread.h>
#include <stddef.h>

#define QUEUE_DEFAULT_CAPACITY 1024

/* One entry in the queue. */
typedef struct {
    char *url;   /* heap-allocated; owned by queue until popped */
    int   depth; /* crawl depth of this URL */
} queue_entry_t;

/*
 * Bounded, thread-safe FIFO queue.
 * All fields are private; use only the API functions.
 */
typedef struct {
    queue_entry_t  *buf;       /* ring buffer [0 .. capacity-1] */
    int             head;      /* index of next slot to pop      */
    int             tail;      /* index of next slot to push     */
    int             size;      /* current number of items        */
    int             capacity;  /* maximum number of items        */
    int             max_seen;  /* high-water mark (for stats)    */
    volatile int    shutdown;  /* 1 after url_queue_shutdown()   */
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;  /* signaled when size drops below capacity */
    pthread_cond_t  not_empty; /* signaled when size rises above 0        */
} url_queue_t;

/*
 * Initialize the queue with the given capacity.
 * Returns 0 on success, -1 on allocation failure.
 */
int  url_queue_init(url_queue_t *q, int capacity);

/*
 * Destroy the queue and free all internal resources.
 * Any URLs still in the queue are freed.
 * Does NOT free the url_queue_t struct itself.
 */
void url_queue_destroy(url_queue_t *q);

/*
 * Push a URL at the given depth onto the queue.
 * Blocks if the queue is full (backpressure).
 *
 * Returns  0 on success (queue owns a strdup'd copy of url).
 * Returns -1 if the queue is in shutdown mode (caller must free url).
 */
int  url_queue_push(url_queue_t *q, const char *url, int depth);

/*
 * Pop a URL and its depth from the queue.
 * Blocks if the queue is empty.
 *
 * Returns  0 on success; *url_out is heap-allocated (caller must free).
 * Returns -1 if the queue is shut down AND empty (no more items coming).
 */
int  url_queue_pop(url_queue_t *q, char **url_out, int *depth_out);

/*
 * Signal shutdown: wake all blocked push/pop callers.
 * After this, push() always returns -1 and pop() returns -1 once empty.
 * Safe to call multiple times.
 */
void url_queue_shutdown(url_queue_t *q);

/* Return current queue size (approximate if called without lock held). */
int  url_queue_size(url_queue_t *q);

/* Return the highest queue size ever observed (high-water mark). */
int  url_queue_max_depth(url_queue_t *q);

#endif /* QUEUE_H */
