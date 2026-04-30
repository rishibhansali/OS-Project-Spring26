#ifndef IPC_SERVER_H
#define IPC_SERVER_H

/*
 * ipc_server.h — UNIX domain socket server (indexer side).
 *
 * Lifecycle:
 *   1. ipc_server_init()   — create socket, bind, listen
 *   2. ipc_server_accept() — block until crawler connects
 *   3. ipc_server_recv()   — call in a loop until it returns 0 (sentinel)
 *   4. ipc_server_close()  — close fds, unlink socket file
 */

#include <stddef.h>
#include <stdint.h>
#include "common/ipc_proto.h"

typedef struct {
    int  server_fd;                          /* listening socket        */
    int  client_fd;                          /* accepted crawler socket */
    char socket_path[IPC_SOCKET_MAX_PATH+1]; /* path for unlink()       */
} ipc_server_t;

/*
 * Create, bind, and listen on socket_path.
 * Unlinks any stale socket file at that path first.
 * Returns 0 on success, -1 on error.
 */
int  ipc_server_init(ipc_server_t *s, const char *socket_path);

/*
 * Block until the crawler connects.
 * Returns 0 on success, -1 on error.
 */
int  ipc_server_accept(ipc_server_t *s);

/*
 * Receive one message from the crawler.
 *   url_buf  / url_buf_sz  — buffer to receive null-terminated URL
 *   path_buf / path_buf_sz — buffer to receive null-terminated filepath
 *
 * Returns:
 *   1  — normal message received; *docid, *depth, url_buf, path_buf filled.
 *   0  — sentinel received (IPC_SENTINEL_DOCID); crawler is done.
 *  -1  — read error or connection closed unexpectedly.
 */
int  ipc_server_recv(ipc_server_t *s,
                     uint32_t *docid, uint16_t *depth,
                     char *url_buf,  size_t url_buf_sz,
                     char *path_buf, size_t path_buf_sz);

/* Close fds and unlink socket file. */
void ipc_server_close(ipc_server_t *s);

#endif /* IPC_SERVER_H */
