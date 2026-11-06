// The Vulkan driver the game uses: the phone's own, or a driver package (a .zip with meta.json naming the library, as emulators use: Mesa Turnip for Adreno, Mesa PanVK
// for Mali), unpacked into the app's private storage (libadrenotools loads drivers only from there; nvn_vk.c vk_open opens PanVK directly). The choice is the phone's,
// so it goes to every game's env.txt, and each game's pipeline cache is dropped (compiled by the old driver). Used by the setup screen and the in-game menu.
// The app ships no driver and downloads none: the player gets a package from its developer's own releases page (offered as links for the phone's GPU)
package com.nxnative;

import android.content.Context;
import android.net.Uri;
import java.io.*;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class DriverPackages {
    private DriverPackages() { }
    static final String TURNIP_RELEASES = "https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases";
    static final String RADV_XCLIPSE_RELEASES = "https://github.com/JimVulkan/radv-xclipse/releases";
    static final String PANVK_RELEASES = "https://github.com/FristOneRR/FristOneRR-Panvk-Driver/releases";

    static File root(Context c) { return new File(c.getFilesDir(), "drivers"); }
    // every game's folder (the driver is the phone's): its env.txt and pipeline cache
    private static List<File> gameDirs(Context c) {
        List<File> out = new ArrayList<>(); List<String[]> gs = GameSettings.games(c);
        for (int i = 0; i < gs.size(); i++) out.add(i == 0 ? GameSettings.root(c) : new File(GameSettings.root(c), gs.get(i)[0]));
        return out;
    }
    private static void setAll(Context c, String key, String value) {
        for (File d : gameDirs(c)) { d.mkdirs(); GameSettings.setIn(new File(d, "env.txt"), key, value); new File(d, "vk_pipeline_cache.bin").delete(); }
    }
    static String current(Context c) { return GameSettings.get(c, "DM_VK_DRIVER"); }
    static String label(String lib) { if (lib == null || lib.isEmpty()) return "the phone's own driver"; File f = new File(lib); return f.getParentFile().getName() + " (" + f.getName() + ")"; }
    static void useSystem(Context c) { setAll(c, "DM_VK_DRIVER", ""); setAll(c, "DM_VK_DRIVER_SHA256", ""); }
    // this phone's GPU as the engine wrote it (gpu.txt, lower case; empty before the first start)
    static String gpu(Context c) { try (BufferedReader r = new BufferedReader(new FileReader(new File(GameSettings.dir(c), "gpu.txt")))) { String l = r.readLine(); return l == null ? "" : l.toLowerCase(java.util.Locale.ROOT); } catch (IOException e) { return ""; } }
    // a Turnip (Mesa for Adreno) package: libvulkan_freedreno.so, or the adrenotools name vulkan.adNNN.so, or a folder named after it
    static boolean adrenoOnly(File lib) { String n = lib.getName().toLowerCase(java.util.Locale.ROOT), d = lib.getParentFile() == null ? "" : lib.getParentFile().getName().toLowerCase(java.util.Locale.ROOT);
        return n.contains("freedreno") || n.matches("vulkan\\.ad[0-9x]+\\.so") || d.contains("turnip"); }
    // why this package cannot drive this phone's GPU, or null: a Turnip package on a Mali / Xclipse / PowerVR phone finds no GPU (#71: every start closed at once)
    static String mismatch(Context c, File lib) { String g = gpu(c); return adrenoOnly(lib) && !g.isEmpty() && !g.contains("adreno") ? "a Turnip driver works on Adreno GPUs only, and this phone's GPU is " + g : null; }
    static void use(Context c, File lib) {
        if (mismatch(c, lib) != null) return;   // (the menu shows why; the engine also falls back to the phone's own driver when a driver finds no GPU)
        String sha = ""; if (MaliDriver.isPanvk(lib.getName())) try { sha = MaliDriver.sha256(lib); } catch (IOException e) { }
        setAll(c, "DM_VK_DRIVER_SHA256", sha); setAll(c, "DM_VK_DRIVER", lib.getPath());
    }
    // the installed packages: { folder name, library path }
    static List<String[]> installed(Context c) {
        List<String[]> out = new ArrayList<>(); File[] ds = root(c).listFiles(); if (ds == null) return out;
        java.util.Arrays.sort(ds);
        for (File d : ds) { String lib = library(d); if (lib != null) out.add(new String[] { d.getName(), new File(d, lib).getPath() }); }
        return out;
    }
    private static String library(File dir) {
        String lib = null; File meta = new File(dir, "meta.json");
        if (meta.exists()) { StringBuilder sb = new StringBuilder(); try (BufferedReader r = new BufferedReader(new FileReader(meta))) { String l; while ((l = r.readLine()) != null) sb.append(l); } catch (IOException e) { }
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"]+)\"").matcher(sb); if (m.find()) lib = m.group(1); }
        if (lib == null) { String[] so = dir.list((d, n) -> n.endsWith(".so")); if (so != null && so.length == 1) lib = so[0]; }
        return lib != null && new File(dir, lib).exists() ? lib : null;
    }
    // a package from the file picker: unpacked, checked, made the driver; returns what to tell the player
    static String install(Context c, Uri u, String name) {
        File dir = new File(root(c), name.replaceAll("[^A-Za-z0-9._-]", "_").replaceAll("\\.zip$", ""));
        try {
            deleteTree(dir); dir.mkdirs();
            java.security.MessageDigest zmd; try { zmd = java.security.MessageDigest.getInstance("SHA-256"); } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
            try (java.security.DigestInputStream zd = new java.security.DigestInputStream(new BufferedInputStream(c.getContentResolver().openInputStream(u)), zmd);
                 ZipInputStream z = new ZipInputStream(zd)) { ZipEntry e; byte[] b = new byte[1 << 16];
                while ((e = z.getNextEntry()) != null) { if (e.isDirectory()) continue; String n = new File(e.getName()).getName();
                    try (OutputStream o = new FileOutputStream(new File(dir, n))) { int k; while ((k = z.read(b)) > 0) o.write(b, 0, k); } }
                while (zd.read(b) > 0) { } }   // the zip's central directory too: the digest covers the whole file
            String zipSha = MaliDriver.hex(zmd.digest()), lib = library(dir);
            if (lib == null) { deleteTree(dir); return name + " is not a driver package (no meta.json naming its library)"; }
            File so = new File(dir, lib); String note = "";
            if (MaliDriver.isPanvk(lib)) {   // a PanVK (Mali) package: named when it is a known build, else a warning with its digest
                String known = MaliDriver.known(zipSha, MaliDriver.sha256(so));
                note = known != null ? "\nVerified: " + known + " (SHA-256 matches the release)."
                     : "\nNot a known FristOneRR PanVK release (SHA-256 " + zipSha.substring(0, 16) + "…): download drivers only from the developer's own Releases page.";
            }
            String why = mismatch(c, so);
            if (why != null) return "Driver " + name + " installed, but not used: " + why + ". The phone's own driver stays.";
            use(c, so);
            return "Driver " + name + " installed: it is used from the next start." + note;
        } catch (IOException e) { return "could not read " + name + ": " + e.getMessage(); }
    }
    // where this phone's GPU gets a replacement driver: { label, url }, or null when none supports it (gpu: the engine's gpu.txt, else the chip family)
    static String[] source(Context c) {
        String gpu = ""; try (BufferedReader r = new BufferedReader(new FileReader(new File(GameSettings.dir(c), "gpu.txt")))) { String l = r.readLine(); if (l != null) gpu = l.toLowerCase(java.util.Locale.ROOT); } catch (IOException e) { }
        String fam = GameSettings.profile(c)[0];
        if (gpu.contains("adreno") || (gpu.isEmpty() && fam.startsWith("sd"))) {   // the Turnip build for this Adreno: Gen8 for the 8xx series (8 Elite), the R builds for 6xx / 7xx
            boolean a8 = gpu.matches(".*adreno[^0-9]*8[0-9][0-9].*");
            return new String[] { "Get Turnip for Adreno (StevenMXZ's releases: pick a " + (a8 ? "\"Turnip Gen8\" build for this Adreno 8xx)" : "\"Turnip v…-R\" build)"), TURNIP_RELEASES }; }
        if (gpu.matches(".*mali-g(57|68|77|78|52|72|76).*")) return new String[] { "Get PanVK for Mali (FristOneRR's releases)", PANVK_RELEASES };
        // (not offered for Valhall, G610 / G615 / G710 / G715: PanVK on a Mali-G615 gave a black picture at 0 fps, #71)
        // Mesa RADV built for Samsung's Xclipse (JimVulkan's radv-xclipse): Xclipse 920 / 940 / 530 / 540 / 550 / 950 / 960, Android 14 and newer; its zip (meta.json naming
        //   vulkan.radeon.so) installs as any package here. Not yet tried with this app: offered as experimental (#76 asked for it on an Xclipse 530)
        if (gpu.matches(".*xclipse[^0-9]*(920|940|530|540|550|950|960).*") && android.os.Build.VERSION.SDK_INT >= 34)
            return new String[] { "Get RADV for Xclipse (JimVulkan's releases; experimental with this app)", RADV_XCLIPSE_RELEASES };
        return null;
    }
    // Mesa Turnip's own switches (TU_DEBUG, read by the driver from the environment: env.txt), offered while a Turnip package is the driver. They do on any Turnip build
    // what patched variants bake in (StevenMXZ's force_sysmem patch is TU_DEBUG=sysmem)
    static final String[][] TURNIP_OPTIONS = { { "sysmem", "Render to memory (fixes flicker / corrupted frames from tiled rendering; slower)" },
        { "noubwc", "No framebuffer compression (UBWC: fixes corrupted or striped textures)" }, { "nolrz", "No early depth rejection (LRZ: fixes some depth glitches)" } };
    static boolean isTurnip(String lib) { return lib != null && !lib.isEmpty() && adrenoOnly(new File(lib)); }
    static java.util.Set<String> turnipOptions(Context c) { java.util.Set<String> s = new java.util.LinkedHashSet<>(); for (String o : GameSettings.get(c, "TU_DEBUG").split(",")) if (!o.isEmpty()) s.add(o); return s; }
    static void setTurnipOptions(Context c, java.util.Set<String> s) { setAll(c, "TU_DEBUG", String.join(",", s)); }
    static void deleteTree(File f) { File[] k = f.listFiles(); if (k != null) for (File x : k) deleteTree(x); f.delete(); }
}
