// Networking: online services (play, friends, store, cloud saves) stay off, the game takes their offline branch; its sockets are the phone's (local play).
// Cuts: nifm reports no network; socket only hooks up Initialize (returns success, creates no session), the rest of socket/ssl/friends are hooked up as failures when they come into use.
#include "hle.h"

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "connects to the nifm network management service; offline (cut)", Result, h_nifm_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "offline: the network request is not submitted", void, h_nifm_Submit, void) {}
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "offline: no request to cancel", void, h_nifm_Cancel, void) {}
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "offline: the request is not on hold", int, h_nifm_OnHold, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "offline: the network is always unavailable", int, h_nifm_Available, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "offline: reports failure, shows no system network screen", Result, h_nifm_HandleErr, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "offline: no IP", Result, h_nifm_GetIp, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "offline: no proxy setting", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "connects to the ssl service; the network library's global init needs it to succeed, offline creates no session (cut)", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "pairs with nnsslInitialize (cut)", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- socket: the game's BSD sockets on the phone's own (local play: hle_ldn.c; the room server's virtual network: hle_relay.c) ----------
// Horizon's socket API is FreeBSD's: its sockaddr starts with a length byte, SOL_SOCKET is 0xffff, option and flag numbers differ, errors are FreeBSD's. Every
// overload the game calls (nn::socket's typed ones, its POSIX-style ones, the C nnsocket* interface) goes to the one implementation below; the game's descriptors
// are the phone's. Name resolution and online services stay off (nifm above reports no network, GetAddrInfo fails): only addresses the game already has (the local
// network's members) are reachable
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
static __thread int t_skerr;
static int bsd_errno(int e) {   // Linux → FreeBSD
    switch (e) { case EAGAIN: return 35; case EINPROGRESS: return 36; case EALREADY: return 37; case ENOTSOCK: return 38; case EDESTADDRREQ: return 39; case EMSGSIZE: return 40;
        case EPROTOTYPE: return 41; case ENOPROTOOPT: return 42; case EPROTONOSUPPORT: return 43; case EOPNOTSUPP: return 45; case EAFNOSUPPORT: return 47; case EADDRINUSE: return 48;
        case EADDRNOTAVAIL: return 49; case ENETDOWN: return 50; case ENETUNREACH: return 51; case ECONNABORTED: return 53; case ECONNRESET: return 54; case ENOBUFS: return 55;
        case EISCONN: return 56; case ENOTCONN: return 57; case ETIMEDOUT: return 60; case ECONNREFUSED: return 61; case EHOSTUNREACH: return 65; default: return e; }
}
static int sk_err(void) { t_skerr = bsd_errno(errno); return -1; }
static int sk_ret(long r) { if (r < 0) return sk_err(); t_skerr = 0; return (int)r; }
// FreeBSD sockaddr_in: u8 len, u8 family, u16 port, u32 address (network order), 8 zero bytes
static int to_host(const uint8_t *a, unsigned n, struct sockaddr_in *o) {
    if (!a || n < 8) { errno = EINVAL; return 0; } memset(o, 0, sizeof *o); o->sin_family = a[1] ? a[1] : AF_INET; memcpy(&o->sin_port, a + 2, 2); memcpy(&o->sin_addr, a + 4, 4); return 1;
}
static void from_host(const struct sockaddr_in *h, uint8_t *a, unsigned *n) {
    if (!a || !n) return; uint8_t b[16] = { 16, (uint8_t)h->sin_family }; memcpy(b + 2, &h->sin_port, 2); memcpy(b + 4, &h->sin_addr, 4);
    unsigned k = *n < 16 ? *n : 16; memcpy(a, b, k); *n = 16;
}
static int msg_flags(int f) { int o = 0; if (f & 0x1) o |= MSG_OOB; if (f & 0x2) o |= MSG_PEEK; if (f & 0x4) o |= MSG_DONTROUTE; if (f & 0x40) o |= MSG_WAITALL; if (f & 0x80) o |= MSG_DONTWAIT; return o | MSG_NOSIGNAL; }
static int sock_opt(int *lvl, int *opt) {   // FreeBSD level / option → Linux; 0 when the phone has no such option (accepted and ignored)
    if (*lvl == 0xffff) { *lvl = SOL_SOCKET;
        switch (*opt) { case 0x4: *opt = SO_REUSEADDR; return 1; case 0x8: *opt = SO_KEEPALIVE; return 1; case 0x20: *opt = SO_BROADCAST; return 1; case 0x80: *opt = SO_LINGER; return 1;
            case 0x200: *opt = SO_REUSEPORT; return 1; case 0x1001: *opt = SO_SNDBUF; return 1; case 0x1002: *opt = SO_RCVBUF; return 1; case 0x1005: *opt = SO_SNDTIMEO; return 1;
            case 0x1006: *opt = SO_RCVTIMEO; return 1; case 0x1007: *opt = SO_ERROR; return 1; case 0x1008: *opt = SO_TYPE; return 1; default: return 0; } }
    if (*lvl == IPPROTO_TCP) return *opt == 1 ? (*opt = TCP_NODELAY, 1) : 0;
    if (*lvl == IPPROTO_IP) switch (*opt) { case 3: *opt = IP_TOS; return 1; case 4: *opt = IP_TTL; return 1; case 9: *opt = IP_MULTICAST_IF; return 1; case 10: *opt = IP_MULTICAST_TTL; return 1;
        case 11: *opt = IP_MULTICAST_LOOP; return 1; case 12: *opt = IP_ADD_MEMBERSHIP; return 1; case 13: *opt = IP_DROP_MEMBERSHIP; return 1; default: return 0; }
    return 0;
}
// the room server's virtual network (hle_relay.c): an address it owns goes through it; weak, so a build without it is plain sockets
__attribute__((weak)) int relay_owns(uint32_t addr_be);
__attribute__((weak)) long relay_sendto(int fd, const void *b, unsigned long n, const struct sockaddr_in *to);
__attribute__((weak)) long relay_recvfrom(int fd, void *b, unsigned long n, int flags, struct sockaddr_in *from);
__attribute__((weak)) int relay_bind(int fd, const struct sockaddr_in *a);
__attribute__((weak)) void relay_close(int fd);
__attribute__((weak)) int relay_sockname(int fd, struct sockaddr_in *a);

static int sk_socket(int dom, int type, int proto) { if (dom != 2) { t_skerr = 47; return -1; } return sk_ret(socket(AF_INET, type & 0xf, proto)); }
static int sk_bind(int s, const uint8_t *a, unsigned n) {
    struct sockaddr_in h; if (!to_host(a, n, &h)) return sk_err();
    if (relay_owns && relay_bind && relay_owns(h.sin_addr.s_addr)) { int r = relay_bind(s, &h); return r < 0 ? sk_err() : sk_ret(0); }   // the virtual address: the phone binds any address
    return sk_ret(bind(s, (struct sockaddr *)&h, sizeof h));
}
static int sk_connect(int s, const uint8_t *a, unsigned n) { struct sockaddr_in h; if (!to_host(a, n, &h)) return sk_err(); return sk_ret(connect(s, (struct sockaddr *)&h, sizeof h)); }
static int sk_accept(int s, uint8_t *a, unsigned *n) { struct sockaddr_in h; socklen_t l = sizeof h; int r = accept(s, (struct sockaddr *)&h, &l); if (r >= 0) from_host(&h, a, n); return sk_ret(r); }
static int sk_name(int s, uint8_t *a, unsigned *n, int peer) {
    struct sockaddr_in h; socklen_t l = sizeof h;
    if (!peer && relay_sockname && relay_sockname(s, &h)) { from_host(&h, a, n); return sk_ret(0); }
    int r = peer ? getpeername(s, (struct sockaddr *)&h, &l) : getsockname(s, (struct sockaddr *)&h, &l); if (r == 0) from_host(&h, a, n); return sk_ret(r);
}
static int sk_close(int s) { if (relay_close) relay_close(s); return sk_ret(close(s)); }
static long sk_recvfrom(int s, void *b, unsigned long n, int f, uint8_t *a, unsigned *al) {
    struct sockaddr_in h; socklen_t l = sizeof h; long r;
    if (relay_recvfrom && (r = relay_recvfrom(s, b, n, msg_flags(f), &h)) != -2) { if (r >= 0) from_host(&h, a, al); return r < 0 ? sk_err() : sk_ret(r); }   // (-2: not a relayed socket)
    r = recvfrom(s, b, n, msg_flags(f), (struct sockaddr *)&h, &l); if (r >= 0 && a) from_host(&h, a, al); return r < 0 ? sk_err() : (t_skerr = 0, r);
}
static long sk_sendto(int s, const void *b, unsigned long n, int f, const uint8_t *a, unsigned al) {
    if (!a) { long r = send(s, b, n, msg_flags(f)); return r < 0 ? sk_err() : (t_skerr = 0, r); }
    struct sockaddr_in h; if (!to_host(a, al, &h)) return sk_err();
    if (relay_owns && relay_sendto && relay_owns(h.sin_addr.s_addr)) { long r = relay_sendto(s, b, n, &h); return r < 0 ? sk_err() : (t_skerr = 0, r); }
    long r = sendto(s, b, n, msg_flags(f), (struct sockaddr *)&h, sizeof h); return r < 0 ? sk_err() : (t_skerr = 0, r);
}
static int sk_setopt(int s, int l, int o, const void *v, unsigned n) {
    if (l == 0xffff && o == 0x1100) { int fl = (fcntl)(s, F_GETFL); return sk_ret((fcntl)(s, F_SETFL, v && n >= 4 && *(const int *)v ? fl | O_NONBLOCK : fl & ~O_NONBLOCK)); }   // Horizon's SO_NONBLOCK
    if (!sock_opt(&l, &o)) { t_skerr = 0; return 0; }
    if (o == SO_RCVTIMEO || o == SO_SNDTIMEO) { struct timeval tv = {0}; if (v && n >= 16) memcpy(&tv, v, sizeof tv); return sk_ret(setsockopt(s, l, o, &tv, sizeof tv)); }
    return sk_ret(setsockopt(s, l, o, v, n));
}
static int sk_getopt(int s, int l, int o, void *v, unsigned *n) {
    if (!sock_opt(&l, &o)) { if (v && n && *n >= 4) { memset(v, 0, 4); *n = 4; } t_skerr = 0; return 0; }
    socklen_t k = n ? *n : 0; int r = getsockopt(s, l, o, v, &k); if (n) *n = k;
    if (!r && o == SO_ERROR && v && k >= 4) *(int *)v = bsd_errno(*(int *)v);
    return sk_ret(r);
}
static int sk_fcntl(int s, int cmd, int arg) {
    if (cmd == 3) { int fl = (fcntl)(s, F_GETFL); return fl < 0 ? sk_err() : (t_skerr = 0, fl & O_NONBLOCK ? 0x4 : 0); }     // F_GETFL: FreeBSD's O_NONBLOCK is 0x4
    if (cmd == 4) { int fl = (fcntl)(s, F_GETFL); return sk_ret((fcntl)(s, F_SETFL, arg & 0x4 ? fl | O_NONBLOCK : fl & ~O_NONBLOCK)); }
    t_skerr = 0; return 0;
}
static int sk_poll(struct pollfd *f, unsigned long n, int t) { return sk_ret(poll(f, (nfds_t)n, t)); }   // (struct pollfd and its bits are the same)
static int sk_shutdown(int s, int how) { return sk_ret(shutdown(s, how)); }
static long sk_recv(int s, void *b, unsigned long n, int f) { return sk_recvfrom(s, b, n, f, NULL, NULL); }
static long sk_send(int s, const void *b, unsigned long n, int f) { long r = send(s, b, n, msg_flags(f)); return r < 0 ? sk_err() : (t_skerr = 0, r); }

#define SKI(m, cname, ret, args, call) HLE_FORCE(m, "bsd socket on the phone's own", ret, cname, args) { return call; }
#define A2(...) __VA_ARGS__
// nn::socket, typed overloads
SKI("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int, A2(int a, int b, int c), sk_socket(a, b, c))
SKI("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int, A2(int s, const uint8_t *a, unsigned n), sk_bind(s, a, n))
SKI("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int, A2(int s, const uint8_t *a, unsigned n), sk_connect(s, a, n))
SKI("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int, A2(int s, uint8_t *a, unsigned *n), sk_accept(s, a, n))
SKI("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int, A2(int s, uint8_t *a, unsigned *n), sk_name(s, a, n, 1))
SKI("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int, A2(int s, uint8_t *a, unsigned *n), sk_name(s, a, n, 0))
SKI("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int, A2(int s, int l, int o, void *v, unsigned *n), sk_getopt(s, l, o, v, n))
SKI("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int, A2(int s, int l, int o, const void *v, unsigned n), sk_setopt(s, l, o, v, n))
SKI("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int, A2(int s, int c, int a), sk_fcntl(s, c, a))
SKI("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, int, A2(struct pollfd *f, unsigned long n, int t), sk_poll(f, n, t))
SKI("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", h_sk_Recv, long, A2(int s, void *b, unsigned long n, int f), sk_recv(s, b, n, f))
SKI("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", h_sk_RecvFrom, long, A2(int s, void *b, unsigned long n, int f, uint8_t *a, unsigned *al), sk_recvfrom(s, b, n, f, a, al))
SKI("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", h_sk_Send, long, A2(int s, const void *b, unsigned long n, int f), sk_send(s, b, n, f))
SKI("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", h_sk_SendTo, long, A2(int s, const void *b, unsigned long n, int f, const uint8_t *a, unsigned al), sk_sendto(s, b, n, f, a, al))
// nn::socket, POSIX-style overloads
SKI("_ZN2nn6socket6SocketEiii", h_sk_Socket2, int, A2(int a, int b, int c), sk_socket(a, b, c))
SKI("_ZN2nn6socket4BindEiPK8sockaddrj", h_sk_Bind2, int, A2(int s, const uint8_t *a, unsigned n), sk_bind(s, a, n))
SKI("_ZN2nn6socket7ConnectEiPK8sockaddrj", h_sk_Connect2, int, A2(int s, const uint8_t *a, unsigned n), sk_connect(s, a, n))
SKI("_ZN2nn6socket6AcceptEiP8sockaddrPj", h_sk_Accept2, int, A2(int s, uint8_t *a, unsigned *n), sk_accept(s, a, n))
SKI("_ZN2nn6socket6ListenEii", h_sk_Listen, int, A2(int s, int n), sk_ret(listen(s, n)))
SKI("_ZN2nn6socket5CloseEi", h_sk_Close, int, A2(int s), sk_close(s))
SKI("_ZN2nn6socket8ShutdownEii", h_sk_Shutdown, int, A2(int s, int h), sk_shutdown(s, h))
SKI("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", h_sk_GetSockName2, int, A2(int s, uint8_t *a, unsigned *n), sk_name(s, a, n, 0))
SKI("_ZN2nn6socket10GetSockOptEiiiPvPj", h_sk_GetSockOpt2, int, A2(int s, int l, int o, void *v, unsigned *n), sk_getopt(s, l, o, v, n))
SKI("_ZN2nn6socket10SetSockOptEiiiPKvj", h_sk_SetSockOpt2, int, A2(int s, int l, int o, const void *v, unsigned n), sk_setopt(s, l, o, v, n))
SKI("_ZN2nn6socket5FcntlEiiz", h_sk_Fcntl2, int, A2(int s, int c, int a), sk_fcntl(s, c, a))
SKI("_ZN2nn6socket4PollEP6pollfdmi", h_sk_Poll2, int, A2(struct pollfd *f, unsigned long n, int t), sk_poll(f, n, t))
SKI("_ZN2nn6socket4RecvEiPvmi", h_sk_Recv2, long, A2(int s, void *b, unsigned long n, int f), sk_recv(s, b, n, f))
SKI("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", h_sk_RecvFrom2, long, A2(int s, void *b, unsigned long n, int f, uint8_t *a, unsigned *al), sk_recvfrom(s, b, n, f, a, al))
SKI("_ZN2nn6socket4SendEiPKvmi", h_sk_Send2, long, A2(int s, const void *b, unsigned long n, int f), sk_send(s, b, n, f))
SKI("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", h_sk_SendTo2, long, A2(int s, const void *b, unsigned long n, int f, const uint8_t *a, unsigned al), sk_sendto(s, b, n, f, a, al))
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "socket library finalize: nothing to release", Result, h_sk_Finalize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "socket error code kept by the host", int, h_sk_GetLastErrno, void) { return t_skerr; }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
// name resolution: off (local play reaches only the members' addresses; nothing online)
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (off): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4; }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "name resolution (off): EAI_FAIL", int, h_sk_GetAddrInfo2, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo", void, h_sk_FreeAddrInfo, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "pairs with GetAddrInfo", void, h_sk_FreeAddrInfo2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution (off): NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = 50; return NULL; }
HLE_FORCE("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", "resolver (off)", int, h_sk_ResGet, void *o, int k) { (void)o; (void)k; t_skerr = 50; return -1; }
HLE_FORCE("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", "resolver (off)", int, h_sk_ResSet, const void *o) { (void)o; t_skerr = 50; return -1; }
// C interface
SKI("nnsocketSocket", h_nsk_Socket, int, A2(int a, int b, int c), sk_socket(a, b, c))
SKI("nnsocketBind", h_nsk_Bind, int, A2(int s, const uint8_t *a, unsigned n), sk_bind(s, a, n))
SKI("nnsocketConnect", h_nsk_Connect, int, A2(int s, const uint8_t *a, unsigned n), sk_connect(s, a, n))
SKI("nnsocketClose", h_nsk_Close, int, A2(int s), sk_close(s))
SKI("nnsocketGetPeerName", h_nsk_GetPeerName, int, A2(int s, uint8_t *a, unsigned *n), sk_name(s, a, n, 1))
SKI("nnsocketGetSockName", h_nsk_GetSockName, int, A2(int s, uint8_t *a, unsigned *n), sk_name(s, a, n, 0))
SKI("nnsocketGetSockOpt", h_nsk_GetSockOpt, int, A2(int s, int l, int o, void *v, unsigned *n), sk_getopt(s, l, o, v, n))
SKI("nnsocketSetSockOpt", h_nsk_SetSockOpt, int, A2(int s, int l, int o, const void *v, unsigned n), sk_setopt(s, l, o, v, n))
SKI("nnsocketFcntl", h_nsk_Fcntl, int, A2(int s, int c, int a), sk_fcntl(s, c, a))
SKI("nnsocketPoll", h_nsk_Poll, int, A2(struct pollfd *f, unsigned long n, int t), sk_poll(f, n, t))
SKI("nnsocketRecv", h_nsk_Recv, long, A2(int s, void *b, unsigned long n, int f), sk_recv(s, b, n, f))
SKI("nnsocketSend", h_nsk_Send, long, A2(int s, const void *b, unsigned long n, int f), sk_send(s, b, n, f))
HLE_FORCE("nnsocketResolverGetOption", "resolver (off)", int, h_nsk_ResGet, void *o, int k) { (void)o; (void)k; t_skerr = 50; return -1; }
HLE_FORCE("nnsocketResolverSetOption", "resolver (off)", int, h_nsk_ResSet, const void *o) { (void)o; t_skerr = 50; return -1; }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution (off): NULL", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = 50; return NULL; }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl: offline, Initialize succeeds (the network library's global initialization depends on it), everything else that contacts the service fails ----------
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "same as nnsslInitialize: succeeds, creates no session (cut)", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "pairs with Initialize (cut)", Result, h_ssl_FinCpp, void) { return RES_OK; }
SSF("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context7DestroyEv", h_ssl_CtxDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) { (void)self; (void)s; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl service (cut): read returns -1", int, h_ssl_CnRead, void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl service (cut): write returns -1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
// C interface: all return nnResult
#define SSC(m, cname) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, void) { return RES_FAIL; }
SSC("nnsslConnectionCreate", h_nssl_Create) SSC("nnsslConnectionDestroy", h_nssl_Destroy)
SSC("nnsslConnectionDoHandshakeWithCertBuffer", h_nssl_Handshake) SSC("nnsslConnectionGetConnectionId", h_nssl_GetId)
SSC("nnsslConnectionGetNextAlpnProto", h_nssl_GetAlpn) SSC("nnsslConnectionGetVerifyCertError", h_nssl_VerifyErr)
SSC("nnsslConnectionPeek", h_nssl_Peek) SSC("nnsslConnectionPending", h_nssl_Pending) SSC("nnsslConnectionRead", h_nssl_Read)
SSC("nnsslConnectionSetHostName", h_nssl_SetHost) SSC("nnsslConnectionSetIoMode", h_nssl_SetIo)
SSC("nnsslConnectionSetNextAlpnProto", h_nssl_SetAlpn) SSC("nnsslConnectionSetOption", h_nssl_SetOpt)
SSC("nnsslConnectionSetSessionCacheMode", h_nssl_SetCache) SSC("nnsslConnectionSetSocketDescriptor", h_nssl_SetSock)
SSC("nnsslConnectionSetVerifyOption", h_nssl_SetVerify) SSC("nnsslConnectionWrite", h_nssl_Write)
SSC("nnsslContextDestroy", h_nssl_CtxDestroy) SSC("nnsslContextGetContextId", h_nssl_CtxGetId)
