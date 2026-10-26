// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void hlecount_flush(void);
#ifndef __APPLE__
static void *shutdown_fallback(void *a) { (void)a; usleep(1500000); LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0); }
// SIGTERM is blocked in every thread (dm_term_install runs before any other thread starts) and taken here with sigwait, on an ordinary thread
static void *term_thread(void *a) { sigset_t *set = a; int sig; while (sigwait(set, &sig)) {} dm_host_shutdown("SIGTERM"); return NULL; }
#endif
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
#ifdef __APPLE__
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0); });
#else
    { pthread_t th; if (!pthread_create(&th, NULL, shutdown_fallback, NULL)) pthread_detach(th); }
#endif
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    _exit(0);
}
void dm_term_install(void) {
#ifndef __APPLE__
    static sigset_t set; sigemptyset(&set); sigaddset(&set, SIGTERM); pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t th; if (!pthread_create(&th, NULL, term_thread, &set)) pthread_detach(th);
#else
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
#endif
}
