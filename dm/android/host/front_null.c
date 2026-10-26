// Android front end for the command-line host (Phase 1): no window, no input. The guest runs on a big-stack thread, as in the app; graphics presents nowhere.
#include "dm.h"
#include "front.h"
#include <signal.h>
void *front_metal_layer(void) { return NULL; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)max_chars;   // DM_KBD (as on the Mac): the answer to every text prompt, else the app's default name
    const char *k = getenv("DM_KBD"); if (!k || !*k) k = initial && *initial ? initial : "Player";
    snprintf(out, (size_t)out_cap, "%s", k); return 1;
}
static void *guest_tramp(void *p) {
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);   // crash printing must work on the guest thread too
    ((void (*)(void))p)(); LOG("guest thread finished"); exit(0);
}
void front_run(void (*guest)(void)) {
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);   // as the iOS app: Android's main thread stack is 8 MB
    pthread_t t; if (pthread_create(&t, &a, guest_tramp, (void *)guest)) die("cannot start the guest thread");
    pthread_join(t, NULL);
}
