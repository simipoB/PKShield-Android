// The in-game settings: an Android dialog over the game, opened by the touch controls' menu button (touch_ui.c) or by holding − on a controller for 3 s, which goes
// straight to the controller buttons (android_main.c front_open_menu). Every change is written to env.txt (GameSettings) for the next start; the picture, the touch
// controls and the controller buttons also change at once (nativeSetting), the rest say they apply from the next start.
package com.nxnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.view.KeyEvent;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

public final class GameMenu {
    private GameMenu() { }
    private static native boolean nativeSetting(String key, String value);
    private static native void nativeEditTouch();
    private static boolean open;   // one menu at a time (UI thread)

    // android_main.c: what 0 = the menu, 1 = the controller buttons
    public static void show(Activity a, int what) {
        a.runOnUiThread(() -> { if (open) return; if (what == 1) remap(a); else menu(a); });
    }

    private static void set(Activity a, String key, String value) {
        GameSettings.set(a, key, value);
        boolean now; try { now = nativeSetting(key, value); } catch (UnsatisfiedLinkError e) { now = false; }
        if (!now) Toast.makeText(a, "Saved: applies the next time you start the game.", Toast.LENGTH_SHORT).show();
    }
    private static String label(String[][] opts, String cur) { for (String[] o : opts) if (o[0].equals(cur)) return o[1]; return opts[0][1]; }

    private static String[][] graphics(Activity a) {
        String[] p = GameSettings.profile(a); String rec = p[2].equals("high") ? "High" : p[2].equals("auto") ? "Adaptive" : "Low";
        return new String[][] { { "", "Auto: this phone gets " + rec + " (" + p[1] + ")" }, { "high", "High: TV mode, 1080p, full effects" },
                                { "low", "Low: handheld mode, 720p, lighter" }, { "auto", "Adaptive: High, Low while the phone is hot" } };
    }
    private static String[][] textures(Activity a) {
        String[] p = GameSettings.profile(a); String rec = p[3].equals("half") ? "Half" : "Full";
        return new String[][] { { "", "Auto: this phone gets " + rec + " (" + p[1] + ")" }, { "full", "Full: the game's textures as they are" },
                                { "half", "Half: large textures at half size, less memory and GPU work" } };
    }
    private static final String[][] PICTURE = { { "", "16:9, as the game is made" }, { "fill", "Fill the screen (stretched)" }, { "20:9", "20:9, for a widescreen mod" } };
    private static final String[][] SCALER = { { "", "Sharp: the picture upscaled with sharpening (lighter settings keep their detail)" }, { "smooth", "Smooth: a plain stretch" } };
    private static String[][] fps(Activity a) {
        String[] p = GameSettings.profile(a);
        return new String[][] { { "", "Auto: this phone gets " + p[5] + " fps (" + p[1] + ")" }, { "30", "30 fps, as the game is made" }, { "60", "60 fps (experimental: needs a fast phone)" } };
    }
    private static final String[][] PAD = { { "", "By position, as on a Switch pad" }, { "labels", "By label (the button marked A is A)" } };

    private static void menu(Activity a) {
        List<String> rows = new ArrayList<>(); List<Runnable> acts = new ArrayList<>();
        String[][] g = graphics(a), t = textures(a);
        rows.add("Picture: " + label(PICTURE, GameSettings.get(a, "DM_ASPECT"))); acts.add(() -> choose(a, "Picture", PICTURE, "DM_ASPECT"));
        rows.add("Upscaling: " + label(SCALER, GameSettings.get(a, "DM_SCALER"))); acts.add(() -> choose(a, "Upscaling", SCALER, "DM_SCALER"));
        rows.add("Graphics: " + label(g, GameSettings.get(a, "DM_GRAPHICS"))); acts.add(() -> choose(a, "Graphics", graphics(a), "DM_GRAPHICS"));
        rows.add("Textures: " + label(t, GameSettings.get(a, "DM_VK_TEX_QUALITY"))); acts.add(() -> choose(a, "Textures", textures(a), "DM_VK_TEX_QUALITY"));
        if (GameSettings.has60(a)) { rows.add("Frame rate: " + label(fps(a), GameSettings.get(a, "DM_FPS_TARGET"))); acts.add(() -> choose(a, "Frame rate", fps(a), "DM_FPS_TARGET")); }
        rows.add("Controller buttons: " + label(PAD, GameSettings.get(a, "DM_PAD_LAYOUT"))); acts.add(() -> choose(a, "Controller buttons", PAD, "DM_PAD_LAYOUT"));
        rows.add("Remap controller buttons (or hold − for 3 s)"); acts.add(() -> remap(a));
        rows.add("Touch controls: look"); acts.add(() -> touchLook(a));
        rows.add("Move the touch controls"); acts.add(() -> {
            try { nativeEditTouch(); } catch (UnsatisfiedLinkError e) { }
            Toast.makeText(a, "Drag a control to move it, pinch to resize, double-tap to hide. Hold − for 2 s to save (hold + to reset).", Toast.LENGTH_LONG).show(); });
        rows.add("Mods (" + mods(a).size() + ")"); acts.add(() -> modList(a));
        rows.add("Import a mod (.zip)"); acts.add(() -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_MOD));
        rows.add("Export save data"); acts.add(() -> pick(a, Intent.ACTION_CREATE_DOCUMENT, GameSettings.game(a)[2] + "-save.zip", REQ_EXPORT));
        rows.add("Import save data"); acts.add(() -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_IMPORT));
        open = true;
        new AlertDialog.Builder(a).setTitle(GameSettings.game(a)[1] + " settings").setItems(rows.toArray(new String[0]), (d, w) -> { open = false; acts.get(w).run(); })
            .setNegativeButton("Back to the game", null).setOnDismissListener(d -> open = false).show();
    }
    // a choice: picking one saves it and comes back to the menu
    private static void choose(Activity a, String title, String[][] opts, String key) {
        String cur = GameSettings.get(a, key); int sel = 0; String[] names = new String[opts.length];
        for (int k = 0; k < opts.length; k++) { names[k] = opts[k][1]; if (opts[k][0].equals(cur)) sel = k; }
        open = true;
        new AlertDialog.Builder(a).setTitle(title).setSingleChoiceItems(names, sel, (d, w) -> { set(a, key, opts[w][0]); d.dismiss(); })
            .setNegativeButton("Back", null).setOnDismissListener(d -> { open = false; menu(a); }).show();
    }
    private static void touchLook(Activity a) {
        LinearLayout root = new LinearLayout(a); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(48, 24, 48, 0);
        slider(a, root, "Opacity", "DM_TOUCH_OPACITY", 20, 100, 100, v -> v + " %");
        slider(a, root, "Fade after", "DM_TOUCH_FADE", 0, 10, 3, v -> v == 0 ? "never fade" : v + " s without a touch");
        open = true;
        new AlertDialog.Builder(a).setTitle("Touch controls").setView(root).setPositiveButton("Done", null).setOnDismissListener(d -> { open = false; menu(a); }).show();
    }
    private static void slider(Activity a, LinearLayout root, String label, String key, int min, int max, int def, java.util.function.IntFunction<String> fmt) {
        TextView t = new TextView(a); t.setPadding(0, 16, 0, 0); root.addView(t);
        SeekBar sb = new SeekBar(a); sb.setMax(max - min);
        int v; try { v = Integer.parseInt(GameSettings.get(a, key)); } catch (NumberFormatException e) { v = def; }
        v = Math.max(min, Math.min(max, v)); sb.setProgress(v - min); t.setText(label + ": " + fmt.apply(v));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int p, boolean user) { t.setText(label + ": " + fmt.apply(p + min)); if (user) set(a, key, String.valueOf(p + min)); }
            public void onStartTrackingTouch(SeekBar b) { }
            public void onStopTrackingTouch(SeekBar b) { }
        });
        root.addView(sb);
    }

    // Remapping (PadMapping.java, DM_PAD_MAPPING): each Switch button with the key it is on; choosing one waits for a press on the controller, Reset clears the remapping
    private static void remap(Activity a) {
        Map<String, Integer> map = PadMapping.parse(GameSettings.get(a, "DM_PAD_MAPPING"));
        String[] rows = new String[PadMapping.BUTTONS.length];
        for (int i = 0; i < rows.length; i++) { Integer kc = map.get(PadMapping.BUTTONS[i][0]); rows[i] = PadMapping.BUTTONS[i][1] + ": " + (kc != null ? PadMapping.keyName(kc) : "default"); }
        open = true;
        new AlertDialog.Builder(a).setTitle("Remap controller buttons").setItems(rows, (d, which) -> { open = false; captureKey(a, map, which); })
            .setNeutralButton("Reset all", (d, w) -> { set(a, "DM_PAD_MAPPING", ""); Toast.makeText(a, "Controller buttons reset.", Toast.LENGTH_SHORT).show(); })
            .setNegativeButton("Done", null).setOnDismissListener(d -> open = false).show();
    }
    private static void captureKey(Activity a, Map<String, Integer> map, int which) {
        String[] btn = PadMapping.BUTTONS[which];
        AlertDialog d = new AlertDialog.Builder(a).setTitle(btn[1]).setMessage("Press the controller button for " + btn[1] + ".")
            .setNegativeButton("Cancel", (x, w) -> remap(a)).create();
        d.setOnKeyListener((x, kc, e) -> {
            if (e.getAction() != KeyEvent.ACTION_DOWN || !PadMapping.isPadKey(e)) return false;
            map.values().removeIf(v -> v == kc);   // a key drives one Switch button
            map.put(btn[0], kc); set(a, "DM_PAD_MAPPING", PadMapping.serialize(map));
            d.dismiss(); remap(a); return true;
        });
        d.show();
    }

    // Mods: <files>/mods/<mod>/romfs (files that replace the game's) and <mod>/exefs/*.pchtxt (code patches for this game version), as the Switch emulators lay them out;
    // loose .pchtxt files in mods/ work too. Ticking one on or off renames its folder (a leading "-" switches it off, android_main.c / loader.c); applies at the next start
    private static File modsDir(Activity a) { return new File(GameSettings.dir(a), "mods"); }
    private static List<File> mods(Activity a) {
        File[] fs = modsDir(a).listFiles(); List<File> out = new ArrayList<>(); if (fs == null) return out;
        Arrays.sort(fs); for (File f : fs) if (f.isDirectory() && !f.getName().startsWith(".") && !f.getName().equals("romfs")) out.add(f);
        return out;
    }
    private static void modList(Activity a) {
        List<File> ms = mods(a);
        String where = "Put each mod in its own folder in Android/data/" + a.getPackageName() + "/files/mods/ — <mod>/romfs for files, <mod>/exefs for .pchtxt code patches "
                     + "(they must be made for " + GameSettings.game(a)[1] + " 1.3.2).";
        open = true;
        AlertDialog.Builder b = new AlertDialog.Builder(a).setTitle("Mods").setNegativeButton("Back", null).setOnDismissListener(d -> { open = false; menu(a); });
        if (ms.isEmpty()) { b.setMessage("No mods yet. " + where).show(); return; }
        String[] names = new String[ms.size()]; boolean[] on = new boolean[ms.size()];
        for (int i = 0; i < names.length; i++) { String n = ms.get(i).getName(); on[i] = !n.startsWith("-"); names[i] = on[i] ? n : n.substring(1); }
        b.setMultiChoiceItems(names, on, (d, i, checked) -> {
            File f = ms.get(i), to = new File(f.getParentFile(), checked ? names[i] : "-" + names[i]);
            if (!f.getName().equals(to.getName()) && f.renameTo(to)) { ms.set(i, to); Toast.makeText(a, (checked ? "On" : "Off") + ": applies the next time you start the game.", Toast.LENGTH_SHORT).show(); }
        }).setPositiveButton("Where do mods go?", (d, w) -> Toast.makeText(a, where, Toast.LENGTH_LONG).show()).show();
    }
    // ---------- files: save data out and in, mods in (the system file picker; GameActivity.onActivityResult hands the answer here) ----------
    static final int REQ_EXPORT = 101, REQ_IMPORT = 102, REQ_MOD = 103;
    private static void pick(Activity a, String action, String name, int req) {
        Intent i = new Intent(action); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip");
        if (name != null) i.putExtra(Intent.EXTRA_TITLE, name);
        if (req == REQ_EXPORT) Toast.makeText(a, "Save in the game first: the export is the last save the game wrote.", Toast.LENGTH_LONG).show();
        a.startActivityForResult(i, req);
    }
    static boolean onResult(Activity a, int req, int res, Intent data) {
        if (req != REQ_EXPORT && req != REQ_IMPORT && req != REQ_MOD) return false;
        Uri u = res == Activity.RESULT_OK && data != null ? data.getData() : null; if (u == null) return true;
        File files = GameSettings.dir(a);
        new Thread(() -> {
            String msg;
            try {
                if (req == REQ_EXPORT) {
                    try (OutputStream o = a.getContentResolver().openOutputStream(u, "wt"); ZipOutputStream z = new ZipOutputStream(o)) {
                        File save = new File(files, "save").getCanonicalFile(); int n = zipTree(save, save, z); msg = "Exported " + n + " save file" + (n == 1 ? "" : "s") + "."; }
                } else if (req == REQ_IMPORT) {
                    try (InputStream in = a.getContentResolver().openInputStream(u)) {
                        int n = SaveImport.install(in, new File(files, "save_import"));
                        msg = "Imported " + n + " save file" + (n == 1 ? "" : "s") + ". Close the game and start it again to load it (your current save is kept as a backup)."; }
                } else {
                    String name = displayName(a, u).replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9 ._()\\[\\]-]", "_").trim();
                    if (name.isEmpty() || name.startsWith(".") || name.startsWith("-")) name = "mod";
                    File dest = new File(new File(files, "mods"), name);
                    try (InputStream in = a.getContentResolver().openInputStream(u)) { int n = installMod(in, dest);
                        msg = "Mod \"" + name + "\" installed (" + n + " files). It is used from the next start."; }
                }
            } catch (IOException | SecurityException e) { msg = "Could not " + (req == REQ_EXPORT ? "export the save" : req == REQ_IMPORT ? "import the save" : "install the mod") + ": " + e.getMessage(); }
            final String m = msg; a.runOnUiThread(() -> Toast.makeText(a, m, Toast.LENGTH_LONG).show());
        }).start();
        return true;
    }
    private static String displayName(Activity a, Uri u) {
        try (android.database.Cursor c = a.getContentResolver().query(u, new String[] { android.provider.OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0); } catch (Exception e) { }
        return "mod";
    }
    private static int zipTree(File root, File dir, ZipOutputStream z) throws IOException {
        File[] cs = dir.listFiles(); if (cs == null) throw new IOException("could not read " + dir); int n = 0;
        for (File f : cs) { File cf = f.getCanonicalFile(); if (!f.getAbsolutePath().equals(cf.getPath())) continue;   // (links stay out)
            if (f.isDirectory()) { n += zipTree(root, f, z); continue; }
            z.putNextEntry(new ZipEntry(root.toURI().relativize(f.toURI()).getPath()));
            try (InputStream in = new java.io.FileInputStream(f)) { byte[] b = new byte[1 << 16]; int k; while ((k = in.read(b)) > 0) z.write(b, 0, k); }
            z.closeEntry(); n++; }
        return n;
    }
    // A mod zip into mods/<name>/ with romfs/ and exefs/ at its top, whichever way the zip holds them: at its root, inside one folder (the mod's name), or the Atmosphere
    // layout (atmosphere/contents/<title ID>/romfs); loose .pchtxt files go to exefs/. Entries are checked to stay inside the folder; at most 8 GB
    static int installMod(InputStream source, File dest) throws IOException {
        File tmp = new File(dest.getParentFile(), "." + dest.getName() + ".part"); deleteTree(tmp); tmp.mkdirs();
        int n = 0; long total = 0; String root = tmp.getCanonicalPath() + File.separator;
        try (ZipInputStream z = new ZipInputStream(source)) { ZipEntry e; byte[] b = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                String p = e.getName().replace('\\', '/'); String rel = modPath(p); if (rel == null || e.isDirectory()) continue;
                File out = new File(tmp, rel); if (!out.getCanonicalPath().startsWith(root)) throw new IOException("the zip has a path outside its folder: " + p);
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) { int k; while ((k = z.read(b)) > 0) { total += k; if (total > (8L << 30)) throw new IOException("the mod is larger than 8 GB"); o.write(b, 0, k); } }
                n++;
            } }
        if (n == 0) { deleteTree(tmp); throw new IOException("no romfs, exefs or .pchtxt files in it"); }
        deleteTree(dest); if (!tmp.renameTo(dest)) throw new IOException("could not move it into place");
        return n;
    }
    // the path inside the mod folder for a zip entry: from its romfs/ or exefs/ part on; a .pchtxt elsewhere goes to exefs/; anything else is left out
    static String modPath(String p) {
        String[] parts = p.split("/");
        for (int i = 0; i < parts.length - 1; i++) { String q = parts[i].toLowerCase(java.util.Locale.ROOT);
            if (q.equals("romfs") || q.equals("exefs")) { StringBuilder sb = new StringBuilder(q);
                for (int j = i + 1; j < parts.length; j++) { if (parts[j].equals("..")) return null; sb.append('/').append(parts[j]); } return sb.toString(); } }
        String last = parts[parts.length - 1];
        return last.toLowerCase(java.util.Locale.ROOT).endsWith(".pchtxt") && !last.contains("..") ? "exefs/" + last : null;
    }
    private static void deleteTree(File f) { File[] cs = f.listFiles(); if (cs != null) for (File c : cs) deleteTree(c); f.delete(); }
}
