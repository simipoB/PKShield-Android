// The first line of a crash record and the reads the crash handler makes (crash.c), kept apart so they can be tested on their own (tests/t_crash_line.c).
// Everything here works in a signal handler that may have been entered with a broken process: no stdio, no dladdr (the linker's lock), no allocation, no lock, and
// no read that can fault. A second fault in the handler, with the signal blocked, ends the process at once and the record is lost (Android then reports "signaled,
// status 11"): #89's starts left only the "crashed" note, #82's record stopped after the registers (the frame chain walked through a GPU driver's x29)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach/mach.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

// A read that fails instead of faulting (vm_read_overwrite: process_vm_readv on Android)
static int rd_safe(uint64_t a, void *out, size_t n) {
    vm_size_t got = 0;
    return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}
// The frame pointer chain: [fp] = the previous frame's fp, [fp + 8] = the return address; 8-byte aligned, increasing frames only, each read without faulting
// (code built without frame pointers, a GPU driver, leaves anything in x29). The return addresses go to ra[]; returns how many
static int frame_walk(uint64_t fp, uint64_t *ra, int max) {
    int n = 0;
    while (n < max && fp && !(fp & 7)) {
        uint64_t w[2]; if (!rd_safe(fp, w, sizeof w)) break;
        ra[n++] = w[1];
        if (w[0] <= fp) break;
        fp = w[0];
    }
    return n;
}

// Text into a fixed buffer, p up to e (e stays free for nothing: callers keep one byte back)
static char *put_s(char *p, char *e, const char *s) { while (s && *s && p < e) *p++ = *s++; return p; }
static char *put_x(char *p, char *e, uint64_t v) {
    char t[16]; int n = 0;
    do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    p = put_s(p, e, "0x"); while (n && p < e) *p++ = t[--n];
    return p;
}
static char *put_d(char *p, char *e, uint64_t v) { char t[20]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n && p < e) *p++ = t[--n]; return p; }
static uint64_t hex_in(const char **s) {
    uint64_t v = 0;
    for (;; (*s)++) { char c = **s; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) return v; v = v << 4 | (unsigned)d; }
}

#if defined(__linux__)
// The mapping that holds an address, from /proc/self/maps (open / read only): "<file>+<offset>", "(anonymous)+<offset into the mapping>" for one without a file.
// A GPU driver's code is named this way without dladdr. The offset is from the file's first mapping (file offset 0), its load base, as dladdr gives it (dli_fbase), so
// it reads the same as the record's later lines and older logs (#82: libGLES_mali.so+0x8a77e0); the offset into the file when that mapping isn't found.
// Returns 0 when no mapping holds the address
static int maps_where(char **pp, char *e, uint64_t a) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char rb[512], ln[512], bpath[256] = ""; size_t n = 0; int over = 0, found = 0; ssize_t r; uint64_t bstart = 0;
    while (!found && (r = read(fd, rb, sizeof rb)) > 0)
        for (ssize_t i = 0; i < r && !found; i++) {
            if (rb[i] != '\n') { if (n < sizeof ln - 1) ln[n++] = rb[i]; else over = 1; continue; }
            ln[n] = 0; n = 0;
            if (over) { over = 0; continue; }   // (a line longer than the buffer: left out)
            // "start-end perms offset dev inode   path"
            const char *s = ln; uint64_t lo = hex_in(&s), hi = 0;
            if (*s == '-') { s++; hi = hex_in(&s); }
            while (*s == ' ') s++;
            while (*s && *s != ' ') s++;   // (the permissions)
            while (*s == ' ') s++;
            uint64_t fo = hex_in(&s);
            const char *path = s; while (*path && *path != '/' && *path != '[') path++;
            if (fo == 0 && *path == '/') {   // a file's first mapping: its load base, for the file's later mappings
                size_t k = 0; while (path[k] && k < sizeof bpath - 1) { bpath[k] = path[k]; k++; } bpath[k] = 0; bstart = lo; }
            if (a < lo || a >= hi) continue;
            if (!*path) { *pp = put_s(*pp, e, "(anonymous)+"); *pp = put_x(*pp, e, a - lo); }
            else {
                const char *b = path; for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
                size_t k = 0; while (path[k] && path[k] == bpath[k]) k++;
                uint64_t o = *path == '/' && !path[k] && !bpath[k] ? a - bstart : a - lo + fo;
                *pp = put_s(*pp, e, b); *pp = put_s(*pp, e, "+"); *pp = put_x(*pp, e, o);
            }
            found = 1;
        }
    close(fd);
    return found;
}
#endif

// " tag=<where>": the game's module and offset (module_of: a table lookup), else the mapping's file and offset, else the raw address
static char *put_where(char *p, char *e, const char *tag, uint64_t a) {
    p = put_s(p, e, " "); p = put_s(p, e, tag); p = put_s(p, e, "=");
    module_t *m = module_of((void *)(uintptr_t)a);
    if (m) { p = put_s(p, e, m->name); p = put_s(p, e, "+"); return put_x(p, e, a - (uint64_t)(uintptr_t)m->base); }
#if defined(__linux__)
    if (maps_where(&p, e, a)) return p;
#endif
    return put_x(p, e, a);
}

// The whole first line into b (n bytes), with its newline; returns its length. Then one write()
static size_t crash_line_format(char *b, size_t n, int sig, uint64_t addr, uint64_t pc, uint64_t lr, uint64_t sp) {
    char *p = b, *e = b + n - 1;
    p = put_s(p, e, "[dm] CRASH sig="); p = put_d(p, e, (uint64_t)(sig < 0 ? 0 : sig));
    p = put_s(p, e, " addr="); p = put_x(p, e, addr);
    p = put_where(p, e, "pc", pc); p = put_where(p, e, "lr", lr);
    p = put_s(p, e, " sp="); p = put_x(p, e, sp);
#if defined(__linux__)
    { char nm[17] = {0}; if (!prctl(PR_GET_NAME, nm, 0, 0, 0) && nm[0]) { p = put_s(p, e, " thread \""); p = put_s(p, e, nm); p = put_s(p, e, "\""); } }
#endif
    p = put_s(p, e, " (first line; the full record follows)");
    *p++ = '\n';   // (e = b + n - 1 kept this byte free)
    return (size_t)(p - b);
}
