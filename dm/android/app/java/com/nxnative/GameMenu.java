// The in-game settings: an Android dialog over the game, opened by the touch controls' menu button (touch_ui.c), by holding − and + together on a controller for 1 s,
// or by holding − for 3 s, which goes straight to the controller buttons (android_main.c front_open_menu). Every change is written to env.txt (GameSettings) for the
// next start; the picture, the touch controls, the controller buttons, rumble and the performance overlay also change at once, the rest say they apply from the next start.
// Every text goes through T.t (the player's language)
package com.nxnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.view.KeyEvent;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class GameMenu {
    private GameMenu() { }
    private static native boolean nativeSetting(String key, String value);
    private static native void nativeEditTouch();
    private static boolean open;   // one menu at a time (UI thread)

    // android_main.c: what 0 = the menu, 1 = the controller buttons
    public static void show(Activity a, int what) {
        a.runOnUiThread(() -> { if (open) return; if (what == 1) remap(a); else menu(a); });
    }
    private static String t(Activity a, String en, Object... args) { return T.t(a, en, args); }

    private static void set(Activity a, String key, String value) {
        String was = GameSettings.get(a, key);
        GameSettings.set(a, key, value);
        boolean now;
        if (key.equals("DM_PERF_OVERLAY")) { if (a instanceof GameActivity) ((GameActivity) a).perfOverlay(value.equals("on")); now = true; }
        else if (key.equals("DM_LANG")) now = true;
        else if (key.equals("DM_RUMBLE")) now = !value.equals("off") && !was.equals("off");   // (controller / phone: at once; off and back on: the game's motors, from the next start)
        else { try { now = nativeSetting(key, value); } catch (UnsatisfiedLinkError e) { now = false; } }
        if (!now) Toast.makeText(a, t(a, "Saved: applies the next time you start the game."), Toast.LENGTH_SHORT).show();
    }
    private static String label(String[][] opts, String cur) { for (String[] o : opts) if (o[0].equals(cur)) return o[1]; return opts[0][1]; }

    private static String[][] graphics(Activity a) {
        String[] p = GameSettings.profile(a); String rec = t(a, p[2].equals("high") ? "High" : p[2].equals("auto") ? "Adaptive" : "Low");
        return new String[][] { { "", t(a, "Auto: this phone gets %s (%s)", rec, p[1]) }, { "high", t(a, "High: TV mode, 1080p, full effects") },
                                { "low", t(a, "Low: handheld mode, 720p, lighter") }, { "auto", t(a, "Adaptive: High, Low while the phone is hot") } };
    }
    private static String[][] textures(Activity a) {
        String[] p = GameSettings.profile(a); String rec = t(a, p[3].equals("half") ? "Half" : "Full");
        return new String[][] { { "", t(a, "Auto: this phone gets %s (%s)", rec, p[1]) }, { "full", t(a, "Full: the game's textures as they are") },
                                { "half", t(a, "Half: large textures at half size, less memory and GPU work") } };
    }
    private static String[][] picture(Activity a) {
        return new String[][] { { "", t(a, "16:9, as the game is made") }, { "fill", t(a, "Fill the screen (stretched)") }, { "20:9", t(a, "20:9, for a widescreen mod") } };
    }
    private static String[][] scaler(Activity a) {
        String rec = GameSettings.profile(a)[6].equals("sgsr") ? "SGSR" : t(a, "Sharp");
        return new String[][] { { "", t(a, "Auto: this phone gets %s", rec) }, { "sharp", t(a, "Sharp: the picture upscaled with sharpening (lighter settings keep their detail)") },
                                { "sgsr", t(a, "Snapdragon Game Super Resolution (Qualcomm's upscaler)") }, { "smooth", t(a, "Smooth: a plain stretch") } };
    }
    // GPUs without BC textures (Mali, Xclipse, PowerVR): Auto converts them to formats the GPU reads, as small as on the Switch; Full decodes them (4-8 times the memory)
    private static String[][] texfmt(Activity a) {
        return new String[][] { { "", t(a, "Auto: compact on GPUs without BC (Mali, Xclipse, PowerVR), converted once and kept") },
                                { "off", t(a, "Full decode: more memory, no conversion (Snapdragon is not affected either way)") } };
    }
    private static String[][] fps(Activity a) {
        String[] p = GameSettings.profile(a);
        return new String[][] { { "", t(a, "Auto: this phone gets %s fps (%s)", p[5], p[1]) }, { "30", t(a, "30 fps, as the game is made") }, { "60", t(a, "60 fps (experimental: needs a fast phone)") } };
    }
    private static String[][] pad(Activity a) { return new String[][] { { "", t(a, "By position, as on a Switch pad") }, { "labels", t(a, "By label (the button marked A is A)") } }; }
    private static String[][] rumble(Activity a) {
        return new String[][] { { "", t(a, "On: the controller's motor, else the phone's") }, { "pad", t(a, "Controllers only") }, { "off", t(a, "Off") } };
    }
    private static String[][] perf(Activity a) { return new String[][] { { "", t(a, "Off") }, { "on", t(a, "On: fps, temperature, GPU, memory, battery") } }; }
    private static String[][] langs(Activity a) { String[][] l = T.LANGS.clone(); l[0] = new String[] { "", t(a, "Auto (the phone's language)") }; return l; }

    private static void menu(Activity a) {
        List<String> rows = new ArrayList<>(); List<Runnable> acts = new ArrayList<>();
        row(a, rows, acts, "Picture", picture(a), "DM_ASPECT");
        row(a, rows, acts, "Upscaling", scaler(a), "DM_SCALER");
        row(a, rows, acts, "Graphics", graphics(a), "DM_GRAPHICS");
        row(a, rows, acts, "Textures", textures(a), "DM_VK_TEX_QUALITY");
        row(a, rows, acts, "Texture format", texfmt(a), "DM_VK_BC_TRANSCODE");
        if (GameSettings.has60(a)) row(a, rows, acts, "Frame rate", fps(a), "DM_FPS_TARGET");
        row(a, rows, acts, "Controller buttons", pad(a), "DM_PAD_LAYOUT");
        row(a, rows, acts, "Rumble", rumble(a), "DM_RUMBLE");
        row(a, rows, acts, "Performance overlay", perf(a), "DM_PERF_OVERLAY");
        rows.add(t(a, "Graphics driver") + ": " + DriverPackages.label(DriverPackages.current(a))); acts.add(() -> driverMenu(a));
        rows.add(t(a, "Remap controller buttons (or hold − for 3 s)")); acts.add(() -> remap(a));
        rows.add(t(a, "Touch controls: look")); acts.add(() -> touchLook(a));
        rows.add(t(a, "Move the touch controls")); acts.add(() -> {
            try { nativeEditTouch(); } catch (UnsatisfiedLinkError e) { }
            Toast.makeText(a, t(a, "Drag a control to move it, pinch to resize, double-tap to hide. Hold − for 2 s to save (hold + to reset)."), Toast.LENGTH_LONG).show(); });
        String room = GameSettings.get(a, "DM_RELAY_ROOM");
        rows.add(t(a, "Online room") + ": " + (room.isEmpty() ? t(a, "off (local play on this Wi-Fi)") : room)); acts.add(() -> onlineRoom(a));
        rows.add(t(a, "Mods (%d)", mods(a).size())); acts.add(() -> modList(a));
        rows.add(t(a, "Import a mod (.zip)")); acts.add(() -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_MOD));
        rows.add(t(a, "Export save data")); acts.add(() -> pick(a, Intent.ACTION_CREATE_DOCUMENT, GameSettings.game(a)[2] + "-save.zip", REQ_EXPORT));
        rows.add(t(a, "Import save data")); acts.add(() -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_IMPORT));
        row(a, rows, acts, "Language", langs(a), "DM_LANG");
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "%s settings", GameSettings.game(a)[1])).setItems(rows.toArray(new String[0]), (d, w) -> { open = false; acts.get(w).run(); })
            .setNegativeButton(t(a, "Back to the game"), null).setOnDismissListener(d -> open = false).show();
    }
    private static void row(Activity a, List<String> rows, List<Runnable> acts, String title, String[][] opts, String key) {
        rows.add(t(a, title) + ": " + label(opts, GameSettings.get(a, key))); acts.add(() -> choose(a, title, opts, key));
    }
    // a choice: picking one saves it and comes back to the menu (built again: a new language shows at once)
    private static void choose(Activity a, String title, String[][] opts, String key) {
        String cur = GameSettings.get(a, key); int sel = 0; String[] names = new String[opts.length];
        for (int k = 0; k < opts.length; k++) { names[k] = opts[k][1]; if (opts[k][0].equals(cur)) sel = k; }
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, title)).setSingleChoiceItems(names, sel, (d, w) -> { set(a, key, opts[w][0]); d.dismiss(); })
            .setNegativeButton(t(a, "Back"), null).setOnDismissListener(d -> { open = false; menu(a); }).show();
    }
    private static void touchLook(Activity a) {
        LinearLayout root = new LinearLayout(a); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(48, 24, 48, 0);
        slider(a, root, t(a, "Opacity"), "DM_TOUCH_OPACITY", 20, 100, 100, v -> v + " %");
        slider(a, root, t(a, "Fade after"), "DM_TOUCH_FADE", 0, 10, 3, v -> v == 0 ? t(a, "never fade") : t(a, "%d s without a touch", v));
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Touch controls")).setView(root).setPositiveButton(t(a, "Done"), null).setOnDismissListener(d -> { open = false; menu(a); }).show();
    }
    private static void slider(Activity a, LinearLayout root, String label, String key, int min, int max, int def, java.util.function.IntFunction<String> fmt) {
        TextView tv = new TextView(a); tv.setPadding(0, 16, 0, 0); root.addView(tv);
        SeekBar sb = new SeekBar(a); sb.setMax(max - min);
        int v; try { v = Integer.parseInt(GameSettings.get(a, key)); } catch (NumberFormatException e) { v = def; }
        v = Math.max(min, Math.min(max, v)); sb.setProgress(v - min); tv.setText(label + ": " + fmt.apply(v));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int p, boolean user) { tv.setText(label + ": " + fmt.apply(p + min)); if (user) set(a, key, String.valueOf(p + min)); }
            public void onStartTrackingTouch(SeekBar b) { }
            public void onStopTrackingTouch(SeekBar b) { }
        });
        root.addView(sb);
    }

    // Remapping (PadMapping.java, DM_PAD_MAPPING): each Switch button with the key it is on; choosing one waits for a press on the controller, Reset clears the remapping
    private static void remap(Activity a) {
        Map<String, Integer> map = PadMapping.parse(GameSettings.get(a, "DM_PAD_MAPPING"));
        String[] rows = new String[PadMapping.BUTTONS.length];
        for (int i = 0; i < rows.length; i++) { Integer kc = map.get(PadMapping.BUTTONS[i][0]); rows[i] = PadMapping.BUTTONS[i][1] + ": " + (kc != null ? PadMapping.keyName(kc) : t(a, "default")); }
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Remap controller buttons")).setItems(rows, (d, which) -> { open = false; captureKey(a, map, which); })
            .setNeutralButton(t(a, "Reset all"), (d, w) -> { set(a, "DM_PAD_MAPPING", ""); Toast.makeText(a, t(a, "Controller buttons reset."), Toast.LENGTH_SHORT).show(); })
            .setNegativeButton(t(a, "Done"), null).setOnDismissListener(d -> open = false).show();
    }
    private static void captureKey(Activity a, Map<String, Integer> map, int which) {
        String[] btn = PadMapping.BUTTONS[which];
        AlertDialog d = new AlertDialog.Builder(a).setTitle(btn[1]).setMessage(t(a, "Press the controller button for %s.", btn[1]))
            .setNegativeButton(t(a, "Cancel"), (x, w) -> remap(a)).create();
        d.setOnKeyListener((x, kc, e) -> {
            if (e.getAction() != KeyEvent.ACTION_DOWN || !PadMapping.isPadKey(e)) return false;
            map.values().removeIf(v -> v == kc);   // a key drives one Switch button
            map.put(btn[0], kc); set(a, "DM_PAD_MAPPING", PadMapping.serialize(map));
            d.dismiss(); remap(a); return true;
        });
        d.show();
    }

    // ---------- mods ----------
    // <files>/mods/<mod>/romfs (files that replace the game's) and <mod>/exefs/*.pchtxt (code patches for this game version), as the Switch emulators lay them out;
    // loose .pchtxt files in mods/ work too. Ticking one on or off renames its folder (a leading "-" switches it off, android_main.c / loader.c); applies at the next start.
    // Load order: mods/order.txt, one name a line (without the "-"), first = wins where two mods replace the same file (android_main.c mods_romfs); mods it does not list
    // come after, by name. Notes: <mod>/notes.txt (the mod's own readme when the zip has one, or what the player writes)
    private static File modsDir(Activity a) { return new File(GameSettings.dir(a), "mods"); }
    private static String plain(File f) { String n = f.getName(); return n.startsWith("-") ? n.substring(1) : n; }
    private static List<String> order(Activity a) {
        List<String> o = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(modsDir(a), "order.txt")))) { String l; while ((l = r.readLine()) != null) if (!l.trim().isEmpty()) o.add(l.trim()); } catch (IOException e) { }
        return o;
    }
    private static void saveOrder(Activity a, List<File> ms) {
        try (FileWriter w = new FileWriter(new File(modsDir(a), "order.txt"))) { for (File f : ms) w.write(plain(f) + "\n"); } catch (IOException e) { }
    }
    private static List<File> mods(Activity a) {
        File[] fs = modsDir(a).listFiles(); List<File> all = new ArrayList<>(); if (fs == null) return all;
        Arrays.sort(fs); for (File f : fs) if (f.isDirectory() && !f.getName().startsWith(".") && !f.getName().equals("romfs")) all.add(f);
        List<File> out = new ArrayList<>();
        for (String n : order(a)) for (File f : all) if (plain(f).equals(n) && !out.contains(f)) out.add(f);
        for (File f : all) if (!out.contains(f)) out.add(f);
        return out;
    }
    private static void files(File root, File d, Set<String> out) {
        File[] fs = d.listFiles(); if (fs == null) return;
        for (File f : fs) { if (f.isDirectory()) files(root, f, out); else out.add(root.toURI().relativize(f.toURI()).getPath().toLowerCase(java.util.Locale.ROOT)); }
    }
    // for each mod that is on: the other mods that are on and replace some of the same game files, with how many
    private static Map<File, Map<File, Integer>> conflicts(List<File> ms) {
        Map<File, Set<String>> fs = new HashMap<>();
        for (File m : ms) if (!m.getName().startsWith("-")) { Set<String> s = new HashSet<>(); File r = new File(m, "romfs"); files(r, r, s); fs.put(m, s); }
        Map<File, Map<File, Integer>> out = new HashMap<>();
        for (File x : fs.keySet()) for (File y : fs.keySet()) {
            if (x == y) continue; int n = 0; for (String p : fs.get(x)) if (fs.get(y).contains(p)) n++;
            if (n > 0) out.computeIfAbsent(x, k -> new HashMap<>()).put(y, n);
        }
        return out;
    }
    private static void modList(Activity a) {
        List<File> ms = mods(a);
        String where = t(a, "Put each mod in its own folder in Android/data/%s/files/mods/ — <mod>/romfs for files, <mod>/exefs for .pchtxt code patches (they must be made for %s 1.3.2).",
                         a.getPackageName(), GameSettings.game(a)[1]);
        open = true;
        boolean[] next = { false };   // (going on to the load order: not back to the menu)
        AlertDialog.Builder b = new AlertDialog.Builder(a).setTitle(t(a, "Mods")).setNegativeButton(t(a, "Back"), null).setOnDismissListener(d -> { open = false; if (!next[0]) menu(a); });
        if (ms.isEmpty()) { b.setMessage(t(a, "No mods yet.") + " " + where).show(); return; }
        Map<File, Map<File, Integer>> cf = conflicts(ms);
        String[] names = new String[ms.size()]; boolean[] on = new boolean[ms.size()];
        for (int i = 0; i < names.length; i++) { File f = ms.get(i); on[i] = !f.getName().startsWith("-"); names[i] = (i + 1) + ". " + plain(f) + (cf.containsKey(f) ? "  ⚠" : ""); }
        b.setMultiChoiceItems(names, on, (d, i, checked) -> {
            File f = ms.get(i), to = new File(f.getParentFile(), checked ? plain(f) : "-" + plain(f));
            if (!f.getName().equals(to.getName()) && f.renameTo(to)) { ms.set(i, to); Toast.makeText(a, t(a, checked ? "On: applies the next time you start the game." : "Off: applies the next time you start the game."), Toast.LENGTH_SHORT).show(); }
        }).setPositiveButton(t(a, "Order and notes"), (d, w) -> { next[0] = true; modOrder(a); })
          .setNeutralButton(t(a, "Where do mods go?"), (d, w) -> Toast.makeText(a, where, Toast.LENGTH_LONG).show()).show();
    }
    // the load order: tap a mod for its notes, the files it shares with others, moving it up or down, or deleting it
    private static void modOrder(Activity a) {
        List<File> ms = mods(a); Map<File, Map<File, Integer>> cf = conflicts(ms);
        String[] rows = new String[ms.size()];
        for (int i = 0; i < rows.length; i++) { File f = ms.get(i); Map<File, Integer> c = cf.get(f);
            rows[i] = (i + 1) + ". " + plain(f) + (f.getName().startsWith("-") ? " (" + t(a, "off") + ")" : "") + (c != null ? "  ⚠ " + t(a, "shares files with %d other mod(s)", c.size()) : ""); }
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Load order: higher wins")).setItems(rows, (d, i) -> modDetail(a, ms, i, cf.get(ms.get(i))))
            .setNegativeButton(t(a, "Back"), (d, w) -> modList(a)).setOnDismissListener(d -> open = false).show();
    }
    private static void modDetail(Activity a, List<File> ms, int i, Map<File, Integer> shared) {
        File m = ms.get(i), notes = new File(m, "notes.txt");
        StringBuilder sb = new StringBuilder();
        String n = ""; try { n = new String(java.nio.file.Files.readAllBytes(notes.toPath()), java.nio.charset.StandardCharsets.UTF_8).trim(); } catch (IOException e) { }
        sb.append(n.isEmpty() ? t(a, "No notes.") : n);
        if (shared != null) for (Map.Entry<File, Integer> e : shared.entrySet()) {
            boolean wins = ms.indexOf(m) < ms.indexOf(e.getKey());
            sb.append("\n\n⚠ ").append(t(a, wins ? "Replaces %d of the same files as %s: this mod's are used (it is higher)." : "Replaces %d of the same files as %s: that mod's are used (it is higher).", e.getValue(), plain(e.getKey())));
        }
        LinearLayout root = new LinearLayout(a); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(48, 24, 48, 0);
        TextView tv = new TextView(a); tv.setText(sb.toString()); tv.setTextIsSelectable(true); root.addView(tv);
        ScrollView sv = new ScrollView(a); sv.addView(root);
        open = true;
        AlertDialog[] dlg = new AlertDialog[1];
        LinearLayout btns = new LinearLayout(a); btns.setOrientation(LinearLayout.VERTICAL); root.addView(btns);
        java.util.function.BiConsumer<String, Runnable> btn = (label, r) -> { Button b = new Button(a); b.setText(label); b.setOnClickListener(v -> { dlg[0].dismiss(); r.run(); }); btns.addView(b); };
        if (i > 0) btn.accept(t(a, "Move up"), () -> { java.util.Collections.swap(ms, i, i - 1); saveOrder(a, ms); modOrder(a); });
        if (i < ms.size() - 1) btn.accept(t(a, "Move down"), () -> { java.util.Collections.swap(ms, i, i + 1); saveOrder(a, ms); modOrder(a); });
        btn.accept(t(a, "Edit notes"), () -> editNotes(a, notes));
        btn.accept(t(a, "Delete this mod"), () -> new AlertDialog.Builder(a).setTitle(plain(m)).setMessage(t(a, "Delete this mod from the phone?"))
            .setPositiveButton(t(a, "Delete"), (d, w) -> { deleteTree(m); ms.remove(m); saveOrder(a, ms); Toast.makeText(a, t(a, "Deleted: applies the next time you start the game."), Toast.LENGTH_SHORT).show(); modOrder(a); })
            .setNegativeButton(t(a, "Cancel"), (d, w) -> modOrder(a)).show());
        dlg[0] = new AlertDialog.Builder(a).setTitle(plain(m)).setView(sv).setNegativeButton(t(a, "Back"), (d, w) -> modOrder(a)).setOnDismissListener(d -> open = false).create();
        dlg[0].show();
    }
    private static void editNotes(Activity a, File notes) {
        EditText e = new EditText(a); e.setMinLines(4);
        try { e.setText(new String(java.nio.file.Files.readAllBytes(notes.toPath()), java.nio.charset.StandardCharsets.UTF_8)); } catch (IOException x) { }
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Notes")).setView(e)
            .setPositiveButton(t(a, "Save"), (d, w) -> { try (FileWriter fw = new FileWriter(notes)) { fw.write(e.getText().toString()); } catch (IOException x) { } })
            .setNegativeButton(t(a, "Cancel"), null).setOnDismissListener(d -> modOrder(a)).show();
    }

    // ---------- files: save data out and in, mods in (the system file picker; GameActivity.onActivityResult hands the answer here) ----------
    static final int REQ_EXPORT = 101, REQ_IMPORT = 102, REQ_MOD = 103, REQ_DRIVER = 104;
    // the graphics driver: the phone's own, an installed package (Turnip, PanVK...), a new package from the file picker, or the releases page for this GPU; from the next start
    private static void driverMenu(Activity a) {
        List<String[]> pk = DriverPackages.installed(a); String cur = DriverPackages.current(a); String[] src = DriverPackages.source(a);
        List<String> rows = new ArrayList<>(); List<Runnable> acts = new ArrayList<>();
        rows.add((cur.isEmpty() ? "✓ " : "") + t(a, "The phone's own driver")); acts.add(() -> { DriverPackages.useSystem(a); Toast.makeText(a, t(a, "The phone's own driver: from the next start."), Toast.LENGTH_LONG).show(); });
        for (String[] p : pk) { rows.add((p[1].equals(cur) ? "✓ " : "") + p[0]); acts.add(() -> { DriverPackages.use(a, new File(p[1])); Toast.makeText(a, t(a, "%s: from the next start.", p[0]), Toast.LENGTH_LONG).show(); }); }
        rows.add(t(a, "Install a driver package (.zip)")); acts.add(() -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_DRIVER));
        if (DriverPackages.isTurnip(cur)) { rows.add(t(a, "Turnip options")); acts.add(() -> turnipOptions(a)); }
        if (src != null) { rows.add(src[0]); acts.add(() -> a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(src[1])))); }
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Graphics driver")).setItems(rows.toArray(new String[0]), (dd, w) -> { open = false; acts.get(w).run(); })
            .setNegativeButton(t(a, "Back"), null).setOnDismissListener(dd -> open = false).show();
    }
    private static void pick(Activity a, String action, String name, int req) {
        Intent i = new Intent(action); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip");
        if (name != null) i.putExtra(Intent.EXTRA_TITLE, name);
        if (req == REQ_EXPORT) Toast.makeText(a, t(a, "Save in the game first: the export is the last save the game wrote."), Toast.LENGTH_LONG).show();
        a.startActivityForResult(i, req);
    }
    private static void turnipOptions(Activity a) {
        String[][] o = DriverPackages.TURNIP_OPTIONS; java.util.Set<String> on = DriverPackages.turnipOptions(a);
        String[] names = new String[o.length]; boolean[] chk = new boolean[o.length];
        for (int i = 0; i < o.length; i++) { names[i] = o[i][1]; chk[i] = on.contains(o[i][0]); }
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Turnip options (from the next start)")).setMultiChoiceItems(names, chk, (d, i, v) -> { if (v) on.add(o[i][0]); else on.remove(o[i][0]); DriverPackages.setTurnipOptions(a, on); })
            .setPositiveButton(t(a, "Done"), null).setOnDismissListener(d -> open = false).show();
    }
    static boolean onResult(Activity a, int req, int res, Intent data) {
        if (req != REQ_EXPORT && req != REQ_IMPORT && req != REQ_MOD && req != REQ_DRIVER) return false;
        Uri u = res == Activity.RESULT_OK && data != null ? data.getData() : null; if (u == null) return true;
        File files = GameSettings.dir(a);
        new Thread(() -> {
            String msg;
            try {
                if (req == REQ_DRIVER) msg = DriverPackages.install(a, u, displayName(a, u));
                else if (req == REQ_EXPORT) {
                    try (OutputStream o = a.getContentResolver().openOutputStream(u, "wt"); ZipOutputStream z = new ZipOutputStream(o)) {
                        File save = new File(files, "save").getCanonicalFile(); int n = zipTree(save, save, z); msg = t(a, "Exported %d save file(s).", n); }
                } else if (req == REQ_IMPORT) {
                    try (InputStream in = a.getContentResolver().openInputStream(u)) {
                        int n = SaveImport.install(in, new File(files, "save_import"));
                        msg = t(a, "Imported %d save file(s). Close the game and start it again to load it (your current save is kept as a backup).", n); }
                } else {
                    String name = displayName(a, u).replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9 ._()\\[\\]-]", "_").trim();
                    if (name.isEmpty() || name.startsWith(".") || name.startsWith("-")) name = "mod";
                    File dest = new File(new File(files, "mods"), name);
                    try (InputStream in = a.getContentResolver().openInputStream(u)) { int n = installMod(in, dest);
                        msg = t(a, "Mod \"%s\" installed (%d files). It is used from the next start.", name, n); }
                }
            } catch (IOException | SecurityException e) {
                msg = t(a, req == REQ_EXPORT ? "Could not export the save: %s" : req == REQ_IMPORT ? "Could not import the save: %s" : "Could not install the mod: %s", e.getMessage()); }
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
    // layout (atmosphere/contents/<title ID>/romfs); loose .pchtxt files go to exefs/. The mod's own readme (readme*, info*, notes*: .txt / .md) becomes its notes.txt.
    // Entries are checked to stay inside the folder; at most 8 GB
    static int installMod(InputStream source, File dest) throws IOException {
        File tmp = new File(dest.getParentFile(), "." + dest.getName() + ".part"); deleteTree(tmp); tmp.mkdirs();
        int n = 0; long total = 0; String root = tmp.getCanonicalPath() + File.separator; boolean noted = false;
        try (ZipInputStream z = new ZipInputStream(source)) { ZipEntry e; byte[] b = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                String p = e.getName().replace('\\', '/'); String rel = modPath(p);
                if (rel == null && !e.isDirectory() && !noted && isReadme(p)) {   // (the first readme, up to 64 KB)
                    java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(); int k; while ((k = z.read(b)) > 0 && bo.size() < (64 << 10)) bo.write(b, 0, k);
                    try (OutputStream o = new FileOutputStream(new File(tmp, "notes.txt"))) { o.write(bo.toByteArray()); } noted = true; continue;
                }
                if (rel == null || e.isDirectory()) continue;
                File out = new File(tmp, rel); if (!out.getCanonicalPath().startsWith(root)) throw new IOException("the zip has a path outside its folder: " + p);
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) { int k; while ((k = z.read(b)) > 0) { total += k; if (total > (8L << 30)) throw new IOException("the mod is larger than 8 GB"); o.write(b, 0, k); } }
                n++;
            } }
        if (n == 0) { deleteTree(tmp); throw new IOException("no romfs, exefs or .pchtxt files in it"); }
        deleteTree(dest); if (!tmp.renameTo(dest)) throw new IOException("could not move it into place");
        return n;
    }
    static boolean isReadme(String p) {
        String last = p.substring(p.lastIndexOf('/') + 1).toLowerCase(java.util.Locale.ROOT);
        return (last.startsWith("readme") || last.startsWith("info") || last.startsWith("notes")) && (last.endsWith(".txt") || last.endsWith(".md")) && !p.contains("..");
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

    // Online room (hle_relay.c, dm/server): local play with players elsewhere through a room server someone hosts (their own Railway service, or any PC); everyone enters the
    // same server address and room code. Applies from the next start; mods stay off for local play, as on Wi-Fi
    private static void onlineRoom(Activity a) {
        LinearLayout root = new LinearLayout(a); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(48, 24, 48, 0);
        TextView h = new TextView(a); h.setText(t(a, "Play the game's local link battles, trades and Max Raids with friends anywhere: everyone enters the same room server and room code. Host your own server (dm/server in the project: Railway or any PC). Mods must be off.")); root.addView(h);
        EditText srv = new EditText(a); srv.setHint(t(a, "server (address:port)")); srv.setSingleLine(); srv.setText(GameSettings.get(a, "DM_RELAY")); root.addView(srv);
        EditText code = new EditText(a); code.setHint(t(a, "room code")); code.setSingleLine(); code.setText(GameSettings.get(a, "DM_RELAY_ROOM")); root.addView(code);
        open = true;
        new AlertDialog.Builder(a).setTitle(t(a, "Online room")).setView(root)
            .setPositiveButton(t(a, "Join"), (d, w) -> { String sv = srv.getText().toString().trim(), c = code.getText().toString().trim();
                if (sv.isEmpty() || c.isEmpty()) { Toast.makeText(a, t(a, "Enter both the server and the room code."), Toast.LENGTH_LONG).show(); return; }
                GameSettings.set(a, "DM_RELAY", sv); GameSettings.set(a, "DM_RELAY_ROOM", c.replaceAll("\\s+", ""));
                Toast.makeText(a, t(a, "Saved: close the game and start it again, then use the game's local link options."), Toast.LENGTH_LONG).show(); })
            .setNeutralButton(t(a, "Leave room"), (d, w) -> { GameSettings.set(a, "DM_RELAY_ROOM", ""); Toast.makeText(a, t(a, "Local play is back on this Wi-Fi from the next start."), Toast.LENGTH_LONG).show(); })
            .setNegativeButton(t(a, "Back"), null).setOnDismissListener(d -> { open = false; }).show();
    }
}
