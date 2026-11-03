// nn::ldn, local wireless (link battles, trades and Max Raids nearby), over the phone's Wi-Fi: the phones on one network play as consoles in range.
//   · access point (the game hosts): CreateNetwork builds the NetworkInfo from the phone's Wi-Fi address and announces it every 0.5 s (UDP broadcast, port 11452);
//     stations join over TCP (port 11453): the host checks the game's passphrase and its player limit, gives a node, and sends the node list to every member
//     whenever it changes (a member gone too); the state change event fires each time
//   · station (the game looks for others): Scan listens for the announcements (1 s) and returns those of the same game family (Sword and Shield play together,
//     as on a Switch) that pass the game's scan filter; Connect joins the host, then follows its node list; the host gone = signal lost
//   · the game's own traffic (battle, trade data) then goes over its sockets to the members' real Wi-Fi addresses (hle_net.c: the phone's sockets)
// With a room set (DM_RELAY / DM_RELAY_ROOM: the in-game menu's Online room) the same runs over the room server's virtual network instead (hle_relay.c): this phone's
// address is its virtual one (10.13.0.x), the announcements go to the network's broadcast, and joins and node lists are datagrams on port 11453 (the server link is TCP:
// they arrive, in order); a member gone is the server's notice.
// Off while mods are on (DM_MODS_ACTIVE, android_main.c boot_guard): Initialize reports failure, so the game says local play is unavailable; DM_LDN=0 turns it off too.
// Structures (offsets in bytes) as switchbrew documents them: NetworkInfo 0x480 = NetworkId 0x20 (IntentId 0x10: LocalCommunicationId u64, SceneId u16 at +0xa;
// SessionId 0x10), CommonNetworkInfo 0x30 at +0x20 (Bssid 6, Ssid 0x22 {u8 length, 33 bytes}, Channel s16 +0x28, LinkLevel +0x2a, NetworkType +0x2b),
// LdnNetworkInfo 0x430 at +0x50 (ServerRandom 16, SecurityMode u16 +0x10, StationAcceptPolicy +0x12, NodeCountMax +0x16, NodeCount +0x17, 8 NodeInfo of 0x40 at
// +0x18 {Ipv4 u32, Mac 6, NodeId +0xa, IsConnected +0xb, UserName 0x21 +0xc, LocalCommunicationVersion s16 +0x2e}, AdvertiseDataSize u16 +0x21a, AdvertiseData 0x180 +0x21c)
#include "hle.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { ST_NONE, ST_INIT, ST_AP, ST_AP_CREATED, ST_STATION, ST_CONNECTED, ST_ERROR };
enum { NI_SIZE = 0x480, LDN = 0x50, NODES = LDN + 0x18, NODE = 0x40, MAXN = 8, PORT_BEACON = 11452, PORT_JOIN = 11453 };
static const char MAGIC[8] = "PKLDN1\0";

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_state, g_reason, g_node = -1, g_beacon = -1, g_listen = -1, g_host = -1, g_member[MAXN];
static uint8_t g_ni[NI_SIZE], g_sec[0x44];   // the network (as the game reads it), the host's SecurityConfig
static void *g_ev; static uint32_t g_ip, g_mask; static pthread_t g_th; static int g_th_on;
// the room server (hle_relay.c): the virtual network's sockets, the members by virtual address and port
int relay_start(void); int relay_active(void); uint32_t relay_vip(void); void relay_on_left(void (*fn)(uint32_t));
int relay_bind(int fd, const struct sockaddr_in *a); long relay_sendto(int fd, const void *b, unsigned long n, const struct sockaddr_in *to);
long relay_recvfrom(int fd, void *b, unsigned long n, int flags, struct sockaddr_in *from);
static int g_relay, g_join = -1; static uint32_t g_maddr[MAXN]; static uint16_t g_mport[MAXN]; static struct sockaddr_in g_hostaddr;
static long vsend(int fd, const void *b, size_t n, uint32_t ip, uint16_t port) {   // a datagram on the virtual network
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip) }; return relay_sendto(fd, b, n, &to); }

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void ev(void) { if (g_ev) hle_signal_system_event(g_ev); }
static int mods_on(void) { const char *m = getenv("DM_MODS_ACTIVE"); return m && atoi(m) > 0; }
static int family(uint64_t a, uint64_t b) {   // the same game, or Sword with Shield (their local play is one)
    uint64_t sw = 0x0100ABF008968000ull, sh = 0x01008DB008C2C000ull;
    return a == b || ((a == sw || a == sh) && (b == sw || b == sh));
}
static uint64_t my_program(void) { uint64_t dm_program_id(void); return dm_program_id(); }

// the phone's Wi-Fi address (the first IPv4 that is up and not loopback; wlan first), numeric (a.b.c.d = a << 24 | ...), as nn::ldn::Ipv4Address holds it
static int wifi_addr(uint32_t *ip, uint32_t *mask) {
    struct ifaddrs *ia, *i; if (getifaddrs(&ia)) return 0; int found = 0;
    for (int pass = 0; pass < 2 && !found; pass++)
        for (i = ia; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            if (!pass && strncmp(i->ifa_name, "wlan", 4)) continue;
            *ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr); *mask = i->ifa_netmask ? ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0xffffff00u;
            found = 1; break;
        }
    freeifaddrs(ia); return found;
}
static void mac_of(uint32_t ip, uint8_t m[6]) { m[0] = 0x02; m[1] = 0x4b; m[2] = ip >> 24; m[3] = ip >> 16; m[4] = ip >> 8; m[5] = ip; }   // a locally administered MAC from the address
static void node_set(uint8_t *ni, int k, uint32_t ip, const char *name, int16_t ver) {
    uint8_t *n = ni + NODES + k * NODE; memset(n, 0, NODE);
    memcpy(n, &ip, 4); mac_of(ip, n + 4); n[0xa] = (uint8_t)k; n[0xb] = 1; strncpy((char *)n + 0xc, name, 0x20); memcpy(n + 0x2e, &ver, 2);
}
static int node_count(const uint8_t *ni) { int c = 0; for (int k = 0; k < MAXN; k++) if (ni[NODES + k * NODE + 0xb]) c++; return c; }

// ---------- the host side ----------
static int send_all(int fd, const void *b, size_t n) { const uint8_t *p = b; while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return 0; p += w; n -= (size_t)w; } return 1; }
static int recv_all(int fd, void *b, size_t n) { uint8_t *p = b; while (n) { ssize_t r = recv(fd, p, n, 0); if (r <= 0) return 0; p += r; n -= (size_t)r; } return 1; }
static void members_update_locked(void) {   // the node list to every member, the host's own event
    g_ni[LDN + 0x17] = (uint8_t)node_count(g_ni);
    if (g_relay) { uint8_t m[1 + NI_SIZE]; m[0] = 'N'; memcpy(m + 1, g_ni, NI_SIZE);
        for (int k = 1; k < MAXN; k++) if (g_maddr[k]) vsend(g_join, m, sizeof m, g_maddr[k], g_mport[k]);
        ev(); return; }
    for (int k = 1; k < MAXN; k++) if (g_member[k] >= 0) { uint8_t t = 'N'; if (!send_all(g_member[k], &t, 1) || !send_all(g_member[k], g_ni, NI_SIZE)) { close(g_member[k]); g_member[k] = -1; memset(g_ni + NODES + k * NODE, 0, NODE); } }
    g_ni[LDN + 0x17] = (uint8_t)node_count(g_ni); ev();
}
static void host_accept_locked(int fd) {   // a station's join: 'J', UserName[0x21], LocalCommunicationVersion s16, PassphraseSize u16, Passphrase[0x40]
    uint8_t j[1 + 0x21 + 2 + 2 + 0x40]; struct timeval tv = { 2, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (!recv_all(fd, j, sizeof j) || j[0] != 'J') { close(fd); return; }
    uint16_t pl; memcpy(&pl, j + 0x24, 2); uint16_t hl; memcpy(&hl, g_sec + 2, 2);
    int ok = pl == hl && !memcmp(j + 0x26, g_sec + 4, pl < 0x40 ? pl : 0x40) && !g_ni[LDN + 0x12];   // the game's passphrase; StationAcceptPolicy 0 = accept all
    int k = 1; while (ok && k < MAXN && (k < g_ni[LDN + 0x16] ? g_ni[NODES + k * NODE + 0xb] : 1)) k++;
    if (!ok || k >= MAXN || k >= g_ni[LDN + 0x16]) { uint8_t r = 'R'; send_all(fd, &r, 1); close(fd); LOG("[ldn] a station was refused (%s)", ok ? "the network is full" : "passphrase or accept policy"); return; }
    struct sockaddr_in a; socklen_t al = sizeof a; getpeername(fd, (struct sockaddr *)&a, &al);
    char name[0x21]; memcpy(name, j + 1, 0x20); name[0x20] = 0; int16_t ver; memcpy(&ver, j + 0x22, 2);
    node_set(g_ni, k, ntohl(a.sin_addr.s_addr), name, ver); g_member[k] = fd;
    tv.tv_sec = 0; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t acc[2] = { 'A', (uint8_t)k }; send_all(fd, acc, 2);
    LOG("[ldn] %s joined as node %d (%s)", name, k, inet_ntoa(a.sin_addr));
    members_update_locked();
}
static void *host_main(void *p) {
    (void)p; struct sockaddr_in b = { .sin_family = AF_INET, .sin_port = htons(PORT_BEACON), .sin_addr.s_addr = htonl((g_ip & g_mask) | ~g_mask) };
    uint8_t pkt[8 + 8 + NI_SIZE]; uint64_t prog = my_program(); struct timespec last = {0};
    for (;;) {
        pthread_mutex_lock(&g_mu); if (g_state != ST_AP_CREATED) { pthread_mutex_unlock(&g_mu); break; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= 500) {   // the announcement
            memcpy(pkt, MAGIC, 8); memcpy(pkt + 8, &prog, 8); memcpy(pkt + 16, g_ni, NI_SIZE);
            sendto(g_beacon, pkt, sizeof pkt, 0, (struct sockaddr *)&b, sizeof b); last = now; }
        struct pollfd pf[MAXN + 1]; int n = 0, who[MAXN + 1]; pf[n].fd = g_listen; pf[n].events = POLLIN; who[n++] = 0;
        for (int k = 1; k < MAXN; k++) if (g_member[k] >= 0) { pf[n].fd = g_member[k]; pf[n].events = POLLIN; who[n++] = k; }
        pthread_mutex_unlock(&g_mu);
        if (poll(pf, (nfds_t)n, 100) <= 0) continue;
        pthread_mutex_lock(&g_mu);
        if (g_state == ST_AP_CREATED) {
            if (pf[0].revents & POLLIN) { int fd = accept(g_listen, NULL, NULL); if (fd >= 0) host_accept_locked(fd); }
            for (int i = 1; i < n; i++) if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {   // a member: anything it sends (or its closing) means it left
                char c; if (recv(pf[i].fd, &c, 1, MSG_DONTWAIT) <= 0 || c == 'D') { int k = who[i]; close(g_member[k]); g_member[k] = -1; memset(g_ni + NODES + k * NODE, 0, NODE);
                    LOG("[ldn] node %d left", k); members_update_locked(); } }
        }
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static void host_join_relay_locked(const uint8_t *j, size_t n, const struct sockaddr_in *from) {   // a station's 'J' from the virtual network
    uint32_t ip = ntohl(from->sin_addr.s_addr); uint16_t port = ntohs(from->sin_port);
    for (int k = 1; k < MAXN; k++) if (g_maddr[k] == ip && g_mport[k] == port) { uint8_t a[2] = { 'A', (uint8_t)k }; vsend(g_join, a, 2, ip, port); members_update_locked(); return; }   // (a repeat)
    if (n < 1 + 0x21 + 2 + 2 + 0x40) return;
    uint16_t pl; memcpy(&pl, j + 0x24, 2); uint16_t hl; memcpy(&hl, g_sec + 2, 2);
    int ok = pl == hl && !memcmp(j + 0x26, g_sec + 4, pl < 0x40 ? pl : 0x40) && !g_ni[LDN + 0x12];
    int k = 1; while (k < g_ni[LDN + 0x16] && k < MAXN && g_ni[NODES + k * NODE + 0xb]) k++;
    if (!ok || k >= MAXN || k >= g_ni[LDN + 0x16]) { uint8_t r = 'R'; vsend(g_join, &r, 1, ip, port); LOG("[ldn] a station was refused (%s)", ok ? "the network is full" : "passphrase or accept policy"); return; }
    char name[0x21]; memcpy(name, j + 1, 0x20); name[0x20] = 0; int16_t ver; memcpy(&ver, j + 0x22, 2);
    node_set(g_ni, k, ip, name, ver); g_maddr[k] = ip; g_mport[k] = port;
    uint8_t a[2] = { 'A', (uint8_t)k }; vsend(g_join, a, 2, ip, port);
    struct in_addr ia = { htonl(ip) }; LOG("[ldn] %s joined as node %d (room, %s)", name, k, inet_ntoa(ia));
    members_update_locked();
}
static void *host_relay_main(void *p) {
    (void)p; uint8_t pkt[8 + 8 + NI_SIZE]; uint64_t prog = my_program(); struct timespec last = {0};
    for (;;) {
        pthread_mutex_lock(&g_mu); if (g_state != ST_AP_CREATED || !g_relay) { pthread_mutex_unlock(&g_mu); break; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= 500) {
            memcpy(pkt, MAGIC, 8); memcpy(pkt + 8, &prog, 8); memcpy(pkt + 16, g_ni, NI_SIZE); vsend(g_beacon, pkt, sizeof pkt, (g_ip & g_mask) | ~g_mask, PORT_BEACON); last = now; }
        int fd = g_join; pthread_mutex_unlock(&g_mu);
        struct pollfd pf = { fd, POLLIN, 0 }; if (poll(&pf, 1, 100) <= 0) continue;
        uint8_t m[1 + 0x21 + 2 + 2 + 0x40 + 8]; struct sockaddr_in from; long r = relay_recvfrom(fd, m, sizeof m, MSG_DONTWAIT, &from); if (r <= 0) continue;
        pthread_mutex_lock(&g_mu);
        if (g_state == ST_AP_CREATED) {
            if (m[0] == 'J') host_join_relay_locked(m, (size_t)r, &from);
            else if (m[0] == 'D') { uint32_t ip = ntohl(from.sin_addr.s_addr);
                for (int k = 1; k < MAXN; k++) if (g_maddr[k] == ip) { g_maddr[k] = 0; memset(g_ni + NODES + k * NODE, 0, NODE); LOG("[ldn] node %d left", k); members_update_locked(); } }
        }
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void *station_relay_main(void *p) {   // the host's node lists over the virtual network
    (void)p;
    for (;;) {
        int fd; pthread_mutex_lock(&g_mu); fd = g_join; int st = g_state; pthread_mutex_unlock(&g_mu); if (fd < 0 || st != ST_CONNECTED) break;
        struct pollfd pf = { fd, POLLIN, 0 }; if (poll(&pf, 1, 200) <= 0) continue;
        uint8_t m[1 + NI_SIZE]; struct sockaddr_in from; long r = relay_recvfrom(fd, m, sizeof m, MSG_DONTWAIT, &from);
        if (r == (long)sizeof m && m[0] == 'N') { pthread_mutex_lock(&g_mu); if (g_state == ST_CONNECTED) { memcpy(g_ni, m + 1, NI_SIZE); ev(); } pthread_mutex_unlock(&g_mu); }
    }
    return NULL;
}
// a member left the room (hle_relay.c; 0: the whole network is gone)
static void peer_left(uint32_t ip) {
    pthread_mutex_lock(&g_mu);
    if (g_state == ST_AP_CREATED) { for (int k = 1; k < MAXN; k++) if (g_maddr[k] && (!ip || g_maddr[k] == ip)) { g_maddr[k] = 0; memset(g_ni + NODES + k * NODE, 0, NODE); LOG("[ldn] node %d left (room)", k); members_update_locked(); } }
    else if (g_state == ST_CONNECTED && (!ip || ip == ntohl(g_hostaddr.sin_addr.s_addr))) { g_state = ST_STATION; g_reason = 6; g_node = -1; if (g_join >= 0) { close(g_join); g_join = -1; } LOG("[ldn] the host left the room (signal lost)"); ev(); }
    pthread_mutex_unlock(&g_mu);
}

// ---------- the station side ----------
static void *station_main(void *p) {   // the host's node list as it changes; the host gone = signal lost
    (void)p;
    for (;;) {
        uint8_t t; int fd; pthread_mutex_lock(&g_mu); fd = g_host; pthread_mutex_unlock(&g_mu); if (fd < 0) break;
        if (!recv_all(fd, &t, 1) || t != 'N') goto lost;
        uint8_t ni[NI_SIZE]; if (!recv_all(fd, ni, NI_SIZE)) goto lost;
        pthread_mutex_lock(&g_mu); if (g_state == ST_CONNECTED) { memcpy(g_ni, ni, NI_SIZE); ev(); } pthread_mutex_unlock(&g_mu);
        continue;
    lost:
        pthread_mutex_lock(&g_mu);
        if (g_state == ST_CONNECTED && g_host == fd) { close(g_host); g_host = -1; g_state = ST_STATION; g_reason = 6; g_node = -1; LOG("[ldn] the host is gone (signal lost)"); ev(); }
        pthread_mutex_unlock(&g_mu); break;
    }
    return NULL;
}

static void teardown_locked(void) {
    if (g_relay && g_join >= 0) { if (g_state == ST_CONNECTED) { uint8_t d = 'D'; relay_sendto(g_join, &d, 1, &g_hostaddr); } close(g_join); g_join = -1; }
    for (int k = 0; k < MAXN; k++) g_maddr[k] = 0;
    if (g_beacon >= 0) { close(g_beacon); g_beacon = -1; } if (g_listen >= 0) { close(g_listen); g_listen = -1; }
    for (int k = 0; k < MAXN; k++) if (g_member[k] >= 0) { close(g_member[k]); g_member[k] = -1; }
    if (g_host >= 0) { uint8_t d = 'D'; send_all(g_host, &d, 1); close(g_host); g_host = -1; }
    g_node = -1;
}
static void start_thread(void *(*fn)(void *)) { pthread_t t; if (!pthread_create(&t, NULL, fn, NULL)) pthread_detach(t); }

// ---------- the API ----------
HLE_FORCE("_ZN2nn3ldn10InitializeEv", "local wireless over the phone's Wi-Fi (off while mods are on)", Result, h_ldn_Init, void) {
    pthread_mutex_lock(&g_mu); Result r = RES_OK;
    const char *off = getenv("DM_LDN");
    if (off && !strcmp(off, "0")) { LOG("[ldn] local play is off (DM_LDN=0)"); r = RES_FAIL; }
    else if (mods_on()) { LOG("[RED] [ldn] local play is off while mods are on: switch them off in the menu, Mods (play with others only on the unmodified game)"); r = RES_FAIL; }
    else if ((g_relay = getenv("DM_RELAY_ROOM") && *getenv("DM_RELAY_ROOM")) && !relay_start()) { LOG("[RED] [ldn] the online room could not be joined (see [relay] above): local play is off"); r = RES_FAIL; }
    else if (g_relay) { g_ip = relay_vip(); g_mask = 0xFFFF0000u; relay_on_left(peer_left); for (int k = 0; k < MAXN; k++) g_member[k] = -1; g_state = ST_INIT; g_reason = 0;
        struct in_addr a = { htonl(g_ip) }; LOG("[ldn] local play over the online room, this phone %s", inet_ntoa(a)); }
    else if (!wifi_addr(&g_ip, &g_mask)) { LOG("[ldn] no Wi-Fi address: local play needs the phone on a Wi-Fi network"); r = RES_FAIL; }
    else { static int seeded; if (!seeded++) srand((unsigned)time(NULL) ^ g_ip ^ (unsigned)getpid());
           for (int k = 0; k < MAXN; k++) g_member[k] = -1; g_state = ST_INIT; g_reason = 0; struct in_addr a = { htonl(g_ip) }; LOG("[ldn] local play over Wi-Fi, this phone %s", inet_ntoa(a)); }
    pthread_mutex_unlock(&g_mu); return r;
}
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "local wireless: everything closed", void, h_ldn_Fini, void) { pthread_mutex_lock(&g_mu); teardown_locked(); g_state = ST_NONE; pthread_mutex_unlock(&g_mu); }
HLE_FORCE("_ZN2nn3ldn8GetStateEv", "local wireless state", int, h_ldn_State, void) { return g_state; }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "local wireless state event: fires on every change", void, h_ldn_Attach, void *e) { hle_bind_system_event(e, 0); g_ev = e; }
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "local wireless: why the last network ended", int, h_ldn_DiscReason, void) { return g_reason; }

HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "local wireless: host mode", Result, h_ldn_OpenAp, void) { if (g_state != ST_INIT) return RES_FAIL; g_state = ST_AP; ev(); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "local wireless: host mode left", Result, h_ldn_CloseAp, void) {
    pthread_mutex_lock(&g_mu); teardown_locked(); g_state = ST_INIT; ev(); pthread_mutex_unlock(&g_mu); return RES_OK; }
static Result create(const uint8_t *nc, const uint8_t *sc, const uint8_t *uc) {
    pthread_mutex_lock(&g_mu);
    if (g_state != ST_AP) { pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    memset(g_ni, 0, NI_SIZE); memcpy(g_ni, nc, 0x10);                                            // IntentId
    for (int i = 0; i < 16; i++) { g_ni[0x10 + i] = (uint8_t)rand(); g_ni[LDN + i] = (uint8_t)rand(); }   // SessionId, ServerRandom
    mac_of(g_ip, g_ni + 0x20);                                                                   // Bssid
    char ssid[33]; snprintf(ssid, sizeof ssid, "pkldn%08x%08x", (unsigned)rand(), (unsigned)rand()); g_ni[0x26] = (uint8_t)strlen(ssid); memcpy(g_ni + 0x27, ssid, strlen(ssid));
    int16_t ch; memcpy(&ch, nc + 0x10, 2); if (ch <= 0) ch = 6; memcpy(g_ni + 0x48, &ch, 2); g_ni[0x4a] = 3; g_ni[0x4b] = 2;   // channel, link level, LDN network
    memcpy(g_ni + LDN + 0x10, sc, 2); g_ni[LDN + 0x12] = 0; g_ni[LDN + 0x13] = 3;                 // security mode, accept all, version
    g_ni[LDN + 0x16] = nc[0x12] && nc[0x12] <= MAXN ? nc[0x12] : MAXN;                          // node count max
    int16_t ver; memcpy(&ver, nc + 0x14, 2); node_set(g_ni, 0, g_ip, (const char *)uc, ver); g_ni[LDN + 0x17] = 1;
    memcpy(g_sec, sc, sizeof g_sec);
    if (g_relay) {   // in a room: the announcement and the joins over the virtual network
        g_beacon = socket(AF_INET, SOCK_DGRAM, 0); g_join = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ja = { .sin_family = AF_INET, .sin_port = htons(PORT_JOIN), .sin_addr.s_addr = htonl(g_ip) };
        if (g_beacon < 0 || g_join < 0 || relay_bind(g_join, &ja)) { LOG("[RED] [ldn] cannot host in the room: %s", strerror(errno)); teardown_locked(); pthread_mutex_unlock(&g_mu); return RES_FAIL; }
        g_state = ST_AP_CREATED; g_node = 0; start_thread(host_relay_main); ev();
        LOG("[ldn] hosting in the online room: up to %d players", g_ni[LDN + 0x16]);
        pthread_mutex_unlock(&g_mu); return RES_OK;
    }
    int one = 1; g_beacon = socket(AF_INET, SOCK_DGRAM, 0); setsockopt(g_beacon, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    g_listen = socket(AF_INET, SOCK_STREAM, 0); setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT_JOIN), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (g_beacon < 0 || g_listen < 0 || bind(g_listen, (struct sockaddr *)&a, sizeof a) || listen(g_listen, 8)) {
        LOG("[RED] [ldn] cannot host: %s (port %d)", strerror(errno), PORT_JOIN); teardown_locked(); pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    g_state = ST_AP_CREATED; g_node = 0; start_thread(host_main); ev();
    LOG("[ldn] hosting: up to %d players, announced on port %d", g_ni[LDN + 0x16], PORT_BEACON);
    pthread_mutex_unlock(&g_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "local wireless: host a network over Wi-Fi", Result, h_ldn_Create,
          const uint8_t *nc, const uint8_t *sc, const uint8_t *uc) { return create(nc, sc, uc); }
HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "local wireless: host a network over Wi-Fi",
          Result, h_ldn_CreateP, const uint8_t *nc, const uint8_t *sc, const void *sp, const uint8_t *uc, int n, const void *e) { (void)sp; (void)n; (void)e; return create(nc, sc, uc); }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "local wireless: the hosted network ends", Result, h_ldn_Destroy, void) {
    pthread_mutex_lock(&g_mu); if (g_state != ST_AP_CREATED) { pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    teardown_locked(); g_state = ST_AP; g_reason = 3; ev(); pthread_mutex_unlock(&g_mu); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "local wireless: the host's advertise data, sent with its announcements", Result, h_ldn_Adv, const void *d, uint64_t n) {
    if (n > 0x180) return RES_FAIL; pthread_mutex_lock(&g_mu); uint16_t s = (uint16_t)n; memcpy(g_ni + LDN + 0x21a, &s, 2); memset(g_ni + LDN + 0x21c, 0, 0x180); if (n) memcpy(g_ni + LDN + 0x21c, d, n);
    if (g_state == ST_AP_CREATED) members_update_locked(); pthread_mutex_unlock(&g_mu); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "local wireless: who may join", Result, h_ldn_Policy, int p) { g_ni[LDN + 0x12] = (uint8_t)p; return RES_OK; }
HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "local wireless: accept filter (all accepted)", Result, h_ldn_Filter, uint64_t mac) { (void)mac; return RES_OK; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "local wireless: a member sent away", Result, h_ldn_Reject, uint32_t ip) {
    pthread_mutex_lock(&g_mu);
    for (int k = 1; k < MAXN; k++) if (g_member[k] >= 0 && rd32(g_ni + NODES + k * NODE) == ip) { close(g_member[k]); g_member[k] = -1; memset(g_ni + NODES + k * NODE, 0, NODE); members_update_locked(); }
    pthread_mutex_unlock(&g_mu); return RES_OK; }

HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "local wireless: join mode", Result, h_ldn_OpenSta, void) { if (g_state != ST_INIT) return RES_FAIL; g_state = ST_STATION; ev(); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "local wireless: join mode left", Result, h_ldn_CloseSta, void) {
    pthread_mutex_lock(&g_mu); teardown_locked(); g_state = ST_INIT; ev(); pthread_mutex_unlock(&g_mu); return RES_OK; }
// ScanFilter 0x60: NetworkId 0x20, NetworkType +0x20, Bssid +0x24, Ssid +0x2a, Flag +0x5c (1 LocalCommunicationId, 2 SessionId, 4 NetworkType, 8 Bssid, 16 Ssid, 32 SceneId, 64 IntentId, 128 NetworkId)
static int passes(const uint8_t *ni, const uint8_t *f) {
    uint32_t fl = rd32(f + 0x5c);
    if ((fl & (1 | 64 | 128)) && memcmp(ni, f, 8)) return 0;
    if ((fl & (32 | 64 | 128)) && memcmp(ni + 0xa, f + 0xa, 2)) return 0;
    if ((fl & (2 | 128)) && memcmp(ni + 0x10, f + 0x10, 16)) return 0;
    if ((fl & 16) && memcmp(ni + 0x26, f + 0x2a, 0x22)) return 0;
    return 1;
}
static Result scan(uint8_t *out, int *count, int max, const uint8_t *filter) {
    if (count) *count = 0; if (g_state != ST_STATION && g_state != ST_AP) return RES_FAIL;
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1; if (s < 0) return RES_FAIL;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT_BEACON), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (g_relay) { struct sockaddr_in va = { .sin_family = AF_INET, .sin_port = htons(PORT_BEACON), .sin_addr.s_addr = htonl(g_ip) }; if (relay_bind(s, &va)) { close(s); return RES_OK; } }
    else if (bind(s, (struct sockaddr *)&a, sizeof a)) { close(s); LOG("[ldn] scan: cannot listen on port %d (%s)", PORT_BEACON, strerror(errno)); return RES_OK; }
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0); int n = 0; uint64_t prog = my_program();
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t); int left = 1000 - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000); if (left <= 0 || n >= max) break;
        struct pollfd pf = { s, POLLIN, 0 }; if (poll(&pf, 1, left) <= 0) break;
        uint8_t pkt[8 + 8 + NI_SIZE]; ssize_t r = recv(s, pkt, sizeof pkt, 0); if (r != (ssize_t)sizeof pkt || memcmp(pkt, MAGIC, 8)) continue;
        uint64_t p; memcpy(&p, pkt + 8, 8); const uint8_t *ni = pkt + 16;
        if (!family(p, prog) || rd32(ni + NODES) == g_ip || (filter && !passes(ni, filter))) continue;   // another game, this phone itself, or the game's filter
        int dup = 0; for (int i = 0; i < n; i++) if (!memcmp(out + i * NI_SIZE + 0x10, ni + 0x10, 16)) dup = 1;
        if (!dup) memcpy(out + (n++) * NI_SIZE, ni, NI_SIZE);
    }
    close(s); if (count) *count = n;
    if (n) LOG("[ldn] scan: %d network%s found", n, n == 1 ? "" : "s");
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan over Wi-Fi (1 s)", Result, h_ldn_Scan, uint8_t *out, int *n, int max, const uint8_t *f, int ch) { (void)ch; return scan(out, n, max, f); }
HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan over Wi-Fi (1 s)", Result, h_ldn_ScanP, uint8_t *out, int *n, int max, const uint8_t *f, int ch) { (void)ch; return scan(out, n, max, f); }
static Result join(const uint8_t *ni, const uint8_t *sc, const uint8_t *uc, int ver) {
    pthread_mutex_lock(&g_mu);
    if (g_state != ST_STATION) { pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    uint32_t hip = rd32(ni + NODES); struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT_JOIN), .sin_addr.s_addr = htonl(hip) };
    if (g_relay) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0); uint8_t j[1 + 0x21 + 2 + 2 + 0x40] = { 'J' }; memcpy(j + 1, uc, 0x21); int16_t v = (int16_t)ver; memcpy(j + 0x22, &v, 2); memcpy(j + 0x24, sc + 2, 2); memcpy(j + 0x26, sc + 4, 0x40);
        int node = -1;
        for (int tries = 0; fd >= 0 && tries < 6 && node < 0; tries++) {   // (the answer comes back on this socket's port)
            relay_sendto(fd, j, sizeof j, &a);
            struct pollfd pf = { fd, POLLIN, 0 }; pthread_mutex_unlock(&g_mu); int pr = poll(&pf, 1, 500); pthread_mutex_lock(&g_mu); if (pr <= 0) continue;
            uint8_t m[1 + NI_SIZE]; struct sockaddr_in from; long r = relay_recvfrom(fd, m, sizeof m, MSG_DONTWAIT, &from);
            if (r >= 1 && m[0] == 'R') break;
            if (r >= 2 && m[0] == 'A') node = m[1];
        }
        if (node < 0) { LOG("[ldn] could not join %s in the room", inet_ntoa(a.sin_addr)); if (fd >= 0) close(fd); pthread_mutex_unlock(&g_mu); return RES_FAIL; }
        memcpy(g_ni, ni, NI_SIZE); g_join = fd; g_hostaddr = a; g_node = node; g_state = ST_CONNECTED; g_reason = 0; start_thread(station_relay_main); ev();
        LOG("[ldn] joined %s in the room as node %d", inet_ntoa(a.sin_addr), g_node);
        pthread_mutex_unlock(&g_mu); return RES_OK;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0); struct timeval tv = { 3, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    uint8_t j[1 + 0x21 + 2 + 2 + 0x40] = { 'J' }; memcpy(j + 1, uc, 0x21); int16_t v = (int16_t)ver; memcpy(j + 0x22, &v, 2); memcpy(j + 0x24, sc + 2, 2); memcpy(j + 0x26, sc + 4, 0x40);
    uint8_t acc[2] = { 0, 0 };
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) || !send_all(fd, j, sizeof j) || !recv_all(fd, acc, 2) || acc[0] != 'A') {
        LOG("[ldn] could not join %s (%s)", inet_ntoa(a.sin_addr), fd >= 0 && acc[0] == 'R' ? "refused" : strerror(errno)); if (fd >= 0) close(fd);
        pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    tv.tv_sec = 0; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t t; if (!recv_all(fd, &t, 1) || t != 'N' || !recv_all(fd, g_ni, NI_SIZE)) { close(fd); pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    g_host = fd; g_node = acc[1]; g_state = ST_CONNECTED; g_reason = 0; start_thread(station_main); ev();
    LOG("[ldn] joined %s as node %d", inet_ntoa(a.sin_addr), g_node);
    pthread_mutex_unlock(&g_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless: join a network over Wi-Fi", Result, h_ldn_Connect,
          const uint8_t *ni, const uint8_t *sc, const uint8_t *uc, int v, int o) { (void)o; return join(ni, sc, uc, v); }
HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless: join (private: unsupported)",
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) { (void)a; (void)b; (void)c; (void)d; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "local wireless: leave the network", Result, h_ldn_Disc, void) {
    pthread_mutex_lock(&g_mu); if (g_state != ST_CONNECTED) { pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    teardown_locked(); g_state = ST_STATION; g_reason = 1; ev(); pthread_mutex_unlock(&g_mu); return RES_OK; }

HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "local wireless: the network as it is now", Result, h_ldn_NetInfo, uint8_t *o) {
    pthread_mutex_lock(&g_mu); int ok = g_state == ST_AP_CREATED || g_state == ST_CONNECTED; if (ok) memcpy(o, g_ni, NI_SIZE); pthread_mutex_unlock(&g_mu); return ok ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "local wireless: this phone's Wi-Fi address", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) {
    if (g_state == ST_NONE) return RES_FAIL; if (a) *a = g_ip; if (m) *m = g_mask; return RES_OK; }
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "local wireless: the network's security parameter", Result, h_ldn_SecPrm, uint8_t *o) {
    if (g_state != ST_AP_CREATED && g_state != ST_CONNECTED) return RES_FAIL; memcpy(o, g_ni + LDN, 16); memcpy(o + 16, g_ni + 0x10, 16); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", "local wireless: the network's config", Result, h_ldn_NetCfg, uint8_t *o) {
    if (g_state != ST_AP_CREATED && g_state != ST_CONNECTED) return RES_FAIL;
    memset(o, 0, 0x20); memcpy(o, g_ni, 0x10); memcpy(o + 0x10, g_ni + 0x48, 2); o[0x12] = g_ni[LDN + 0x16]; memcpy(o + 0x14, g_ni + NODES + 0x2e, 2); return RES_OK; }
