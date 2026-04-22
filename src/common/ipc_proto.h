#ifndef IPC_PROTO_H
#define IPC_PROTO_H

/*
 * ipc_proto.h — shared IPC wire protocol between crawler and indexer.
 *
 * Transport: UNIX domain socket (SOCK_STREAM).
 * Both processes run on the same machine, so host byte order is used
 * throughout (no hton/ntoh conversions needed).
 */

#include <stdint.h>

/* Maximum UNIX socket path length on macOS (104 bytes including null). */
#define IPC_SOCKET_MAX_PATH 103

/* Sentinel docid: crawler sends a header with this docid and url_len=0,
 * path_len=0, depth=0 to signal end-of-crawl to the indexer. */
#define IPC_SENTINEL_DOCID  UINT32_MAX   /* 0xFFFFFFFF */

/*
 * Fixed-size binary header sent for each IPC message.
 *
 * Layout on the wire:
 *   [ipc_msg_header_t]  (10 bytes, packed, no padding)
 *   [url_len bytes]     (URL string, NOT null-terminated)
 *   [path_len bytes]    (filepath string, NOT null-terminated)
 *
 * Sentinel message: docid = IPC_SENTINEL_DOCID, url_len = 0, path_len = 0.
 * No body bytes follow the header for a sentinel.
 */
typedef struct __attribute__((packed)) {
    uint32_t docid;      /* document ID assigned by crawler (monotonic) */
    uint16_t depth;      /* crawl depth of this page (0 = seed)         */
    uint16_t url_len;    /* byte length of URL string following header   */
    uint16_t path_len;   /* byte length of filepath string following     */
} ipc_msg_header_t;

#endif /* IPC_PROTO_H */
