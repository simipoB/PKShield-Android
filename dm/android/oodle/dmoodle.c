// dmoodle: the game's own Oodle decoder as a command, for the setup's shader scan of Trinity games (Pokémon Violet, Legends: Z-A: every file of arc/data.trpfs is
// Oodle-compressed, and Oodle's decoder is not open source). It loads the engine (libnxnative.so, beside it or DM_ENGINE) and runs the host in its tool mode
// (dm/host/hle_oodle.c DM_OODLE_SERVE): the rewritten modules are loaded, OodleLZ_Decompress is found in main, and requests on stdin are answered on stdout.
//   dmoodle <rewritten modules folder>     (shader_split.py runs it as DM_OODLE_HOST, with DM_OODLE_MODULES as the argument)
#include <dlfcn.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dmoodle <rewritten modules folder>\n"); return 2; }
    char lib[4096]; const char *e = getenv("DM_ENGINE");
    const char *nl = getenv("NX_NATIVE_LIB_DIR");   // (the setup: the app's native library folder)
    if (e && *e) snprintf(lib, sizeof lib, "%s", e);
    else if (nl && *nl) snprintf(lib, sizeof lib, "%s/libnxnative.so", nl);
    else { char self[4096]; snprintf(self, sizeof self, "%s", argv[0]); snprintf(lib, sizeof lib, "%s/libnxnative.so", dirname(self)); }
    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL); if (!h) h = dlopen("libnxnative.so", RTLD_NOW | RTLD_GLOBAL);   // (the app's native library folder)
    if (!h) { fprintf(stderr, "dmoodle: cannot load the engine (%s): %s\n", lib, dlerror()); return 127; }
    int (*dm_main)(int, char **) = (int (*)(int, char **))dlsym(h, "dm_main");
    if (!dm_main) { fprintf(stderr, "dmoodle: the engine has no dm_main\n"); return 127; }
    if (!getenv("DM_OODLE_SERVE")) setenv("DM_OODLE_SERVE", "1", 1);
    char *av[3] = { "dmhost", argv[1], NULL };
    return dm_main(2, av);
}
