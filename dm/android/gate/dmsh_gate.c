// dmsh_gate: runs the shader translator (DM_GATE_EXEC, with this program's arguments) when the phone has the memory for one more of it. setup_game.py hands it
// to dmsh_build.py as XLATE, so the build itself is unchanged (dmsh_build.py is part of the shader chain version) while its J translators share what memory there is.
//   Slots: gate.<i> lock files in DM_GATE_DIR (else TMPDIR), DM_GATE_SLOTS of them (8). Slot 0 always runs, so the build never stops; every other slot starts only
//   while MemAvailable is at least DM_GATE_FLOOR_MB (300) + DM_GATE_MB (600, about one translator). A phone with 16 GB runs all J, one with 4 GB as many as fit.
//   A translator the low-memory killer stops (SIGKILL) runs again, up to 3 times, in slot 0 once memory is back, rather than failing the setup (#43).
// The lock is held on an open file (flock), so a gate that dies lets go of its slot by itself.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long env_l(const char *k, long d) { const char *v = getenv(k); return v && atol(v) > 0 ? atol(v) : d; }
static long avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char ln[128]; long kb = -1;
    if (!f) return -1;
    while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "MemAvailable:", 13)) { kb = atol(ln + 13); break; }
    fclose(f); return kb < 0 ? -1 : kb / 1024;
}
static int take(const char *dir, int i) {
    char p[1024]; snprintf(p, sizeof p, "%s/gate.%d", dir, i);
    int fd = open(p, O_CREAT | O_RDWR | O_CLOEXEC, 0600); if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    return fd;
}
static void nap(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
// a slot: any free one while memory allows, else slot 0 only (alone: slot 0 only)
static int acquire(const char *dir, int slots, long need, int alone) {
    int said = 0;
    for (;;) {
        for (int i = 0; i < (alone ? 1 : slots); i++) {
            int fd = take(dir, i); if (fd < 0) continue;
            long a = i ? avail_mb() : 0;
            if (!i || a < 0 || a >= need) return fd;   // (no meminfo: no gate)
            close(fd);
            if (!said) { fprintf(stderr, "gate: %ld MB free, waiting for %ld before another translator\n", a, need); said = 1; }
            break;   // (the higher slots need the same memory)
        }
        nap(100);
    }
}
int main(int argc, char **argv) {
    const char *exe = getenv("DM_GATE_EXEC"); if (!exe || !*exe) { fprintf(stderr, "gate: DM_GATE_EXEC is not set\n"); return 127; }
    const char *dir = getenv("DM_GATE_DIR"); if (!dir || !*dir) dir = getenv("TMPDIR"); if (!dir || !*dir) dir = "/data/local/tmp";
    int slots = (int)env_l("DM_GATE_SLOTS", 8); long need = env_l("DM_GATE_FLOOR_MB", 300) + env_l("DM_GATE_MB", 600);
    argv[0] = (char *)exe; (void)argc;
    for (int attempt = 0;; attempt++) {
        int fd = acquire(dir, slots, need, attempt > 0);
        pid_t pid = fork();
        if (pid < 0) { close(fd); fprintf(stderr, "gate: fork: %s\n", strerror(errno)); return 127; }
        if (!pid) { execv(exe, argv); fprintf(stderr, "gate: %s: %s\n", exe, strerror(errno)); _exit(127); }
        int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
        close(fd);
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL && attempt < 3) {
            fprintf(stderr, "gate: the translator was stopped for memory (%ld MB free): again, alone\n", avail_mb()); nap(1000); continue;
        }
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
}
