// The app's first screen: the player sets the game up from their own dump, then plays.
//   Game files → Choose: the .nsp / .xci of the base game, the update and any DLC (several at once). Read in place (all-files access) when they are on the phone's
//   storage, so nothing large is copied.
//   Keys → Choose: prod.keys (and title.keys when the dump needs it), copied into the app's private storage (getFilesDir()/keys): never uploaded, never in the APK.
//   Set up: runs dm/android/tools/setup_game.py with the Python the app ships (libpython_exe.so + the standard library unpacked from assets/python.zip), the
//   unpacker / shader translator / spirv-cross the app ships as native executables. The game ends up in the app's external files folder, where the native
//   activity (android_main.c) reads it. Once set up, the app starts straight into the game.
//   Save data can be exported to a ZIP, edited elsewhere, and imported here; the next game start swaps it in and keeps the previous save as a backup.
//   Shaders the game builds in memory (Shield: 45) are saved by the game the first time it makes them (nvn_vk.c mtl_shader_seen → nvn_capture/); the next start
//   translates them first (setup_game.py --captures, a few seconds), so they draw from then on.
//   The game's own icon, from the player's dump (icon.jpg, written by the setup), goes on a home screen shortcut, the recent-apps card and this screen; the app's
//   launcher icon itself is fixed by the APK, which carries no game artwork.
package com.nxnative;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.*;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Consumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class SetupActivity extends Activity {
    private static final int PICK_GAME = 1, PICK_KEYS = 2, PICK_DRIVER = 3, PICK_SAVE_EXPORT = 4, PICK_SAVE_IMPORT = 5;
    private TextView driverText;
    private final List<String> games = new ArrayList<>();
    private final List<File> importedGames = new ArrayList<>();
    private static final class PickedGame {
        final Uri uri; final String name;
        PickedGame(Uri uri, String name) { this.uri = uri; this.name = name; }
    }
    private TextView status, gameText, keysText; private ProgressBar bar; private Button setup, play, shortcut, saveExport, saveImport; private ImageView logo;
    private volatile boolean running;

    private File gameDir() { return getExternalFilesDir(null); }
    private File keysDir() { File d = new File(getFilesDir(), "keys"); d.mkdirs(); return d; }
    private boolean ready() { return new File(gameDir(), "ready").exists() && new File(gameDir(), "shaders.pak").exists(); }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        boolean asked = getIntent().getBooleanExtra("setup", false);
        if (ready()) setupShortcut();
        if (ready() && !asked) { gameIcon(); if (pendingCaptures().isEmpty()) { startGame(); return; } }
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(48, 48, 48, 48); root.setGravity(Gravity.CENTER_HORIZONTAL);
        logo = new ImageView(this); logo.setVisibility(android.view.View.GONE); root.addView(logo, new LinearLayout.LayoutParams(320, 320));
        TextView title = new TextView(this); title.setText("Pokémon Shield"); title.setTextSize(28); root.addView(title);
        TextView note = new TextView(this); note.setText("Set the game up from your own dump. No game files or keys come with this app; yours never leave the phone."); note.setPadding(0, 16, 0, 32); root.addView(note);
        gameText = row(root, "Game files", "Choose", v -> pick(PICK_GAME, true));
        keysText = row(root, "Keys", "Choose", v -> pick(PICK_KEYS, true));
        // the Vulkan driver: the phone's own, or an Adreno driver package (a zip with meta.json and the library, e.g. Mesa Turnip) opened through libadrenotools
        driverText = row(root, "Graphics driver", "Choose", v -> pick(PICK_DRIVER, false));
        button(root, "Use the phone's own driver", v -> { envSet("DM_VK_DRIVER", ""); refresh(); });
        settings(root);
        setup = button(root, "Set up the game", v -> startSetup());
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(6); root.addView(bar);
        status = new TextView(this); status.setPadding(0, 16, 0, 16); root.addView(status);
        play = button(root, "Play", v -> startGame()); play.setEnabled(ready());
        saveExport = button(root, "Export save data", v -> chooseSaveExport());
        saveImport = button(root, "Import edited save data", v -> chooseSaveImport());
        shortcut = button(root, "Add Pokémon Shield to the home screen", v -> pinShortcut());
        ScrollView sv = new ScrollView(this); sv.addView(root); setContentView(sv);
        refresh(); gameIcon();
        if (ready() && !asked) startCaptures();
    }
    private TextView row(LinearLayout root, String label, String btn, android.view.View.OnClickListener l) {
        LinearLayout r = new LinearLayout(this); r.setOrientation(LinearLayout.HORIZONTAL); r.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this); t.setText(label); t.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1)); r.addView(t);
        Button bt = new Button(this); bt.setText(btn); bt.setOnClickListener(l); r.addView(bt); root.addView(r);
        TextView sel = new TextView(this); sel.setPadding(0, 0, 0, 24); root.addView(sel); return sel;
    }
    private Button button(LinearLayout root, String text, android.view.View.OnClickListener l) { Button bt = new Button(this); bt.setText(text); bt.setOnClickListener(l); root.addView(bt); return bt; }

    // Settings, read by the game at its next start (env.txt): graphics (the console mode the game renders for) and the touch controls' look
    private void settings(LinearLayout root) {
        TextView h = new TextView(this); h.setText("Settings"); h.setTextSize(20); h.setPadding(0, 32, 0, 8); root.addView(h);
        TextView gl = new TextView(this); gl.setText("Graphics"); root.addView(gl);
        android.widget.RadioGroup rg = new android.widget.RadioGroup(this);
        String[][] modes = { { "high", "High: TV mode, 1080p, full effects" }, { "low", "Low: handheld mode, 720p, lighter (for weaker phones)" },
                             { "auto", "Auto: High, switching to Low while the phone is hot" } };
        String cur = envGet("DM_GRAPHICS"); if (cur.isEmpty()) cur = "high";
        for (int k = 0; k < modes.length; k++) {
            android.widget.RadioButton b = new android.widget.RadioButton(this); b.setId(1000 + k); b.setText(modes[k][1]); rg.addView(b);
            if (modes[k][0].equals(cur)) b.setChecked(true);
        }
        rg.setOnCheckedChangeListener((g, id) -> envSet("DM_GRAPHICS", modes[id - 1000][0]));
        root.addView(rg);
        slider(root, "Touch controls opacity", "DM_TOUCH_OPACITY", 20, 100, 100, v -> v + " %");
        slider(root, "Touch controls fade after", "DM_TOUCH_FADE", 0, 10, 3, v -> v == 0 ? "never fade" : v + " s without a touch");
    }
    private void slider(LinearLayout root, String label, String key, int min, int max, int def, java.util.function.IntFunction<String> fmt) {
        TextView t = new TextView(this); t.setPadding(0, 16, 0, 0); root.addView(t);
        android.widget.SeekBar sb = new android.widget.SeekBar(this); sb.setMax(max - min);
        int v; try { v = Integer.parseInt(envGet(key)); } catch (NumberFormatException e) { v = def; }
        v = Math.max(min, Math.min(max, v)); sb.setProgress(v - min); t.setText(label + ": " + fmt.apply(v));
        sb.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(android.widget.SeekBar b, int p, boolean user) { t.setText(label + ": " + fmt.apply(p + min)); if (user) envSet(key, String.valueOf(p + min)); }
            public void onStartTrackingTouch(android.widget.SeekBar b) { }
            public void onStopTrackingTouch(android.widget.SeekBar b) { }
        });
        root.addView(sb);
    }
    // env.txt in the game folder: KEY=VALUE lines the native side reads at start (android_main.c env_file)
    private File envFile() { return new File(gameDir(), "env.txt"); }
    private String envGet(String key) {
        try (BufferedReader r = new BufferedReader(new FileReader(envFile()))) { String l; while ((l = r.readLine()) != null) if (l.startsWith(key + "=")) return l.substring(key.length() + 1); } catch (IOException e) { }
        return "";
    }
    private void envSet(String key, String value) {
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(envFile()))) { String l; while ((l = r.readLine()) != null) if (!l.startsWith(key + "=")) lines.add(l); } catch (IOException e) { }
        if (!value.isEmpty()) lines.add(key + "=" + value.replace("\n", " "));
        try (FileWriter w = new FileWriter(envFile())) { for (String l : lines) w.write(l + "\n"); } catch (IOException e) { }
    }
    // a driver package into the app's private storage (libadrenotools loads drivers only from there): its meta.json names the library
    private void installDriver(Uri u, String name) {
        File dir = new File(getFilesDir(), "drivers/" + name.replaceAll("[^A-Za-z0-9._-]", "_").replaceAll("\\.zip$", "")); String lib = null;
        try {
            deleteTree(dir); dir.mkdirs();
            try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(getContentResolver().openInputStream(u)))) { ZipEntry e;
                while ((e = z.getNextEntry()) != null) { if (e.isDirectory()) continue; String n = new File(e.getName()).getName();
                    try (OutputStream o = new FileOutputStream(new File(dir, n))) { copy(z, o); } } }
            File meta = new File(dir, "meta.json");
            if (meta.exists()) { StringBuilder sb = new StringBuilder(); try (BufferedReader r = new BufferedReader(new FileReader(meta))) { String l; while ((l = r.readLine()) != null) sb.append(l); }
                java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"]+)\"").matcher(sb); if (m.find()) lib = m.group(1); }
            if (lib == null) { String[] so = dir.list((d, n) -> n.endsWith(".so")); if (so != null && so.length == 1) lib = so[0]; }
            if (lib == null || !new File(dir, lib).exists()) { status.setText(name + " is not a driver package (no meta.json naming its library)"); deleteTree(dir); return; }
            envSet("DM_VK_DRIVER", new File(dir, lib).getPath()); status.setText("Driver " + name + " installed: it is used from the next start.");
        } catch (IOException e) { status.setText("could not read " + name + ": " + e.getMessage()); }
    }
    private static void deleteTree(File f) { File[] k = f.listFiles(); if (k != null) for (File c : k) deleteTree(c); f.delete(); }
    private void refresh() {
        if (driverText != null) { String d = envGet("DM_VK_DRIVER"); driverText.setText(d.isEmpty() ? "the phone's own Vulkan driver" : new File(d).getParentFile().getName() + " (" + new File(d).getName() + ")"); }
        gameText.setText(games.isEmpty() ? "none chosen (.nsp / .xci: base game, update 1.3.2, DLC)" : String.join("\n", games));
        boolean keys = new File(keysDir(), "prod.keys").exists();
        keysText.setText(keys ? "prod.keys" + (new File(keysDir(), "title.keys").exists() ? " + title.keys" : "") + " (kept in the app's private storage)" : "none chosen (prod.keys, and title.keys if needed)");
        setup.setEnabled(!running && keys && !games.isEmpty()); play.setEnabled(!running && ready());
        saveExport.setEnabled(!running && ready());
        saveImport.setEnabled(!running && ready());
        shortcut.setVisibility(iconFile().exists() ? android.view.View.VISIBLE : android.view.View.GONE);
    }
    private void pick(int what, boolean multi) {
        if (running) { status.setText("Please wait for the current setup or file copy to finish."); return; }
        if (what == PICK_GAME && Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {   // read the dump in place: all-files access
            status.setText("Allow access to all files so the app can read your dump where it is (it is several GB; nothing is copied).");
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))); return; }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multi);
        startActivityForResult(i, what);
    }
    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (res != RESULT_OK) return;
        if (data == null) { status.setText("The file picker returned no selected file. Try the phone's Files app and select a file from Downloads or Documents."); return; }
        if (req == PICK_SAVE_EXPORT) { exportSave(data.getData()); return; }
        if (req == PICK_SAVE_IMPORT) { importSave(data.getData()); return; }
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) for (int k = 0; k < data.getClipData().getItemCount(); k++) uris.add(data.getClipData().getItemAt(k).getUri()); else if (data.getData() != null) uris.add(data.getData());
        if (uris.isEmpty()) { status.setText("The file picker did not return a file. Try the phone's Files app and select a file from Downloads or Documents."); return; }
        List<PickedGame> imports = new ArrayList<>();
        for (Uri u : uris) {
            String name = nameOf(u);
            if (req == PICK_DRIVER) { installDriver(u, name); continue; }
            if (req == PICK_KEYS) {
                String to = name.toLowerCase().contains("title") ? "title.keys" : "prod.keys";
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(new File(keysDir(), to))) { copy(in, out); }
                catch (IOException e) { status.setText("could not read " + name + ": " + e.getMessage()); }
            } else {
                String p = pathOf(u);
                if (p == null) imports.add(new PickedGame(u, name));
                else if (!games.contains(p)) games.add(p);
            }
        }
        refresh();
        if (!imports.isEmpty()) {
            importGames(imports);
        }
    }
    private String nameOf(Uri u) { try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) { if (c != null && c.moveToFirst()) return c.getString(0); } catch (Exception e) { } return u.getLastPathSegment(); }
    // Resolve document-provider paths for in-place reads; copying multi-gigabyte game files into the app is not practical.
    private String pathOf(Uri u) {
        if ("file".equals(u.getScheme())) return readableFile(u.getPath());
        if (DocumentsContract.isDocumentUri(this, u)) {
            try {
                String id = DocumentsContract.getDocumentId(u);
                if (id.startsWith("raw:")) return readableFile(id.substring(4));
                if ("com.android.externalstorage.documents".equals(u.getAuthority())) {
                    int sep = id.indexOf(':');
                    if (sep > 0) {
                        String volume = id.substring(0, sep), relative = id.substring(sep + 1);
                        File root = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory() : new File("/storage", volume);
                        return readableFile(new File(root, relative).getPath());
                    }
                }
            } catch (IllegalArgumentException e) { }
        }
        try (Cursor c = getContentResolver().query(u, new String[] { "_data" }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int column = c.getColumnIndex("_data");
                if (column >= 0) return readableFile(c.getString(column));
            }
        } catch (IllegalArgumentException | SecurityException e) { }
        return null;
    }
    private String readableFile(String path) { if (path == null) return null; File f = new File(path); return f.isFile() && f.canRead() ? f.getPath() : null; }
    private static void copy(InputStream in, OutputStream out) throws IOException { byte[] buf = new byte[1 << 16]; int n; while ((n = in.read(buf)) > 0) out.write(buf, 0, n); }

    private long documentSize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
            return c != null && c.moveToFirst() && !c.isNull(0) ? c.getLong(0) : -1;
        } catch (IllegalArgumentException | SecurityException e) { return -1; }
    }
    private void importGames(List<PickedGame> imports) {
        running = true; refresh();
        gameText.setText("Copying " + imports.size() + " selected file" + (imports.size() == 1 ? "" : "s") + " into app storage...");
        status.setText("Copying " + imports.size() + " selected game file" + (imports.size() == 1 ? "" : "s") + " into app storage; large files need extra free space...");
        new Thread(() -> {
            List<File> copied = new ArrayList<>();
            String error = null;
            try {
                File dir = new File(gameDir(), "imported");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create app storage for the selected game file");
                for (PickedGame picked : imports) {
                    File destination = uniqueImportFile(dir, picked.name);
                    long size = documentSize(picked.uri);
                    long available = new StatFs(dir.getPath()).getAvailableBytes();
                    if (size >= 0 && available < size + (512L << 20))
                        throw new IOException("not enough free space to copy " + picked.name + "; select it from phone storage where possible or free at least " + size / (1L << 20) + " MB");
                    runOnUiThread(() -> status.setText("Copying " + picked.name + " (" + (size >= 0 ? size / (1L << 20) + " MB" : "size unknown") + ") into app storage..."));
                    try (InputStream in = getContentResolver().openInputStream(picked.uri);
                         OutputStream out = new BufferedOutputStream(new FileOutputStream(destination))) {
                        if (in == null) throw new IOException("the file provider could not open " + picked.name);
                        byte[] buffer = new byte[1 << 20]; int n;
                        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                    } catch (IOException | SecurityException e) { destination.delete(); throw e; }
                    if (!destination.isFile() || destination.length() == 0) { destination.delete(); throw new IOException("the selected file was empty or could not be copied: " + picked.name); }
                    copied.add(destination);
                }
            } catch (IOException | SecurityException | IllegalArgumentException e) { error = e.getMessage(); }
            final String failure = error;
            runOnUiThread(() -> {
                running = false;
                for (File file : copied) { games.add(file.getPath()); importedGames.add(file); }
                if (failure == null) status.setText("Game file" + (copied.size() == 1 ? "" : "s") + " copied into app storage and selected.");
                else status.setText("Could not import game file: " + failure + (copied.isEmpty() ? "" : " Previously copied selections are ready."));
                refresh();
            });
        }).start();
    }
    private static String safeFileName(String name) {
        String base = new File(name == null ? "game.nsp" : name).getName().replaceAll("[^A-Za-z0-9._ -]", "_").trim();
        return base.isEmpty() ? "game.nsp" : base;
    }
    private static File uniqueImportFile(File directory, String name) throws IOException {
        File root = directory.getCanonicalFile();
        String base = safeFileName(name), stem = base, ext = "";
        int dot = base.lastIndexOf('.');
        if (dot > 0) { stem = base.substring(0, dot); ext = base.substring(dot); }
        for (int suffix = 1; ; suffix++) {
            String candidate = stem + (suffix == 1 ? "" : " (" + suffix + ")") + ext;
            File file = new File(root, candidate).getCanonicalFile();
            if (!file.getParentFile().equals(root)) throw new IOException("invalid game file name");
            if (!file.exists()) return file;
        }
    }

    private void chooseSaveExport() {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip"); i.putExtra(Intent.EXTRA_TITLE, "PokemonShield-save.zip");
        startActivityForResult(i, PICK_SAVE_EXPORT);
    }
    private void chooseSaveImport() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        startActivityForResult(i, PICK_SAVE_IMPORT);
    }
    private void importSave(Uri source) {
        if (source == null) { status.setText("Choose the save ZIP you edited or exported."); return; }
        running = true; refresh(); status.setText("Importing save archive...");
        new Thread(() -> {
            String result;
            try (InputStream input = getContentResolver().openInputStream(source)) {
                if (input == null) throw new IOException("the file provider could not open the selected archive");
                int files = SaveImport.install(input, new File(gameDir(), "save_import"));
                result = "Imported " + files + " save file" + (files == 1 ? "" : "s") +
                         ". Tap Play to load it; your previous save will be kept as a backup.";
            } catch (IOException | SecurityException e) {
                result = "Could not import save data: " + e.getMessage();
            }
            final String message = result;
            runOnUiThread(() -> { running = false; status.setText(message); refresh(); });
        }).start();
    }
    private void exportSave(Uri destination) {
        if (destination == null) return;
        File save = new File(gameDir(), "save");
        status.setText("Exporting save data...");
        new Thread(() -> {
            String result;
            try {
                OutputStream output = getContentResolver().openOutputStream(destination, "wt");
                if (output == null) throw new IOException("could not open the selected destination");
                int files;
                try (java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(output)) { files = addSaveFiles(save.getCanonicalFile(), save.getCanonicalFile(), zip); }
                result = "Exported " + files + " save file" + (files == 1 ? "" : "s") + ".";
            } catch (IOException e) { result = "Could not export save data: " + e.getMessage(); }
            final String message = result;
            runOnUiThread(() -> status.setText(message));
        }).start();
    }
    private int addSaveFiles(File root, File directory, java.util.zip.ZipOutputStream zip) throws IOException {
        File[] children = directory.listFiles();
        if (children == null) throw new IOException("could not read " + directory);
        int count = 0;
        String rootPath = root.getPath() + File.separator;
        for (File child : children) {
            File canonical = child.getCanonicalFile();
            if (!child.getAbsolutePath().equals(canonical.getPath())) throw new IOException("save contains a symbolic link");
            if (!canonical.getPath().startsWith(rootPath)) throw new IOException("save contains a link outside its folder");
            if (canonical.isDirectory()) { count += addSaveFiles(root, canonical, zip); continue; }
            if (!canonical.isFile()) continue;
            String relative = root.toPath().relativize(canonical.toPath()).toString().replace(File.separatorChar, '/');
            zip.putNextEntry(new ZipEntry(relative));
            try (InputStream input = new FileInputStream(canonical)) { copy(input, zip); }
            zip.closeEntry(); count++;
        }
        return count;
    }

    // the game's base, update and DLC told apart by their file names (the title ID ends 000 for the base, 800 for the update; [UPD] / [DLC] tags), else by size
    private String[] sortGames() {
        String base = null, upd = null; List<String> dlc = new ArrayList<>();
        for (String g : games) { String n = new File(g).getName().toUpperCase();
            if (n.contains("[UPD]") || n.contains("800]")) upd = g; else if (n.contains("[DLC]") || n.contains("[AOC]")) dlc.add(g);
            else if (base == null || new File(g).length() > new File(base).length()) { if (base != null) dlc.add(base); base = g; } else dlc.add(g); }
        List<String> a = new ArrayList<>(); a.add(base); a.add(upd); a.addAll(dlc); return a.toArray(new String[0]);
    }
    private void startSetup() {
        bar.setMax(6); bar.setProgress(0); status.setText("Preparing...");
        String[] g = sortGames(); List<String> cmd = new ArrayList<>();
        cmd.add("--keys"); cmd.add(keysDir().getPath()); cmd.add("--base"); cmd.add(g[0]);
        if (g[1] != null) { cmd.add("--update"); cmd.add(g[1]); }
        for (int k = 2; k < g.length; k++) { cmd.add("--dlc"); cmd.add(g[k]); }
        runSetup(cmd, err -> {
            bar.setProgress(err == null ? 6 : bar.getProgress());
            status.setText(err == null ? "Ready. The setup log is in Android/data/" + getPackageName() + "/files/setup_work." : "Setup failed: " + err);
            if (err == null) {
                for (File file : importedGames) file.delete();
                importedGames.clear();
                new File(gameDir(), "imported").delete();
                if (gameIcon() != null) pinShortcut();
            }
        });
    }
    // shaders the game saved on an earlier run (nvn_capture/<key>.code) and no setup has translated yet (captures_done lists those already done)
    private List<String> pendingCaptures() {
        String[] all = new File(gameDir(), "nvn_capture").list((d, n) -> n.endsWith(".code")); List<String> out = new ArrayList<>(); if (all == null) return out;
        Set<String> done = new HashSet<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(gameDir(), "captures_done")))) { String l; while ((l = r.readLine()) != null) done.add(l.trim()); } catch (IOException e) { }
        for (String n : all) if (!done.contains(n)) out.add(n);
        return out;
    }
    private void startCaptures() {
        List<String> todo = pendingCaptures(); if (todo.isEmpty()) return;
        bar.setMax(3); bar.setProgress(0); status.setText("Adding " + todo.size() + " shader" + (todo.size() == 1 ? "" : "s") + " the game made last time...");
        runSetup(new ArrayList<>(Arrays.asList("--captures", new File(gameDir(), "nvn_capture").getPath())), err -> {
            if (err != null) { status.setText("Could not add the new shaders (" + err + "); the game still plays without them."); return; }
            try (FileWriter w = new FileWriter(new File(gameDir(), "captures_done"), true)) { for (String n : todo) w.write(n + "\n"); } catch (IOException e) { }
            startGame();
        });
    }
    // setup_game.py with the given arguments (plus the folders every run needs): progress from its "STEP n/total what" lines, done(null) or done(the reason it failed) on the UI thread
    private void runSetup(List<String> args, Consumer<String> done) {
        running = true; refresh();
        new Thread(() -> {
            String err = null;
            try {
                File py = unpackPython(), dm = unpackScripts(), bin = binDir();
                File work = new File(gameDir(), "setup_work"); work.mkdirs();
                List<String> cmd = new ArrayList<>();
                String lib = getApplicationInfo().nativeLibraryDir;
                cmd.add(lib + "/libpython_exe.so"); cmd.add(new File(dm, "android/tools/setup_game.py").getPath());
                cmd.addAll(args);
                cmd.add("--out"); cmd.add(gameDir().getPath()); cmd.add("--work"); cmd.add(work.getPath()); cmd.add("--bin"); cmd.add(bin.getPath());
                File ext = new File(dm, "android/app/assets/nintendo_ext.ttf"); if (ext.exists() && !args.contains("--captures")) { cmd.add("--ext-font"); cmd.add(ext.getPath()); }
                ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true);
                pb.environment().put("PYTHONHOME", py.getPath()); pb.environment().put("PYTHONPATH", new File(dm, "android/pyshim").getPath());
                // bin first: the .NET unpacker dlopens libssl.so.3 / libcrypto.so.3, which binDir() links to the OpenSSL 3 that ships with CPython for Android
                pb.environment().put("LD_LIBRARY_PATH", bin.getPath() + ":" + lib); pb.environment().put("LIBCAPSTONE_PATH", lib); pb.environment().put("NX_NATIVE_LIB_DIR", lib);
                pb.environment().put("DOTNET_OPENSSL_VERSION_OVERRIDE", "3"); pb.environment().put("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1");
                pb.environment().put("TMPDIR", getCacheDir().getPath()); pb.environment().put("J", String.valueOf(Math.max(2, Runtime.getRuntime().availableProcessors())));
                Process p = pb.start();
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream())); String ln;
                while ((ln = r.readLine()) != null) { final String l = ln;
                    if (l.startsWith("STEP ")) { String[] f = l.split(" ", 3); int n = Integer.parseInt(f[1].split("/")[0]); runOnUiThread(() -> { bar.setProgress(n - 1); status.setText(f[2] + "..."); }); }
                    else if (l.startsWith("FAILED")) err = l.substring(6).trim(); }
                if (p.waitFor() != 0 && err == null) err = "setup stopped (" + p.exitValue() + ")";
            } catch (Exception e) { err = e.toString(); }
            final String e2 = err;
            runOnUiThread(() -> { running = false; done.accept(e2); refresh(); });
        }).start();
    }
    private File iconFile() { return new File(gameDir(), "icon.jpg"); }
    // the game's icon from the player's dump: this screen and the recent-apps card
    @SuppressWarnings("deprecation")
    private Bitmap gameIcon() {
        Bitmap b = iconFile().exists() ? BitmapFactory.decodeFile(iconFile().getPath()) : null; if (b == null) return null;
        setTaskDescription(new ActivityManager.TaskDescription("Pokémon Shield", b));
        if (logo != null) { logo.setImageBitmap(b); logo.setVisibility(android.view.View.VISIBLE); }
        return b;
    }
    // once set up the app is the game: it starts straight into it. A long press on its icon offers "Set up again" (another dump, an update, the layout of the setup)
    private void setupShortcut() {
        ShortcutManager sm = getSystemService(ShortcutManager.class); if (sm == null) return;
        Intent again = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN).putExtra("setup", true);
        ShortcutInfo si = new ShortcutInfo.Builder(this, "setup").setShortLabel("Settings").setLongLabel("Settings and setup")
            .setIcon(Icon.createWithResource(this, android.R.drawable.ic_menu_manage)).setIntent(again).build();
        try { List<ShortcutInfo> all = new ArrayList<>(); all.add(si); for (ShortcutInfo d : sm.getDynamicShortcuts()) if (!d.getId().equals("setup")) all.add(d); sm.setDynamicShortcuts(all); } catch (Exception e) { }
    }
    // a home screen shortcut with the game's icon (the launcher asks the player to confirm), also offered on a long press of the app's icon
    private void pinShortcut() {
        Bitmap b = gameIcon(); ShortcutManager sm = getSystemService(ShortcutManager.class); if (b == null || sm == null) return;
        Intent go = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN);
        ShortcutInfo si = new ShortcutInfo.Builder(this, "game").setShortLabel("Pokémon Shield").setLongLabel("Play Pokémon Shield").setIcon(Icon.createWithBitmap(b)).setIntent(go).build();
        try { sm.addDynamicShortcuts(Arrays.asList(si)); } catch (Exception e) { }
        for (ShortcutInfo p : sm.getPinnedShortcuts()) if (p.getId().equals("game")) { sm.updateShortcuts(Arrays.asList(si)); status.setText("The Pokémon Shield shortcut is on the home screen."); return; }
        if (sm.isRequestPinShortcutSupported()) sm.requestPinShortcut(si, null); else status.setText("This launcher cannot add shortcuts: long-press the app's icon for Pokémon Shield.");
    }
    // the executables the setup runs, under the names the scripts call them by (links into the app's native library folder, where Android allows execution);
    // also OpenSSL 3 under the names the .NET unpacker dlopens (libssl.so.3 / libcrypto.so.3), so it does not pick up the system's BoringSSL
    private File binDir() throws IOException {
        File d = new File(getFilesDir(), "bin"); d.mkdirs(); String lib = getApplicationInfo().nativeLibraryDir;
        String[][] links = { { "nx_extract", "libnx_extract.so" }, { "dmsh_xlate", "libdmsh_xlate.so" }, { "spirv-cross", "libspirv_cross.so" },
                             { "libssl.so.3", "libssl_python.so" }, { "libcrypto.so.3", "libcrypto_python.so" } };
        for (String[] l : links) { File f = new File(d, l[0]); f.delete(); try { android.system.Os.symlink(lib + "/" + l[1], f.getPath()); } catch (Exception e) { throw new IOException("link " + l[0] + ": " + e); } }
        return d;
    }
    private File unpackPython() throws IOException { return unpack("python.zip", new File(getFilesDir(), "python")); }
    private File unpackScripts() throws IOException { return unpack("dm.zip", new File(getFilesDir(), "dm")); }
    // an asset zip unpacked once per app version (a stamp file records the version)
    private File unpack(String asset, File to) throws IOException {
        long ver; try { ver = getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { ver = 0; }
        File stamp = new File(to, ".unpacked"); try (BufferedReader r = new BufferedReader(new FileReader(stamp))) { if (Long.parseLong(r.readLine().trim()) == ver) return to; } catch (Exception e) { }
        to.mkdirs();
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(getAssets().open(asset)))) { ZipEntry e;
            while ((e = z.getNextEntry()) != null) { File f = new File(to, e.getName()); if (!f.getCanonicalPath().startsWith(to.getCanonicalPath())) continue;
                if (e.isDirectory()) { f.mkdirs(); continue; } f.getParentFile().mkdirs(); try (OutputStream o = new FileOutputStream(f)) { copy(z, o); } } }
        try (FileWriter w = new FileWriter(stamp)) { w.write(ver + "\n"); }
        return to;
    }
    private void startGame() { startActivity(new Intent(this, GameActivity.class)); finish(); }
}
