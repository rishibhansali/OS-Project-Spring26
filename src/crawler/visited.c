#include "crawler/visited.h"

#include <stdlib.h>
#include <string.h>

/* FNV-1a 32-bit hash. */
static uint32_t fnv1a_32(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ (uint32_t)*p) * 16777619u;
    return h;
}

int visited_init(visited_set_t *v, size_t initial_capacity)
{
    v->keys   = calloc(initial_capacity, sizeof(char *));
    v->hashes = calloc(initial_capacity, sizeof(uint32_t));
    if (!v->keys || !v->hashes) {
        free(v->keys);
        free(v->hashes);
        return -1;
    }
    v->capacity = initial_capacity;
    v->count    = 0;
    pthread_mutex_init(&v->mutex, NULL);
    return 0;
}

void visited_destroy(visited_set_t *v)
{
    pthread_mutex_lock(&v->mutex);
    for (size_t i = 0; i < v->capacity; i++) {
        if (v->keys[i] && v->keys[i] != VISITED_TOMBSTONE)
            free(v->keys[i]);
    }
    free(v->keys);
    free(v->hashes);
    v->keys   = NULL;
    v->hashes = NULL;
    pthread_mutex_unlock(&v->mutex);
    pthread_mutex_destroy(&v->mutex);
}

/*
 * _resize — rehash into a table of new_cap slots.
 * Called with mutex held.
 */
static int _resize(visited_set_t *v, size_t new_cap)
{
    char     **new_keys   = calloc(new_cap, sizeof(char *));
    uint32_t  *new_hashes = calloc(new_cap, sizeof(uint32_t));
    if (!new_keys || !new_hashes) {
        free(new_keys);
        free(new_hashes);
        return -1;
    }

    for (size_t i = 0; i < v->capacity; i++) {
        char *k = v->keys[i];
        if (!k || k == VISITED_TOMBSTONE) continue;

        uint32_t h    = v->hashes[i];
        size_t   slot = h & (new_cap - 1);
        while (new_keys[slot])
            slot = (slot + 1) & (new_cap - 1);
        new_keys[slot]   = k;
        new_hashes[slot] = h;
    }

    free(v->keys);
    free(v->hashes);
    v->keys     = new_keys;
    v->hashes   = new_hashes;
    v->capacity = new_cap;
    return 0;
}

int visited_check_and_insert(visited_set_t *v, const char *url)
{
    pthread_mutex_lock(&v->mutex);

    uint32_t h    = fnv1a_32(url);
    size_t   mask = v->capacity - 1;
    size_t   slot = h & mask;
    size_t   first_tomb = (size_t)-1; /* first tombstone slot seen */

    /* Linear probe: search for existing key or empty slot. */
    for (size_t i = 0; i < v->capacity; i++) {
        size_t idx = (slot + i) & mask;
        char  *k   = v->keys[idx];

        if (!k) {
            /* Empty slot — URL is not present. */
            size_t ins = (first_tomb != (size_t)-1) ? first_tomb : idx;
            char *copy = strdup(url);
            if (!copy) {
                pthread_mutex_unlock(&v->mutex);
                return -1; /* treat as "already visited" on OOM */
            }
            v->keys[ins]   = copy;
            v->hashes[ins] = h;
            v->count++;

            /* Resize if load factor exceeds threshold. */
            if ((double)v->count / (double)v->capacity > VISITED_LOAD_THRESHOLD)
                _resize(v, v->capacity * 2);

            pthread_mutex_unlock(&v->mutex);
            return 1; /* newly inserted */
        }

        if (k == VISITED_TOMBSTONE) {
            if (first_tomb == (size_t)-1) first_tomb = idx;
            continue;
        }

        if (v->hashes[idx] == h && strcmp(k, url) == 0) {
            /* Already visited. */
            pthread_mutex_unlock(&v->mutex);
            return 0;
        }
    }

    /* Table full after probing (shouldn't happen with resize). */
    pthread_mutex_unlock(&v->mutex);
    return 0;
}

size_t visited_count(visited_set_t *v)
{
    pthread_mutex_lock(&v->mutex);
    size_t c = v->count;
    pthread_mutex_unlock(&v->mutex);
    return c;
}
