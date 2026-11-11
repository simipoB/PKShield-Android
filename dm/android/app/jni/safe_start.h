// Safe starts (android_main.c host_start; tests: dm/android/app/jni/tests/t_safe_start.c): a start that crashed in the graphics driver before 30 s of play (boot_state:
// the device up, "crashed" outside the game's own code, no "playing", no mods to blame) makes the next start one level safer. The level is kept in <files>/safe_start for
// this driver and app version (a driver package or an update starts over at the normal settings); a start that plays keeps its level. Each level turns off one more
// feature a driver crashed on, set without overwriting after env.txt is read, so the player's own settings win and the phone profile's defaults (set after) do not:
//   1 full textures (Arm's r44 closed the game with half ones: #82)
//   2 uniform buffers copied when each draw is recorded (DM_VK_UBO_SNAPSHOT), not bound in the game's pools: a Mali-G615 MC2 on Arm's r44p1 still closed three frames in
//     with full textures and every later level (#105, v0.55), and the one such phone that plays (#77) has this on
//   3 BC textures converted on the CPU, not by the BC layer's GPU shaders   4 BC textures decoded, not transcoded to ETC2   5 no float controls (Arm's compiler: #89)
// Mali crash test build (#158): on a Mali GPU (gpu.txt) the levels are instead one candidate fix each for Arm's r44 crash a few frames in (its image barrier handling
// meets an image whose driver state is gone), so the log of the start that plays names the one that helped:
//   1 first-use and upload barriers name their levels and layers instead of VK_REMAINING_* (DM_VK_EXACT_RANGES=1)   2 destroyed images held two more submissions
//   (DM_VK_DEFER_HOLD=2)   3 no view / image quarantine (DM_VK_VIEW_QUARANTINE=0)   4 all three   5 all three + every switch of the usual levels.
//   (Split submissions are off for Sword / Shield already: GameActivity sets DM_VK_SPLIT=0.) These are set over env.txt (a test: the log says so); the key is the build's own, so the test starts at 0
// Past the last level the driver dialog appears as before (boot_guard). Returns 1 when this start went a level up (boot_guard then shows no driver dialog)
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SAFE_MAX 5
static int safe_start(const char *files) {
    char bp[1200], sp[1200], gp[1200], prev[512] = "", gpu[256] = "", line[512] = "", key[512], had[512] = "";
    int up = 0;
    snprintf(bp, sizeof bp, "%s/boot_state", files); snprintf(sp, sizeof sp, "%s/safe_start", files); snprintf(gp, sizeof gp, "%s/gpu.txt", files);
    FILE *f = fopen(bp, "r"); if (f) { size_t k = fread(prev, 1, sizeof prev - 1, f); prev[k] = 0; fclose(f); }
    f = fopen(gp, "r"); if (f) { if (!fgets(gpu, sizeof gpu, f)) gpu[0] = 0; fclose(f); } gpu[strcspn(gpu, "\r\n")] = 0;
    const char *drv = getenv("DM_VK_DRIVER"), *ver = getenv("DM_APP_VERSION");
    int mali = strstr(gpu, "Mali") != NULL;
    snprintf(key, sizeof key, "%s|%s%s", drv && *drv ? drv : "stock", ver ? ver : "?", mali ? "|mali-test2" : "");
    int level = 0; f = fopen(sp, "r");
    if (f) { if (fgets(line, sizeof line, f)) level = atoi(line); if (fgets(had, sizeof had, f)) had[strcspn(had, "\r\n")] = 0; fclose(f);
        if (strcmp(had, key)) { if (level) fprintf(stderr, "boot: safe start level %d was for another driver or app version: the normal settings again\n", level); level = 0; unlink(sp); } }
    if (level < 0 || level > SAFE_MAX) level = 0;
    int had_mods = strstr(prev, "mods 0") == NULL && strstr(prev, "mods ") != NULL;
    if (strstr(prev, "crashed") && !strstr(prev, "in the game's code") && strstr(prev, "device") && !strstr(prev, "playing") && !had_mods && level < SAFE_MAX) {
        level++; up = 1;
        f = fopen(sp, "w"); if (f) { fprintf(f, "%d\n%s\n", level, key); fclose(f); }
        fprintf(stderr, "boot: the last start closed in the graphics driver before 30 s of play: this one is a level safer (safe start level %d of %d)\n", level, SAFE_MAX);
    }
    if (!level) { if (mali) fprintf(stderr, "boot: [mali-test] safe start level 0 for %s: the normal settings (a crash in the driver makes the next start try a fix)\n", gpu); return up; }
    if (mali) {
        static const char *mw[SAFE_MAX + 1] = { "", "barriers with exact levels and layers (DM_VK_EXACT_RANGES=1)", "destroyed images held two more submissions (DM_VK_DEFER_HOLD=2)",
            "no view / image quarantine (DM_VK_VIEW_QUARANTINE=0)", "all three: exact ranges, held longer, no quarantine", "all three + full textures, uniform buffers copied, BC on the CPU and decoded, no float controls" };
        if (level == 1 || level >= 4) setenv("DM_VK_EXACT_RANGES", "1", 1);
        if (level == 2 || level >= 4) setenv("DM_VK_DEFER_HOLD", "2", 1);
        if (level == 3 || level >= 4) setenv("DM_VK_VIEW_QUARANTINE", "0", 1);
        if (level >= 5) { setenv("DM_VK_TEX_QUALITY", "full", 1); setenv("DM_VK_UBO_SNAPSHOT", "1", 1); setenv("DM_VK_BC_GPU", "0", 1); setenv("DM_VK_BC_TRANSCODE", "off", 1); setenv("DM_VK_FLOAT_CONTROLS", "0", 1); }
        fprintf(stderr, "boot: [mali-test] safe start level %d of %d for %s: %s\n", level, SAFE_MAX, gpu, mw[level]);
        return up;
    }
    static const char *what[SAFE_MAX + 1] = { "", "full textures", "uniform buffers copied", "BC textures converted on the CPU", "BC textures decoded, not transcoded", "no float controls" };
    if (level >= 1) setenv("DM_VK_TEX_QUALITY", "full", 0);
    if (level >= 2) setenv("DM_VK_UBO_SNAPSHOT", "1", 0);
    if (level >= 3) setenv("DM_VK_BC_GPU", "0", 0);
    if (level >= 4) setenv("DM_VK_BC_TRANSCODE", "off", 0);
    if (level >= 5) setenv("DM_VK_FLOAT_CONTROLS", "0", 0);
    char t[256]; size_t n = 0; for (int i = 1; i <= level && n < sizeof t; i++) n += (size_t)snprintf(t + n, sizeof t - n, "%s%s", i > 1 ? ", " : "", what[i]);
    fprintf(stderr, "boot: safe start level %d for %s: %s (the player's own settings in env.txt win)\n", level, *gpu ? gpu : "this GPU", t);
    return up;
}
