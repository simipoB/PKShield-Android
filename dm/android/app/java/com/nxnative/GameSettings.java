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

    // What Auto gives this phone: { profile name, what it is, graphics, textures }, from its chip (android_main.c profile_of, kept the same) and then its memory, for every
    // phone (android_main.c device_profile, the same rule): under 8 GB half textures whatever the chip, under 6 GB also Low; 12 GB or more full textures unless the chip is weak
    static String[] profile(Context c) {
        String m = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : "", mk = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER : "";
        if (m == null) m = ""; if (mk == null) mk = ""; m = m.toLowerCase(java.util.Locale.ROOT); String n;
        if (m.matches("sm\\d.*") || m.matches("sdm\\d.*")) { int v = lead(m.substring(m.charAt(1) == 'd' ? 3 : 2));
            n = v >= 8000 && v < 8500 ? "sd8old" : v >= 8500 && v < 9000 ? "sd8" : v >= 7300 && v < 8000 ? "sd7" : (v >= 4000 && v < 7300) || (v > 0 && v < 1000) ? "sd6" : null; }
        else if (m.matches("mt\\d.*")) { int v = lead(m.substring(2)); n = v == 6983 || v == 6985 || v == 6989 || v == 6991 || v == 6993 ? "high" : v >= 6889 && v <= 6899 ? "mid" : "low"; }
        else if (m.startsWith("s5e") || m.contains("exynos")) { boolean s5e = m.startsWith("s5e"); String d = s5e ? m.substring(3) : m.replaceAll("^\\D*", ""); int v = lead(d);
            n = (s5e && v >= 9900 && v < 10000) || (!s5e && v >= 2000 && v < 3000) ? "high" : "low"; }
        else if (m.contains("tensor") || (mk.toLowerCase(java.util.Locale.ROOT).startsWith("google") && !m.isEmpty())) n = "mid";
        else n = "none";
        String what, gfx, tex;
        switch (n) {
            case "sd8": what = "Snapdragon 8 series"; gfx = "high"; tex = "full"; break;
            case "sd8old": what = "older Snapdragon 8 series"; gfx = "auto"; tex = "full"; break;
            case "sd7": what = "Snapdragon 7 series"; gfx = "auto"; tex = "full"; break;
            case "sd6": what = "Snapdragon 6 / 4 series"; gfx = "low"; tex = "half"; break;
            case "high": what = "Dimensity 9000 / Exynos 2000 series"; gfx = "auto"; tex = "full"; break;
            case "mid": what = "Dimensity 8000 series / Google Tensor"; gfx = "low"; tex = "half"; break;
            case "low": what = "MediaTek / Exynos, lighter settings"; gfx = "low"; tex = "half"; break;
            default: what = "chip not in the table"; gfx = "high"; tex = "full";
        }
        double gb = memoryGb(c); boolean weak = n.equals("sd6") || n.equals("low");
        if (gb > 0 && gb < 7.5) tex = "half"; else if (gb >= 11.5 && !weak) tex = "full";
        if (gb > 0 && gb < 5.5) gfx = "low";
        if (gb > 0) what += String.format(java.util.Locale.ROOT, ", %.0f GB", Math.ceil(gb));
        return new String[] { n, what, gfx, tex };
    }
    static double memoryGb(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class); if (am == null) return 0;
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
        return mi.totalMem / 1e9;
    }
    private static int lead(String d) { int v = 0; for (int i = 0; i < d.length() && i < 4 && Character.isDigit(d.charAt(i)); i++) v = v * 10 + (d.charAt(i) - '0'); return v; }
}
