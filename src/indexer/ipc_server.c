#include "indexer/ipc_server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Read exactly n bytes, handling EINTR and partial reads. */
static ssize_t read_all(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0; /* EOF / connection closed */
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int ipc_server_init(ipc_server_t *s, const char *socket_path)
{
    s->server_fd = -1;
    s->client_fd = -1;
    strncpy(s->socket_path, socket_path, IPC_SOCKET_MAX_PATH);
    s->socket_path[IPC_SOCKET_MAX_PATH] = '\0';

    /* Remove stale socket from a previous run. */
    unlink(socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("ipc_server: socket"); return -1; }

    /* SO_REUSEADDR has no effect on UNIX sockets but is harmless. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("ipc_server: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("ipc_server: listen");
        close(fd);
        return -1;
    }

    s->server_fd = fd;
    return 0;
}

int ipc_server_accept(ipc_server_t *s)
{
    s->client_fd = accept(s->server_fd, NULL, NULL);
    if (s->client_fd < 0) {
        perror("ipc_server: accept");
        return -1;
    }
    return 0;
}

int ipc_server_recv(ipc_server_t *s,
                    uint32_t *docid, uint16_t *depth,
                    char *url_buf,  size_t url_buf_sz,
                    char *path_buf, size_t path_buf_sz)
{
    ipc_msg_header_t hdr;
    ssize_t r = read_all(s->client_fd, &hdr, sizeof(hdr));
    if (r <= 0) return -1; /* error or connection closed */

    if (hdr.docid == IPC_SENTINEL_DOCID) return 0; /* sentinel */

    *docid = hdr.docid;
    *depth = hdr.depth;

    /* Read URL string. */
    size_t url_read = (hdr.url_len < url_buf_sz) ? hdr.url_len
                                                   : url_buf_sz - 1;
    if (url_read > 0 && read_all(s->client_fd, url_buf, url_read) <= 0)
        return -1;
    /* Discard overflow bytes. */
    if (hdr.url_len > url_read) {
        char tmp[64];
        size_t remaining = hdr.url_len - url_read;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
            if (read_all(s->client_fd, tmp, chunk) <= 0) return -1;
            remaining -= chunk;
        }
    }
    url_buf[url_read] = '\0';

    /* Read filepath string. */
    size_t path_read = (hdr.path_len < path_buf_sz) ? hdr.path_len
                                                      : path_buf_sz - 1;
    if (path_read > 0 && read_all(s->client_fd, path_buf, path_read) <= 0)
        return -1;
    if (hdr.path_len > path_read) {
        char tmp[64];
        size_t remaining = hdr.path_len - path_read;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
            if (read_all(s->client_fd, tmp, chunk) <= 0) return -1;
            remaining -= chunk;
        }
    }
    path_buf[path_read] = '\0';

    return 1; /* normal message */
}

void ipc_server_close(ipc_server_t *s)
{
    if (s->client_fd >= 0) { close(s->client_fd); s->client_fd = -1; }
    if (s->server_fd >= 0) { close(s->server_fd); s->server_fd = -1; }
    if (s->socket_path[0]) {
        unlink(s->socket_path);
        s->socket_path[0] = '\0';
    }
}
