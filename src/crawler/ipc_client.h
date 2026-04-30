#ifndef IPC_CLIENT_H
#define IPC_CLIENT_H

/*
 * ipc_client.h — UNIX domain socket client (crawler side).
 *
 * The crawler connects to the indexer's listening socket and sends
 * one message per fetched page, then a sentinel at the end.
 *
 * Multiple worker threads share one ipc_client_t; an internal mutex
 * serializes all writes so messages are never interleaved.
 */

#include <stdint.h>
#include <pthread.h>
#include "common/ipc_proto.h"

typedef struct {
    int             fd;          /* connected socket fd, -1 if closed  */
    pthread_mutex_t send_mutex;  /* serializes concurrent worker sends */
} ipc_client_t;

/*
 * Connect to the indexer at socket_path.
 * Returns 0 on success, -1 on error (errno set).
 */
int  ipc_client_connect(ipc_client_t *c, const char *socket_path);

/*
 * Send one document metadata message.
 * Thread-safe (serialized by internal mutex).
 * Returns 0 on success, -1 on write error.
 */
int  ipc_client_send(ipc_client_t *c, uint32_t docid, uint16_t depth,
                     const char *url, const char *filepath);

/*
 * Send the sentinel message (docid = IPC_SENTINEL_DOCID, no body).
 * Call once after all worker threads have finished.
 * Returns 0 on success, -1 on error.
 */
int  ipc_client_send_sentinel(ipc_client_t *c);

/* Close the socket and release resources. */
void ipc_client_close(ipc_client_t *c);

#endif /* IPC_CLIENT_H */
