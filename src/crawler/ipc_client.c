#include "crawler/ipc_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

/* Write exactly n bytes, handling EINTR and partial writes. */
static ssize_t write_all(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = write(fd, (const char *)buf + sent, n - sent);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1; /* should not happen on socket */
        sent += (size_t)r;
    }
    return (ssize_t)sent;
}

int ipc_client_connect(ipc_client_t *c, const char *socket_path)
{
    c->fd = -1;
    pthread_mutex_init(&c->send_mutex, NULL);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("ipc_client: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("ipc_client: connect");
        close(fd);
        return -1;
    }

    c->fd = fd;
    return 0;
}

int ipc_client_send(ipc_client_t *c, uint32_t docid, uint16_t depth,
                    const char *url, const char *filepath)
{
    if (c->fd < 0) return -1;

    ipc_msg_header_t hdr;
    hdr.docid    = docid;
    hdr.depth    = depth;
    hdr.url_len  = (uint16_t)strlen(url);
    hdr.path_len = (uint16_t)strlen(filepath);

    pthread_mutex_lock(&c->send_mutex);
    int ok = 0;
    if (write_all(c->fd, &hdr, sizeof(hdr))          < 0) ok = -1;
    if (ok == 0 && write_all(c->fd, url,      hdr.url_len)  < 0) ok = -1;
    if (ok == 0 && write_all(c->fd, filepath, hdr.path_len) < 0) ok = -1;
    pthread_mutex_unlock(&c->send_mutex);

    if (ok < 0)
        fprintf(stderr, "[IPC CLIENT] write error: %s\n", strerror(errno));
    return ok;
}

int ipc_client_send_sentinel(ipc_client_t *c)
{
    if (c->fd < 0) return -1;

    ipc_msg_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.docid = IPC_SENTINEL_DOCID;

    pthread_mutex_lock(&c->send_mutex);
    int r = (write_all(c->fd, &hdr, sizeof(hdr)) < 0) ? -1 : 0;
    pthread_mutex_unlock(&c->send_mutex);

    if (r < 0)
        fprintf(stderr, "[IPC CLIENT] sentinel write error: %s\n", strerror(errno));
    return r;
}

void ipc_client_close(ipc_client_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_destroy(&c->send_mutex);
}
