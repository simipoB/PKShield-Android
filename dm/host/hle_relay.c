// The room server's virtual network (dm/server/room_server.py): local play over the internet. With a room set (the in-game menu's Online room: DM_RELAY=host:port,
// DM_RELAY_ROOM=code), local play (hle_ldn.c) joins the room at Initialize and this phone gets a virtual address, 10.13.0.x; the game's sockets aimed at the
// virtual network (hle_net.c relay_owns) go through the server:
//   · out: a datagram to 10.13.x.x (or the network's broadcast) becomes a frame to the server, from this phone's address and the socket's own port
//   · in: a frame from the server is delivered to the game's socket on its port through the phone's loopback, from a small local sender standing for the remote
//     member's address and port, so the game's poll / blocking / timeouts work as on any socket; recvfrom reports the member's virtual address
// The link to the server is TCP: datagrams arrive in order and are not lost on the way
#include "hle.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { NET = 0x0A0D0000, MASK = 0xFFFF0000, NPROXY = 64, NFD = 4096 };
static int g_srv = -1; static uint32_t g_vip; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_wmu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t vip; uint16_t port, local; int fd; } g_px[NPROXY]; static int g_npx;   // a remote (address, port) → its loopback sender
static uint8_t g_relayed[NFD];   // the game's sockets bound on the virtual network
static void (*g_left)(uint32_t vip);

int relay_active(void) { return g_srv >= 0; }
uint32_t relay_vip(void) { return g_vip; }
void relay_on_left(void (*fn)(uint32_t)) { g_left = fn; }
int relay_owns(uint32_t a_be) { uint32_t a = ntohl(a_be); return g_srv >= 0 && ((a & MASK) == NET || a == 0xFFFFFFFFu); }

static int rd_all(int fd, void *b, size_t n) { uint8_t *p = b; while (n) { ssize_t r = recv(fd, p, n, 0); if (r <= 0) return 0; p += r; n -= (size_t)r; } return 1; }
static int wr_all(int fd, const void *b, size_t n) { const uint8_t *p = b; while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return 0; p += w; n -= (size_t)w; } return 1; }
static int send_frame(const uint8_t *f, uint32_t n) { uint32_t be = htonl(n); pthread_mutex_lock(&g_wmu); int ok = g_srv >= 0 && wr_all(g_srv, &be, 4) && wr_all(g_srv, f, n); pthread_mutex_unlock(&g_wmu); return ok; }

static int proxy_for(uint32_t vip, uint16_t port) {   // (holding g_mu) the loopback sender standing for this member's socket
    for (int i = 0; i < g_npx; i++) if (g_px[i].vip == vip && g_px[i].port == port) return i;
    int i = g_npx < NPROXY ? g_npx++ : (int)(((vip * 31u) ^ port) % NPROXY);   // (a full table reuses a slot)
    if (i < g_npx - 1 || g_px[i].fd > 0) close(g_px[i].fd);
    int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }; socklen_t l = sizeof a;
    bind(s, (struct sockaddr *)&a, sizeof a); getsockname(s, (struct sockaddr *)&a, &l);
    g_px[i].vip = vip; g_px[i].port = port; g_px[i].fd = s; g_px[i].local = ntohs(a.sin_port); return i;
}
static void *reader(void *p) {
    (void)p; static uint8_t f[70000];
    for (;;) {
        uint32_t be; if (!rd_all(g_srv, &be, 4)) break; uint32_t n = ntohl(be); if (n == 0 || n > sizeof f || !rd_all(g_srv, f, n)) break;
        if (f[0] == 'D' && n >= 13) {
            uint32_t src, dst; uint16_t sp, dp; memcpy(&src, f + 1, 4); memcpy(&sp, f + 5, 2); memcpy(&dst, f + 7, 4); memcpy(&dp, f + 11, 2);
            pthread_mutex_lock(&g_mu); int i = proxy_for(ntohl(src), ntohs(sp)); int fd = g_px[i].fd; pthread_mutex_unlock(&g_mu);
            struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = dp, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
            sendto(fd, f + 13, n - 13, 0, (struct sockaddr *)&to, sizeof to);
        } else if (f[0] == 'L' && n >= 5) { uint32_t a; memcpy(&a, f + 1, 4); if (g_left) g_left(ntohl(a)); }
    }
    LOG("[relay] the room server closed the connection");
    pthread_mutex_lock(&g_mu); if (g_srv >= 0) { close(g_srv); g_srv = -1; } pthread_mutex_unlock(&g_mu);
    if (g_left) g_left(0);   // (0: everyone: the network is gone)
    return NULL;
}
// joins the room (hle_ldn.c Initialize): 1 when this phone has its virtual address
int relay_start(void) {
    if (g_srv >= 0) return 1;
    const char *hp = getenv("DM_RELAY"), *room = getenv("DM_RELAY_ROOM"); if (!hp || !*hp || !room || !*room) return 0;
    char host[256]; snprintf(host, sizeof host, "%s", hp); char *c = strrchr(host, ':'); const char *port = "11450"; if (c) { *c = 0; port = c + 1; }
    struct addrinfo hint = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, port, &hint, &ai) || !ai) { LOG("[RED] [relay] cannot find the room server %s", hp); return 0; }
    int s = socket(AF_INET, SOCK_STREAM, 0); struct timeval tv = { 8, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(s, ai->ai_addr, ai->ai_addrlen)) { LOG("[RED] [relay] cannot reach the room server %s (%s)", hp, strerror(errno)); freeaddrinfo(ai); close(s); return 0; }
    freeaddrinfo(ai);
    const char *name = getenv("DM_RELAY_NAME"); if (!name) name = "player";
    uint8_t h[300]; size_t rl = strlen(room) < 32 ? strlen(room) : 32, nl = strlen(name) < 32 ? strlen(name) : 32; size_t k = 0;
    h[k++] = 'H'; h[k++] = 1; h[k++] = (uint8_t)rl; memcpy(h + k, room, rl); k += rl; h[k++] = (uint8_t)nl; memcpy(h + k, name, nl); k += nl;
    g_srv = s; uint8_t w[64]; uint32_t be, n;
    if (!send_frame(h, (uint32_t)k) || !rd_all(s, &be, 4) || (n = ntohl(be)) > sizeof w || !rd_all(s, w, n) || w[0] != 'W' || n < 9) {
        LOG("[RED] [relay] the room server refused this phone%s%.*s", n > 1 && n <= sizeof w && w[0] == 'F' ? ": " : "", n > 1 && n <= sizeof w ? (int)n - 1 : 0, (const char *)w + 1);
        close(s); g_srv = -1; return 0; }
    memcpy(&be, w + 1, 4); g_vip = ntohl(be);
    tv.tv_sec = 0; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    pthread_t t; if (!pthread_create(&t, NULL, reader, NULL)) pthread_detach(t);
    struct in_addr a = { htonl(g_vip) }; LOG("[relay] in room \"%s\" on %s as %s", room, hp, inet_ntoa(a));
    return 1;
}
void relay_stop(void) { pthread_mutex_lock(&g_mu); if (g_srv >= 0) { shutdown(g_srv, SHUT_RDWR); } pthread_mutex_unlock(&g_mu); }

// ---------- the game's sockets (hle_net.c) and local play's own (hle_ldn.c) ----------
int relay_bind(int fd, const struct sockaddr_in *a) {   // a virtual address: the socket takes the port on the phone (any address), and is marked as on the virtual network
    struct sockaddr_in h = *a; h.sin_addr.s_addr = htonl(INADDR_ANY); int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int r = bind(fd, (struct sockaddr *)&h, sizeof h); if (!r && fd >= 0 && fd < NFD) g_relayed[fd] = 1; return r;
}
void relay_close(int fd) { if (fd >= 0 && fd < NFD) g_relayed[fd] = 0; }
int relay_sockname(int fd, struct sockaddr_in *a) {
    if (fd < 0 || fd >= NFD || !g_relayed[fd] || g_srv < 0) return 0;
    socklen_t l = sizeof *a; if (getsockname(fd, (struct sockaddr *)a, &l)) return 0; a->sin_addr.s_addr = htonl(g_vip); return 1;
}
long relay_sendto(int fd, const void *b, unsigned long n, const struct sockaddr_in *to) {
    if (n > 65507) { errno = EMSGSIZE; return -1; }
    struct sockaddr_in me; socklen_t l = sizeof me;
    if (getsockname(fd, (struct sockaddr *)&me, &l) || !me.sin_port) {   // not bound yet: an ephemeral port, as sendto does
        struct sockaddr_in any = { .sin_family = AF_INET }; bind(fd, (struct sockaddr *)&any, sizeof any); l = sizeof me; getsockname(fd, (struct sockaddr *)&me, &l); }
    if (fd >= 0 && fd < NFD) g_relayed[fd] = 1;
    static __thread uint8_t f[13 + 65507]; f[0] = 'D'; uint32_t src = htonl(g_vip); memcpy(f + 1, &src, 4); memcpy(f + 5, &me.sin_port, 2);
    memcpy(f + 7, &to->sin_addr, 4); memcpy(f + 11, &to->sin_port, 2); memcpy(f + 13, b, n);
    if (!send_frame(f, (uint32_t)(13 + n))) { errno = ENETDOWN; return -1; }
    return (long)n;
}
long relay_recvfrom(int fd, void *b, unsigned long n, int flags, struct sockaddr_in *from) {
    if (fd < 0 || fd >= NFD || !g_relayed[fd] || g_srv < 0) return -2;   // not on the virtual network: the plain path
    struct sockaddr_in h; socklen_t l = sizeof h; long r = recvfrom(fd, b, n, flags, (struct sockaddr *)&h, &l);
    if (r < 0) return -1;
    *from = h;
    if (h.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {   // from a loopback sender: the member it stands for
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_npx; i++) if (g_px[i].local == ntohs(h.sin_port)) { from->sin_addr.s_addr = htonl(g_px[i].vip); from->sin_port = htons(g_px[i].port); break; }
        pthread_mutex_unlock(&g_mu);
    }
    return r;
}
