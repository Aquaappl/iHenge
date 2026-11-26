#ifndef HENGEVM_VIRTIO_VSOCK_H
#define HENGEVM_VIRTIO_VSOCK_H

#include <pthread.h>

#include "virtio.h"

/* virtio-vsock (device type 19): Virtualization.framework's VZVirtioSocketDevice, which
   vphone-cli's guest daemon (vphoned, port 1339) listens on. The host side is a Unix
   socket in the Firecracker style qemu-vphone's virtio-vsock-uds uses: a client writes
   "CONNECT <port>\n", gets "OK <host port>\n", then the stream. vphone-linux's
   tools/vphoned.sh talks to it with VSOCK_DIR set to its directory. Streams only. */
#define HV_VSOCK_CONNS 16

typedef struct HvVsockPkt {
    struct HvVsockPkt *next;
    uint8_t hdr[44];
    uint32_t len, off;   /* payload, and how much of it went already */
    uint8_t data[];
} HvVsockPkt;

typedef struct HvVsockConn {
    int fd;                      /* the host client; -1: free */
    uint32_t host_port, guest_port;
    int open;                    /* the guest answered the request */
    uint32_t peer_buf, peer_fwd; /* the guest's credit */
    uint64_t sent;               /* stream bytes queued to the guest */
    uint64_t recv;               /* stream bytes from the guest written to fd */
    uint64_t credit_told;        /* recv when we last told the guest */
} HvVsockConn;

typedef struct HvVirtioVsock {
    HvVirtio vio;
    uint64_t guest_cid;
    pthread_mutex_t mu;          /* everything below */
    HvVsockConn conn[HV_VSOCK_CONNS];
    HvVsockPkt *out_head, *out_tail; /* packets for the guest's RX queue */
    int listen_fd, wake[2];
    uint32_t next_port;
    pthread_t thread;
    int log;
} HvVirtioVsock;

/* Listens on <dir>/<cid>.sock. */
HvVirtioVsock *hv_virtio_vsock_create(HvMachine *m, HvPcie *bus, uint64_t guest_cid, const char *dir);
/* CPU thread: hand queued packets to the guest. */
void hv_virtio_vsock_poll(HvVirtioVsock *d);

#endif
