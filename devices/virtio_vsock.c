#include "virtio_vsock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* virtio 1.2, 5.10: queues rx, tx, event; a 44-byte header before each packet. */
#define RXQ 0
#define TXQ 1
#define HDR 44
#define HOST_CID 2
#define TYPE_STREAM 1
enum { OP_REQUEST = 1, OP_RESPONSE, OP_RST, OP_SHUTDOWN, OP_RW, OP_CREDIT_UPDATE, OP_CREDIT_REQUEST };
#define OUR_BUF (1u << 20) /* what we tell the guest it may send us ahead (we write it out at once) */
#define MAX_READ 65536

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static const char *op_name(int op) {
    static const char *n[] = {"?", "request", "response", "rst", "shutdown", "rw", "credit_update", "credit_request"};
    return op >= 1 && op <= 7 ? n[op] : "?";
}

static void wake_thread(HvVirtioVsock *d) {
    char c = 0;
    (void)!write(d->wake[1], &c, 1);
}

/* Queue a packet for the guest (mu held). c gives the ports and our credit. */
static void queue(HvVirtioVsock *d, HvVsockConn *c, int op, uint32_t flags, const void *data, uint32_t len) {
    HvVsockPkt *p = malloc(sizeof *p + len);
    if (!p) return;
    memset(p, 0, sizeof *p);
    put64(p->hdr + 0, HOST_CID);
    put64(p->hdr + 8, d->guest_cid);
    put32(p->hdr + 16, c->host_port);
    put32(p->hdr + 20, c->guest_port);
    put32(p->hdr + 24, len);
    put16(p->hdr + 28, TYPE_STREAM);
    put16(p->hdr + 30, (uint16_t)op);
    put32(p->hdr + 32, flags);
    put32(p->hdr + 36, OUR_BUF);
    put32(p->hdr + 40, (uint32_t)c->recv);
    c->credit_told = c->recv;
    p->len = len;
    if (len) memcpy(p->data, data, len);
    if (d->out_tail) d->out_tail->next = p;
    else d->out_head = p;
    d->out_tail = p;
    if (d->log) fprintf(stderr, "hengevm: vsock: -> guest %s %u:%u len %u\n", op_name(op), c->host_port, c->guest_port, len);
}

static HvVsockConn *find(HvVirtioVsock *d, uint32_t host_port, uint32_t guest_port) {
    for (int i = 0; i < HV_VSOCK_CONNS; i++) {
        HvVsockConn *c = &d->conn[i];
        if (c->fd >= 0 && c->host_port == host_port && c->guest_port == guest_port) return c;
    }
    return NULL;
}

static void drop(HvVirtioVsock *d, HvVsockConn *c) {
    if (d->log) fprintf(stderr, "hengevm: vsock: connection %u:%u closed\n", c->host_port, c->guest_port);
    close(c->fd);
    c->fd = -1;
}

static int write_all(int fd, const uint8_t *p, uint32_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w, n -= (uint32_t)w;
    }
    return 0;
}

/* The guest's TX queue: a packet a buffer. */
static void tx(HvVirtioVsock *d) {
    HvVirtio *v = &d->vio;
    static HvVirtqElem e;
    static uint8_t buf[HDR + MAX_READ];
    int any = 0;
    while (hv_virtq_pop(v, TXQ, &e) > 0) {
        uint32_t n = hv_virtq_read(v, &e, 0, buf, sizeof buf);
        hv_virtq_push(v, TXQ, &e, 0);
        any = 1;
        if (n < HDR) continue;
        uint32_t src_port = get32(buf + 16), dst_port = get32(buf + 20), len = get32(buf + 24);
        int op = get16(buf + 30);
        if (len > n - HDR) len = n - HDR;
        pthread_mutex_lock(&d->mu);
        HvVsockConn *c = find(d, dst_port, src_port);
        if (d->log) fprintf(stderr, "hengevm: vsock: <- guest %s %u:%u len %u%s\n", op_name(op), dst_port, src_port, len, c ? "" : " (no connection)");
        if (c) { /* every packet carries the guest's credit */
            c->peer_buf = get32(buf + 36);
            c->peer_fwd = get32(buf + 40);
        }
        if (!c) {
            if (op != OP_RST) { /* nobody on this side: refuse */
                HvVsockConn tmp = {.host_port = dst_port, .guest_port = src_port};
                queue(d, &tmp, OP_RST, 0, NULL, 0);
            }
        } else if (op == OP_RESPONSE && !c->open) {
            char line[32];
            int k = snprintf(line, sizeof line, "OK %u\n", c->host_port);
            c->open = 1;
            if (write_all(c->fd, (const uint8_t *)line, (uint32_t)k)) drop(d, c);
            wake_thread(d);
        } else if (op == OP_RW) {
            if (write_all(c->fd, buf + HDR, len)) {
                queue(d, c, OP_RST, 0, NULL, 0);
                drop(d, c);
            } else {
                c->recv += len;
                if (c->recv - c->credit_told >= OUR_BUF / 4) queue(d, c, OP_CREDIT_UPDATE, 0, NULL, 0);
            }
        } else if (op == OP_CREDIT_REQUEST) {
            queue(d, c, OP_CREDIT_UPDATE, 0, NULL, 0);
        } else if (op == OP_CREDIT_UPDATE) {
            wake_thread(d); /* it may be waiting for room */
        } else if (op == OP_SHUTDOWN || op == OP_RST || (op == OP_RESPONSE && c->open)) {
            if (op == OP_SHUTDOWN) queue(d, c, OP_RST, 0, NULL, 0);
            drop(d, c);
        }
        pthread_mutex_unlock(&d->mu);
    }
    if (any) hv_virtio_interrupt(v);
}

static void notify(HvVirtio *v, int q) {
    if (q == TXQ) tx(v->dev);
    else if (q == RXQ) hv_virtio_vsock_poll(v->dev); /* new buffers: maybe for what waits */
}

void hv_virtio_vsock_poll(HvVirtioVsock *d) {
    if (!d) return;
    HvVirtio *v = &d->vio;
    if (!(v->status & 4) || !v->q[RXQ].enabled) return; /* DRIVER_OK */
    static HvVirtqElem e;
    int any = 0;
    pthread_mutex_lock(&d->mu);
    while (d->out_head) {
        if (hv_virtq_pop(v, RXQ, &e) <= 0) break;
        HvVsockPkt *p = d->out_head;
        uint32_t room = hv_virtq_writable(&e);
        uint32_t chunk = p->len - p->off;
        if (room < HDR) chunk = 0;
        else if (chunk > room - HDR) chunk = room - HDR; /* the rest goes in the next buffer */
        uint8_t hdr[HDR];
        memcpy(hdr, p->hdr, HDR);
        put32(hdr + 24, chunk);
        hv_virtq_write(v, &e, 0, hdr, HDR);
        if (chunk) hv_virtq_write(v, &e, HDR, p->data + p->off, chunk);
        hv_virtq_push(v, RXQ, &e, HDR + chunk);
        any = 1;
        p->off += chunk;
        if (p->off >= p->len) {
            d->out_head = p->next;
            if (!d->out_head) d->out_tail = NULL;
            free(p);
        }
    }
    pthread_mutex_unlock(&d->mu);
    if (any) hv_virtio_interrupt(v);
}

/* A new client: "CONNECT <port>\n", then a request to the guest. */
static void accept_one(HvVirtioVsock *d) {
    int fd = accept(d->listen_fd, NULL, NULL);
    if (fd < 0) return;
    char line[64];
    size_t k = 0;
    while (k + 1 < sizeof line && read(fd, line + k, 1) == 1 && line[k] != '\n') k++;
    line[k] = 0;
    unsigned port = 0;
    if (sscanf(line, "CONNECT %u", &port) != 1) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&d->mu);
    HvVsockConn *c = NULL;
    for (int i = 0; i < HV_VSOCK_CONNS && !c; i++) if (d->conn[i].fd < 0) c = &d->conn[i];
    if (!c) {
        pthread_mutex_unlock(&d->mu);
        close(fd);
        return;
    }
    *c = (HvVsockConn){.fd = fd, .host_port = d->next_port++, .guest_port = port};
    if (d->next_port < 1024) d->next_port = 1024;
    queue(d, c, OP_REQUEST, 0, NULL, 0);
    pthread_mutex_unlock(&d->mu);
    hv_machine_wake(d->vio.m);
}

static void *host_thread(void *arg) {
    HvVirtioVsock *d = arg;
    for (;;) {
        struct pollfd pf[2 + HV_VSOCK_CONNS];
        int who[2 + HV_VSOCK_CONNS], n = 0;
        pf[n] = (struct pollfd){.fd = d->listen_fd, .events = POLLIN}, who[n++] = -1;
        pf[n] = (struct pollfd){.fd = d->wake[0], .events = POLLIN}, who[n++] = -2;
        pthread_mutex_lock(&d->mu);
        for (int i = 0; i < HV_VSOCK_CONNS; i++) {
            HvVsockConn *c = &d->conn[i];
            if (c->fd < 0 || !c->open) continue;
            uint32_t room = c->peer_buf - (uint32_t)(c->sent - c->peer_fwd);
            if (!room || room > c->peer_buf) continue; /* no credit: wait for an update */
            pf[n] = (struct pollfd){.fd = c->fd, .events = POLLIN}, who[n++] = i;
        }
        pthread_mutex_unlock(&d->mu);
        if (poll(pf, (nfds_t)n, 1000) <= 0) continue;
        if (pf[1].revents) {
            char b[64];
            (void)!read(d->wake[0], b, sizeof b);
        }
        if (pf[0].revents & POLLIN) accept_one(d);
        int queued = 0;
        static uint8_t buf[MAX_READ];
        for (int k = 2; k < n; k++) {
            if (!pf[k].revents) continue;
            pthread_mutex_lock(&d->mu);
            HvVsockConn *c = &d->conn[who[k]];
            uint32_t room = c->peer_buf - (uint32_t)(c->sent - c->peer_fwd);
            pthread_mutex_unlock(&d->mu);
            if (room > MAX_READ) room = MAX_READ;
            ssize_t r = c->fd >= 0 && room ? read(c->fd, buf, room) : 0;
            pthread_mutex_lock(&d->mu);
            if (c->fd >= 0) {
                if (r > 0) {
                    c->sent += (uint64_t)r;
                    queue(d, c, OP_RW, 0, buf, (uint32_t)r);
                } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) { /* the client went: both directions done */
                    queue(d, c, OP_SHUTDOWN, 3, NULL, 0);
                    drop(d, c);
                }
                queued = 1;
            }
            pthread_mutex_unlock(&d->mu);
        }
        if (queued) hv_machine_wake(d->vio.m);
    }
    return NULL;
}

HvVirtioVsock *hv_virtio_vsock_create(HvMachine *m, HvPcie *bus, uint64_t guest_cid, const char *dir) {
    HvVirtioVsock *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 19, 0x078000, 3, 0); /* socket device, "other communications" class */
    v->dev = d;
    v->notify = notify;
    v->config_len = 8;
    put64(v->config, guest_cid);
    d->guest_cid = guest_cid;
    d->next_port = 1024;
    d->log = getenv("HV_VSOCK_LOG") != NULL;
    pthread_mutex_init(&d->mu, NULL);
    for (int i = 0; i < HV_VSOCK_CONNS; i++) d->conn[i].fd = -1;
    hv_pcie_attach(bus, &v->pci);

    struct sockaddr_un a = {.sun_family = AF_UNIX};
    mkdir(dir, 0700);
    snprintf(a.sun_path, sizeof a.sun_path, "%s/%llu.sock", dir, (unsigned long long)guest_cid);
    unlink(a.sun_path);
    d->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->listen_fd < 0 || bind(d->listen_fd, (struct sockaddr *)&a, sizeof a) || listen(d->listen_fd, 4) || pipe(d->wake)) {
        fprintf(stderr, "hengevm: vsock: cannot listen on %s: %s\n", a.sun_path, strerror(errno));
        return d; /* the device is there; only the host side is missing */
    }
    fprintf(stderr, "hengevm: vsock: guest CID %llu, host side %s\n", (unsigned long long)guest_cid, a.sun_path);
    pthread_create(&d->thread, NULL, host_thread, d);
    return d;
}
