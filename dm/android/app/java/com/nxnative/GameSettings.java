// The player's settings: env.txt in the game folder, KEY=VALUE lines the native side reads at start (android_main.c env_file); the in-game menu (GameMenu.java) changes them,
// and the device profile picks the defaults for this phone (android_main.c profile_of, kept the same here).
package com.nxnative;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

final class GameSettings {
    private GameSettings() { }

    // The games this app runs (assets/games, build_apk.py: one line each, title ID, name, file name stem; the first is the engine's own profile) and the one chosen
    // (<files>/current_game, which android_main.c reads at start). The first game keeps the app's folder (Shield players keep their setup and saves); another one has
    // <files>/<title ID>/: its own setup, saves, settings and mods
    private static List<String[]> games;
    static List<String[]> games(Context c) {
        if (games == null) {
            List<String[]> g = new ArrayList<>();
            for (String asset : new String[] { "games", "game" }) {
                try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open(asset), "UTF-8"))) {
                    String l; while ((l = r.readLine()) != null) { String[] a = l.split("\t"); if (a.length >= 3) g.add(a); } } catch (Exception e) { }
                if (!g.isEmpty()) break;
            }
            if (g.isEmpty()) g.add(new String[] { "01008DB008C2C000", "Pokémon Shield", "PokemonShield" });
            games = g;
        }
        return games;
    }
    static File root(Context c) { return c.getExternalFilesDir(null); }
    static String[] game(Context c) {
        String id = "";
        try (BufferedReader r = new BufferedReader(new FileReader(new File(root(c), "current_game")))) { String l = r.readLine(); if (l != null) id = l.trim(); } catch (IOException e) { }
        for (String[] g : games(c)) if (g[0].equalsIgnoreCase(id)) return g;
        return games(c).get(0);
    }
    static void choose(Context c, String id) {
        try (FileWriter w = new FileWriter(new File(root(c), "current_game"))) { w.write(id + "\n"); } catch (IOException e) { }
    }
    static File dir(Context c) {
        String[] g = game(c); if (g == games(c).get(0)) return root(c);
        File d = new File(root(c), g[0]); d.mkdirs(); return d;
    }
    static File envFile(Context c) { return new File(dir(c), "env.txt"); }
    static String get(Context c, String key) {
        try (BufferedReader r = new BufferedReader(new FileReader(envFile(c)))) { String l; while ((l = r.readLine()) != null) if (l.startsWith(key + "=")) return l.substring(key.length() + 1); } catch (IOException e) { }
        return "";
    }
    static synchronized void set(Context c, String key, String value) {
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(envFile(c)))) { String l; while ((l = r.readLine()) != null) if (!l.startsWith(key + "=")) lines.add(l); } catch (IOException e) { }
        if (!value.isEmpty()) lines.add(key + "=" + value.replace("\n", " "));
        try (FileWriter w = new FileWriter(envFile(c))) { for (String l : lines) w.write(l + "\n"); } catch (IOException e) { }
    }

    static boolean isShield(Context c) { return "01008DB008C2C000".equalsIgnoreCase(game(c)[0]); }
    // the games the built-in 60 fps patch knows (loader.c FPS_SITES: Pokémon Shield and Sword 1.3.2)
    static boolean has60(Context c) { String id = game(c)[0]; return "01008DB008C2C000".equalsIgnoreCase(id) || "0100ABF008968000".equalsIgnoreCase(id); }

    // What Auto gives this phone, from what it is rather than a list of models: { chip family, a description, graphics, textures, anisotropic filtering, frame rate }.
    // Three tiers, each 0 low / 1 mid / 2 top:
    //   CPU  the fastest core's top clock (sysfs cpufreq): 3.0 GHz or more top, 2.4 GHz or more mid (the chip family when unreadable)
    //   GPU  the chip family (Snapdragon 8 Gen 2+, Dimensity 9000+, Exynos 2000: top; older Snapdragon 8, Snapdragon 7, Dimensity 8000, Tensor: mid; the rest low)
    //   RAM  12 GB or more top, 8 GB or more mid
    // and the display's refresh rates. Graphics: High with a top CPU and GPU, Adaptive (High, Low while hot) with both at least mid, else Low. Textures: half with low
    // memory, or a low GPU without top memory. Filtering 8x / 4x / 2x by the GPU. Frame rate: 60 only when all three are top and the display has a mode 60 divides
    // (60, 120, 180, 240 Hz), else the game's 30; at 60 the host's frame step still falls back to 30 when frames run late (hle_nvn.c vstep_tick).
    // GameActivity hands these to the engine (DM_AUTO_*, android_main.c device_profile); the player's own choices in the menu (env.txt) win over them
    // Auto picks 60 fps on top phones only once 60 fps is verified on a phone for game speed and animations (it presents at 60 Hz; the speed is not confirmed yet):
    // until then Auto stays at the game's own 30 and 60 is the player's choice in the menu
    static final boolean AUTO_60 = false;
    static String[] profile(Context c) {
        String m = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : "", mk = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER : "";
        if (m == null) m = ""; if (mk == null) mk = ""; m = m.toLowerCase(java.util.Locale.ROOT); String n;
        if (m.matches("sm\\d.*") || m.matches("sdm\\d.*")) { int v = lead(m.substring(m.charAt(1) == 'd' ? 3 : 2));
            n = v >= 8000 && v < 8500 ? "sd8old" : v >= 8500 && v < 9000 ? "sd8" : v >= 7300 && v < 8000 ? "sd7" : (v >= 4000 && v < 7300) || (v > 0 && v < 1000) ? "sd6" : "none"; }
        else if (m.matches("mt\\d.*")) { int v = lead(m.substring(2)); n = v == 6983 || v == 6985 || v == 6989 || v == 6991 || v == 6993 ? "high" : v >= 6889 && v <= 6899 ? "mid" : "low"; }
        else if (m.startsWith("s5e") || m.contains("exynos")) { boolean s5e = m.startsWith("s5e"); String d = s5e ? m.substring(3) : m.replaceAll("^\\D*", ""); int v = lead(d);
            n = (s5e && v >= 9900 && v < 10000) || (!s5e && v >= 2000 && v < 3000) ? "high" : "low"; }
        else if (m.contains("tensor") || (mk.toLowerCase(java.util.Locale.ROOT).startsWith("google") && !m.isEmpty())) n = "mid";
        else n = "none";
        int gpu = n.equals("sd8") || n.equals("high") ? 2 : n.equals("sd8old") || n.equals("sd7") || n.equals("mid") ? 1 : n.equals("none") ? -1 : 0;
        long khz = maxCpuKhz(); int cpu = khz >= 3_000_000 ? 2 : khz >= 2_400_000 ? 1 : khz > 0 ? 0 : -1;
        if (cpu < 0) cpu = gpu < 0 ? 1 : gpu; if (gpu < 0) gpu = cpu;   // (each stands in for the other when unknown)
        double gb = memoryGb(c); int ram = gb >= 11.5 ? 2 : gb >= 7.5 || gb <= 0 ? 1 : 0;
        String gfx = cpu == 2 && gpu == 2 ? "high" : cpu >= 1 && gpu >= 1 ? "auto" : "low";
        String tex = ram == 0 || (gpu == 0 && ram < 2) ? "half" : "full";
        String aniso = gpu == 2 ? "8" : gpu == 1 ? "4" : "2";
        String fps = AUTO_60 && cpu == 2 && gpu == 2 && ram == 2 && displayDivides(c, 60) ? "60" : "30";
        String[] t = { "low", "mid", "top" };
        String what = String.format(java.util.Locale.ROOT, "%s CPU%s, %s GPU, %s memory (%.0f GB), display up to %.0f Hz", t[cpu], khz > 0 ? String.format(java.util.Locale.ROOT, " %.1f GHz", khz / 1e6) : "",
                                    t[gpu], t[ram], Math.ceil(gb), maxRefresh(c));
        return new String[] { n, what, gfx, tex, aniso, fps };
    }
    // the fastest core's top clock in kHz (cpu*/cpufreq/cpuinfo_max_freq), 0 when unreadable
    static long maxCpuKhz() {
        long best = 0;
        for (int i = 0; i < 16; i++) {
            try (BufferedReader r = new BufferedReader(new FileReader("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq"))) { String l = r.readLine(); if (l != null) best = Math.max(best, Long.parseLong(l.trim())); }
            catch (IOException | NumberFormatException e) { }
        }
        return best;
    }
    // the display's refresh rates (every mode it supports)
    static float[] refreshRates(Context c) {
        android.hardware.display.DisplayManager dm = c.getSystemService(android.hardware.display.DisplayManager.class);
        android.view.Display d = dm != null ? dm.getDisplay(android.view.Display.DEFAULT_DISPLAY) : null; if (d == null) return new float[] { 60 };
        android.view.Display.Mode[] ms = d.getSupportedModes(); float[] out = new float[ms.length]; for (int i = 0; i < ms.length; i++) out[i] = ms[i].getRefreshRate();
        return out.length > 0 ? out : new float[] { d.getRefreshRate() };
    }
    static float maxRefresh(Context c) { float m = 0; for (float r : refreshRates(c)) m = Math.max(m, r); return m; }
    // a mode the frame rate divides (60 fps: 60 / 120 Hz; 30 fps: 60 / 90 / 120 Hz), so every frame shows for the same number of refreshes
    static boolean displayDivides(Context c, int fps) {
        for (float r : refreshRates(c)) { float k = r / fps; if (k >= 0.98f && Math.abs(k - Math.round(k)) < 0.03f) return true; }
        return false;
    }
    static double memoryGb(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class); if (am == null) return 0;
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
        return mi.totalMem / 1e9;
    }
    private static int lead(String d) { int v = 0; for (int i = 0; i < d.length() && i < 4 && Character.isDigit(d.charAt(i)); i++) v = v * 10 + (d.charAt(i) - '0'); return v; }
}
