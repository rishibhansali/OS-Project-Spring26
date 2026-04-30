#ifndef VISITED_H
#define VISITED_H

/*
 * visited.h — thread-safe hash set for tracking visited URLs.
 *
 * Uses open-addressing with FNV-1a hashing and linear probing.
 * A single mutex protects all operations, making check-and-insert atomic.
 * Automatically resizes when load factor exceeds VISITED_LOAD_THRESHOLD.
 */

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define VISITED_INIT_CAPACITY   65536U   /* must be a power of 2 */
#define VISITED_LOAD_THRESHOLD  0.70     /* resize at 70% load   */

/* Sentinel pointer used to mark deleted (tombstone) slots. */
#define VISITED_TOMBSTONE ((char *)1)

typedef struct {
    char     **keys;     /* NULL = empty, TOMBSTONE = deleted, else strdup'd URL */
    uint32_t  *hashes;   /* cached FNV-1a hashes (speeds up resize)             */
    size_t     capacity; /* current table size (always a power of 2)            */
    size_t     count;    /* number of live entries                              */
    pthread_mutex_t mutex;
} visited_set_t;

/*
 * Initialize with the given initial capacity (must be a power of 2).
 * Returns 0 on success, -1 on allocation failure.
 */
int  visited_init(visited_set_t *v, size_t initial_capacity);

/* Free all memory. Does NOT free the visited_set_t struct itself. */
void visited_destroy(visited_set_t *v);

/*
 * Atomically check whether url is in the set and insert it if absent.
 *
 * Returns 1 if the URL was newly inserted (caller should crawl it).
 * Returns 0 if the URL was already present (caller should skip it).
 */
int  visited_check_and_insert(visited_set_t *v, const char *url);

/* Return current entry count (approximate without lock). */
size_t visited_count(visited_set_t *v);

#endif /* VISITED_H */
