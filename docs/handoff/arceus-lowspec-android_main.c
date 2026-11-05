// "Lower specs" (the settings' DM_LOWSPEC: 1 lower, 2 lowest), for phones that cannot hold the game's frame rate: the POTATO mod's RomFS files, kept in this library
//   (lowspec_files.h) and written to <files>/lowspec on start, are laid over the game's RomFS (hle_fs.c DM_ROMFS_OVERLAY, first root wins): a shorter draw distance for
//   Pokémon, people and trees (bin/misc/app_config/field_common_config.bin) and lighter shadows, at 1 fewer (Low Shadows), at 2 none (No Shadows: field_shadow_config.bin)
#include "lowspec_files.h"
static int write_all(const char *path, const unsigned char *b, size_t n) {
    char d[1200]; snprintf(d, sizeof d, "%s", path);
    for (char *s = d + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(d, 0770); *s = '/'; }
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    int ok = fwrite(b, 1, n, f) == n; return fclose(f) == 0 && ok;
}
static void lowspec(void) {
    const char *e = getenv("DM_LOWSPEC"); int lv = e ? atoi(e) : 0; if (lv < 1 || lv > 2) return;
    char root[1100], f[1300]; snprintf(root, sizeof root, "%s/lowspec/romfs", g_files);
    snprintf(f, sizeof f, "%s/bin/misc/app_config/field_common_config.bin", root);
    int ok = write_all(f, LS_draw_distance, sizeof LS_draw_distance);
    snprintf(f, sizeof f, "%s/bin/misc/app_config/field_shadow_config.bin", root);
    ok &= lv == 2 ? write_all(f, LS_no_shadows, sizeof LS_no_shadows) : write_all(f, LS_low_shadows, sizeof LS_low_shadows);
    if (!ok) { fprintf(stderr, "boot: [RED] lower specs: could not write %s\n", root); return; }
    const char *o = getenv("DM_ROMFS_OVERLAY"); char all[4096];
    snprintf(all, sizeof all, "%s%s%s", root, o && *o ? ":" : "", o && *o ? o : "");   // (ahead of any other overlay)
    setenv("DM_ROMFS_OVERLAY", all, 1);
    fprintf(stderr, "boot: lower specs %d: shorter draw distance, %s (RomFS overlay %s)\n", lv, lv == 2 ? "no shadows" : "lighter shadows", root);
}
// and call lowspec(); right after device_profile(); in android_main
