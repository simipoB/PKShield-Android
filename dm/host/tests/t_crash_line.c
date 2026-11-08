// Crash record first-line unit test (crash_line.h): reads that fail instead of faulting, the frame chain walk stopping at a bad frame instead of faulting, the mapping
// lookup from /proc/self/maps (offsets from the file's load base, as dladdr gives them), the whole line, and the line made inside a real SIGSEGV handler that then walks a
// garbage frame chain and still exits by itself (a second fault would kill the child with SIGSEGV instead). Expected values are hand-written, or come from dladdr.
// Linux (WSL) / Android only (the maps lookup reads /proc): cc -I../../android/compat -I.. <game profile includes> t_crash_line.c -ldl
#define _GNU_SOURCE   // (process_vm_readv, dladdr)
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <dlfcn.h>
#include "../dm.h"
#include "../crash_line.h"

kern_return_t vm_read_overwrite(task_t t, vm_address_t a, vm_size_t n, vm_address_t to, vm_size_t *got) {
    (void)t; struct iovec l = { (void *)to, n }, r = { (void *)a, n }; ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (k < 0) return 1;
    *got = (vm_size_t)k; return KERN_SUCCESS;
}
static uint8_t fake_img[0x1000];   // the game's "main" module, for module_of
static module_t fake;
module_t *module_of(const void *p) { return (const uint8_t *)p >= fake_img && (const uint8_t *)p < fake_img + sizeof fake_img ? &fake : NULL; }

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
static int starts(const char *s, const char *pre) { return !strncmp(s, pre, strlen(pre)); }
static char out[1024];
static const char *where(uint64_t a) { char *p = out, *e = out + sizeof out - 1; if (!maps_where(&p, e, a)) return NULL; *p = 0; return out; }
static const char *fmt_x(uint64_t v) { char *p = put_x(out, out + sizeof out - 1, v); *p = 0; return out; }
static const char *fmt_d(uint64_t v) { char *p = put_d(out, out + sizeof out - 1, v); *p = 0; return out; }

// The crash child: the handler makes the line, writes it to the pipe, walks a garbage frame chain (unaligned, then unmapped), and exits 139 by itself
static int g_pipe = -1; static uint64_t g_unmapped;
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_; uint64_t pc, lr = 0, sp;
#if defined(__x86_64__)
    pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP]; sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
    pc = uc->uc_mcontext.pc; lr = uc->uc_mcontext.regs[30]; sp = uc->uc_mcontext.sp;
#endif
    char b[768]; size_t n = crash_line_format(b, sizeof b, sig, (uint64_t)(uintptr_t)si->si_addr, pc, lr, sp);
    if (write(g_pipe, b, n) < 0) {}
    uint64_t ra[8]; int k = frame_walk(0x1235, ra, 8) + frame_walk(g_unmapped, ra, 8) + frame_walk(0x10, ra, 8);
    static const char ok[] = "walk ok 0\n"; if (!k && write(g_pipe, ok, sizeof ok - 1) < 0) {}
    _exit(139);
}
__attribute__((noinline)) static void crash_here(void) { *(volatile int *)(uintptr_t)0x90 = 1; }

int main(void) {
    prctl(PR_SET_NAME, "t_crash_line", 0, 0, 0);
    strcpy(fake.name, "main"); fake.base = fake_img;
    long pg = sysconf(_SC_PAGESIZE);

    // numbers into the buffer
    CHECK(!strcmp(fmt_x(0), "0x0"), "put_x 0 → 0x0");
    CHECK(!strcmp(fmt_x(0xdeadbeef), "0xdeadbeef"), "put_x 0xdeadbeef");
    CHECK(!strcmp(fmt_x(0xffffffffffffffffull), "0xffffffffffffffff"), "put_x all ones: 16 digits");
    CHECK(!strcmp(fmt_d(11), "11"), "put_d 11");
    CHECK(!strcmp(fmt_d(0), "0"), "put_d 0");
    { char b[8] = "zzzzzzz"; char *p = put_x(b, b + 4, 0xdeadbeef); CHECK(p == b + 4 && !memcmp(b, "0xdezzz", 7), "put_x stops at the buffer's end"); }
    { const char *s = "7f12-abc"; uint64_t v = hex_in(&s); CHECK(v == 0x7f12 && *s == '-', "hex_in reads up to the '-'"); }

    // reads that fail instead of faulting
    uint64_t here = 0x1122334455667788ull, got = 0;
    CHECK(rd_safe((uint64_t)(uintptr_t)&here, &got, sizeof got) && got == here, "rd_safe reads a stack word");
    CHECK(!rd_safe(0, &got, sizeof got), "rd_safe: address 0 fails");
    CHECK(!rd_safe(0x90, &got, sizeof got), "rd_safe: 0x90 (#82's fault address) fails");
    uint8_t *two = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(two + 2 * pg, pg); g_unmapped = (uint64_t)(uintptr_t)(two + 2 * pg);
    CHECK(!rd_safe(g_unmapped, &got, sizeof got), "rd_safe: an unmapped page fails");
    CHECK(!rd_safe(g_unmapped - 8, out, 16), "rd_safe: a read running into an unmapped page fails");
    CHECK(rd_safe(g_unmapped - 8, &got, 8), "rd_safe: the last word before it reads");

    // the frame chain: A → B → C → a frame on the unmapped page: three return addresses, no fault
    uint64_t *A = (uint64_t *)(two + 0x10), *B = (uint64_t *)(two + 0x40), *C = (uint64_t *)(two + 0x80), ra[32];
    A[0] = (uint64_t)(uintptr_t)B; A[1] = 0xA; B[0] = (uint64_t)(uintptr_t)C; B[1] = 0xB; C[0] = g_unmapped; C[1] = 0xC;
    int n = frame_walk((uint64_t)(uintptr_t)A, ra, 32);
    CHECK(n == 3 && ra[0] == 0xA && ra[1] == 0xB && ra[2] == 0xC, "frame_walk: 3 frames, stops at the unmapped one");
    C[0] = (uint64_t)(uintptr_t)A;   // a frame pointing back down: stops there
    n = frame_walk((uint64_t)(uintptr_t)A, ra, 32);
    CHECK(n == 3, "frame_walk: stops at a frame pointing back down");
    CHECK(frame_walk(0x1235, ra, 32) == 0, "frame_walk: unaligned fp, nothing");
    CHECK(frame_walk(g_unmapped, ra, 32) == 0, "frame_walk: unmapped fp, nothing (no fault)");
    CHECK(frame_walk((uint64_t)(uintptr_t)A, ra, 2) == 2, "frame_walk: at most max frames");

    // the mapping lookup
    uint8_t *three = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(three + pg, pg, PROT_READ);   // (its own mapping between two writable ones)
    { const char *w = where((uint64_t)(uintptr_t)(three + pg + 0x10)); CHECK(w && !strcmp(w, "(anonymous)+0x10"), "maps: an anonymous mapping, offset into it"); }
    CHECK(!where(g_unmapped), "maps: an unmapped address: not found");
    Dl_info di;
    { uint64_t a = (uint64_t)(uintptr_t)&getpid; const char *w = where(a); char want[64];
      dladdr((void *)(uintptr_t)a, &di); snprintf(want, sizeof want, "+%#llx", (unsigned long long)(a - (uint64_t)(uintptr_t)di.dli_fbase));
      CHECK(w && starts(w, "libc") && strstr(w, want) && !strcmp(strstr(w, "+"), want), "maps: libc's getpid, offset from libc's load base (= dladdr's)"); }
    { uint64_t a = (uint64_t)(uintptr_t)&crash_here; const char *w = where(a); char want[64];
      dladdr((void *)(uintptr_t)a, &di); snprintf(want, sizeof want, "t_crash_line+%#llx", (unsigned long long)(a - (uint64_t)(uintptr_t)di.dli_fbase));
      CHECK(w && !strcmp(w, want), "maps: this program's own code, offset from its load base (= dladdr's)"); }

    // the whole line
    { char b[768]; size_t k = crash_line_format(b, sizeof b, 11, 0x90, (uint64_t)(uintptr_t)(fake_img + 0x40), (uint64_t)(uintptr_t)(three + pg + 0x10), 0x7ffc0); b[k] = 0;
      CHECK(!strcmp(b, "[dm] CRASH sig=11 addr=0x90 pc=main+0x40 lr=(anonymous)+0x10 sp=0x7ffc0 thread \"t_crash_line\" (first line; the full record follows)\n"),
            "the line: game module, anonymous mapping, thread name"); }
    { char b[768]; size_t k = crash_line_format(b, sizeof b, 7, 0x750f6cbc0, g_unmapped, 0, 0); b[k] = 0; char want[128];
      snprintf(want, sizeof want, "[dm] CRASH sig=7 addr=0x750f6cbc0 pc=%#llx lr=0x0 sp=0x0 thread", (unsigned long long)g_unmapped);
      CHECK(starts(b, want), "the line: an address in no mapping stays raw"); }
    { char b[40]; memset(b, 'z', sizeof b); size_t k = crash_line_format(b, sizeof b, 11, 0x90, 0, 0, 0);
      CHECK(k == sizeof b && b[k - 1] == '\n' && starts(b, "[dm] CRASH sig=11 addr=0x90"), "the line: cut to a short buffer, still ends with its newline"); }

    // a real crash: the handler makes the line and walks a garbage chain, then exits 139 by itself
    int pf[2]; if (pipe(pf)) return 1;
    pid_t pid = fork();
    if (!pid) {
        close(pf[0]); g_pipe = pf[1];
        struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO }; sigaction(SIGSEGV, &sa, 0);
        crash_here(); _exit(0);
    }
    close(pf[1]); char got_s[1024] = ""; size_t have = 0; ssize_t k;
    while ((k = read(pf[0], got_s + have, sizeof got_s - 1 - have)) > 0) have += (size_t)k;
    got_s[have] = 0; int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 139, "a real crash: the handler finished and exited 139 (no second fault)");
    CHECK(starts(got_s, "[dm] CRASH sig=11 addr=0x90 pc=t_crash_line+0x"), "a real crash: the line names this program's code at the fault");
    CHECK(strstr(got_s, "(first line; the full record follows)\nwalk ok 0\n") != NULL, "a real crash: the garbage frame chains walked nothing, without faulting");
    printf("%s\n", got_s);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
