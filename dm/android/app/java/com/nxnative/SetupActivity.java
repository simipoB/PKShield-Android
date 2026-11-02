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
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
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
import android.view.KeyEvent;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.*;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class SetupActivity extends Activity {
    private static final int PICK_KEYS = 2, PICK_DRIVER = 3, PICK_SAVE_EXPORT = 4, PICK_SAVE_IMPORT = 5, PICK_MOD = 6, PICK_BASE = 7, PICK_UPDATE = 8, PICK_DLC = 9;
    // the installation guide's steps, in order (W_ACCESS only shows while the all-files permission is missing)
    private static final int W_WELCOME = 0, W_ACCESS = 1, W_KEYS = 2, W_BASE = 3, W_UPDATE = 4, W_DLC = 5, W_OPTIONS = 6, W_INSTALL = 7;
    private TextView driverText, modsText;
    // what the guide has collected: paths read in place, or copies in the app's storage (importedGames) when the picked file has no readable path
    private String baseGame, updateGame; private final List<String> dlcGames = new ArrayList<>();
    private final List<File> importedGames = new ArrayList<>();
    private static final class PickedGame {
        final Uri uri; final String name; final int role; File dest;
        PickedGame(Uri uri, String name, int role) { this.uri = uri; this.name = name; this.role = role; }
    }
    private TextView status, wizTitle, titleView; private ProgressBar bar; private Button play, shortcut, saveExport, saveImport, wizBack, wizNext; private ImageView logo;
    private LinearLayout wizBody;
    private boolean wizard, installedNow, installFailed, defaultsDone; private int step;
    private volatile boolean running;
    private static String t(String en, String es) { return "es".equals(java.util.Locale.getDefault().getLanguage()) ? es : en; }   // the guide's texts: English, Spanish on Spanish phones
    private static String gb(long bytes) { return String.format(java.util.Locale.US, "%.1f GB", bytes / 1073741824.0); }
    private File gameDir() { return getExternalFilesDir(null); }
    private File modsDir() { File d = new File(gameDir(), "mods"); d.mkdirs(); return d; }
    private File keysDir() { File d = new File(getFilesDir(), "keys"); d.mkdirs(); return d; }
    private boolean hasShield() {
        File d = gameDir();
        if (new File(d, "ready_shield").exists() && (new File(d, "shaders_shield.pak").exists() || new File(d, "shaders.pak").exists())) return true;
        if (new File(d, "ready").exists() && new File(d, "shaders.pak").exists()) {
            if (new File(d, "ready_sword").exists() || new File(d, "shaders_sword.pak").exists()) return false;
            File fixup = new File(d, "patched/main.fixups.tsv");
            if (fixup.exists()) {
                String bid = readBuildId(fixup);
                return bid == null || !bid.toLowerCase().startsWith("a3b75bcd");
            }
            return true;
        }
        return false;
    }
    private boolean hasSword() {
        File d = gameDir();
        if (new File(d, "ready_sword").exists() && (new File(d, "shaders_sword.pak").exists() || new File(d, "shaders.pak").exists())) return true;
        if (new File(d, "ready").exists() && new File(d, "shaders.pak").exists()) {
            if (new File(d, "ready_shield").exists() || new File(d, "shaders_shield.pak").exists()) return false;
            File fixup = new File(d, "patched/main.fixups.tsv");
            if (fixup.exists()) {
                String bid = readBuildId(fixup);
                return bid != null && bid.toLowerCase().startsWith("a3b75bcd");
            }
        }
        return false;
    }
    private static String readBuildId(File fixup) {
        if (!fixup.exists()) return null;
        try (BufferedReader r = new BufferedReader(new FileReader(fixup))) {
            String ln;
            while ((ln = r.readLine()) != null) {
                if (ln.startsWith("# BUILD_ID")) {
                    String[] parts = ln.split("\\s+");
                    if (parts.length >= 3) return parts[2].trim();
                }
            }
        } catch (Exception ignored) { }
        return null;
    }
    private String currentVersion() {
        String v = envGet("DM_GAME_VERSION");
        if (v.isEmpty()) v = envGet("DM_VERSION");
        if ("sword".equalsIgnoreCase(v)) {
            if (hasSword() || !hasShield()) return "sword";
            return "shield";
        }
        if ("shield".equalsIgnoreCase(v)) {
            if (hasShield() || !hasSword()) return "shield";
            return "sword";
        }
        if (hasSword() && !hasShield()) return "sword";
        return "shield";
    }
    private boolean isSword() { return "sword".equalsIgnoreCase(currentVersion()); }
    private String gameTitle() { return isSword() ? "Pokémon Sword" : "Pokémon Shield"; }
    private void setVersion(String v) {
        envSet("DM_GAME_VERSION", v);
        envSet("DM_VERSION", v);
        if ("sword".equalsIgnoreCase(v)) {
            envSet("DM_PROGRAM_ID", "0100abf008968000");
            envSet("DM_GAME", "0100ABF008968000");
        } else {
            envSet("DM_PROGRAM_ID", "01008db008c2c000");
            envSet("DM_GAME", "01008DB008C2C000");
        }
        if (titleView != null) titleView.setText(gameTitle());
    }
    private String detectGameVersion(InputStream in) {
        if (in == null) return null;
        try {
            byte[] buf = new byte[131072];
            int read = in.read(buf);
            if (read > 0) {
                String head = new String(buf, 0, read, java.nio.charset.StandardCharsets.ISO_8859_1).toUpperCase();
                if (head.contains("0100ABF00896") || head.contains("SWORD") || head.contains("ESPADA")) return "sword";
                if (head.contains("01008DB008C2") || head.contains("SHIELD") || head.contains("ESCUDO")) return "shield";
            }
        } catch (Exception ignored) { }
        return null;
    }
    private String detectGameVersion(Uri u, String name) {
        if (name != null) {
            String n = name.toUpperCase();
            if (n.contains("0100ABF00896") || n.contains("SWORD") || n.contains("ESPADA")) return "sword";
            if (n.contains("01008DB008C2") || n.contains("SHIELD") || n.contains("ESCUDO")) return "shield";
        }
        if (u != null) {
            try (InputStream in = getContentResolver().openInputStream(u)) {
                return detectGameVersion(in);
            } catch (Exception ignored) { }
        }
        return null;
    }
    private String detectGameVersion(File f) {
        if (f == null || !f.exists()) return null;
        String v = detectGameVersion(null, f.getName());
        if (v != null) return v;
        try (InputStream in = new FileInputStream(f)) {
            return detectGameVersion(in);
        } catch (Exception ignored) { }
        return null;
    }
    private static File setupLogFile(String ver) {
        boolean sw = "sword".equalsIgnoreCase(ver);
        String fname = sw ? "PokemonSword-setup-log.txt" : "PokemonShield-setup-log.txt";
        File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (dl != null && (dl.isDirectory() || dl.mkdirs())) return new File(dl, fname);
        return new File("/storage/emulated/0/Download", fname);
    }
    private void logSetup(String msg) { logSetup(currentVersion(), msg); }
    private static synchronized void logSetup(String ver, String msg) {
        android.util.Log.i("PKSetup", msg);
        String stamp = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", java.util.Locale.US).format(new java.util.Date());
        String line = "[" + stamp + "] " + msg + "\n";
        File f = setupLogFile(ver);
        try (FileWriter fw = new FileWriter(f, true)) { fw.write(line); } catch (Exception ignored) { }
    }
    private void showLogDialog() {
        File lf = setupLogFile(currentVersion());
        File alt = new File(new File(gameDir(), "setup_work"), "setup.log");
        StringBuilder sb = new StringBuilder();
        File target = lf.exists() ? lf : (alt.exists() ? alt : null);
        if (target != null) {
            try (BufferedReader br = new BufferedReader(new FileReader(target))) {
                List<String> lines = new ArrayList<>(); String l;
                while ((l = br.readLine()) != null) { lines.add(l); if (lines.size() > 400) lines.remove(0); }
                for (String s : lines) sb.append(s).append("\n");
            } catch (Exception e) { sb.append("Error reading log: ").append(e.getMessage()); }
        } else {
            sb.append(t("No setup log found yet.", "Aún no se ha generado ningún registro de instalación."))
              .append("\n\n").append(t("Expected file: ", "Archivo esperado: ")).append(lf.getPath());
        }
        ScrollView sv = new ScrollView(this);
        HorizontalScrollView hsv = new HorizontalScrollView(this);
        TextView tv = new TextView(this);
        tv.setText(sb.toString()); tv.setTextSize(12); tv.setTypeface(Typeface.MONOSPACE);
        tv.setPadding(32, 32, 32, 32); tv.setTextIsSelectable(true);
        hsv.addView(tv); sv.addView(hsv);
        new AlertDialog.Builder(this)
            .setTitle(t("Installation Log: " + lf.getName(), "Registro: " + lf.getName()))
            .setView(sv)
            .setPositiveButton(t("Copy to clipboard", "Copiar al portapapeles"), (d, w) -> {
                ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) {
                    cm.setPrimaryClip(ClipData.newPlainText("SetupLog", sb.toString()));
                    status.setText(t("Log copied to clipboard.", "Registro copiado al portapapeles."));
                }
            })
            .setNegativeButton(t("Close", "Cerrar"), null)
            .show();
    }
    private boolean inApk() {
        try (InputStream is = getAssets().open("game/ready")) { return is != null; } catch (IOException e) { return false; }
    }
    private boolean setUp() { return inApk() || hasShield() || hasSword() || (new File(gameDir(), "ready").exists() && new File(gameDir(), "shaders.pak").exists()); }
    // set up for this app version: the shader pack's translation chain (the ready file) is the one the engine was built for (assets/shader_chain, build_apk.py);
    // an update that changes the chain otherwise starts the game with a pack the engine refuses (every 3D shader "SPIR-V missing")
    private boolean ready() { return setUp() && chainCurrent(); }
    private boolean chainCurrent() {
        String want, have = null;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("shader_chain")))) { want = r.readLine(); } catch (IOException e) { return true; }   // an older build without the asset
        if (inApk()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("game/ready")))) { have = r.readLine(); } catch (IOException e) { }
        }
        if (have == null) {
            String checkFile = isSword() && new File(gameDir(), "ready_sword").exists() ? "ready_sword" :
                               new File(gameDir(), "ready_shield").exists() ? "ready_shield" : "ready";
            try (BufferedReader r = new BufferedReader(new FileReader(new File(gameDir(), checkFile)))) { have = r.readLine(); } catch (IOException e) { return false; }
        }
        return want != null && have != null && want.trim().equals(have.trim());
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        String launchVer = getIntent().getStringExtra("game_version");
        if (launchVer != null && !launchVer.isEmpty()) setVersion(launchVer);
        boolean asked = getIntent().getBooleanExtra("setup", false);
        if (b != null) {   // the guide's progress survives the process being recreated while a picker is open
            wizard = b.getBoolean("wizard"); step = b.getInt("step"); baseGame = b.getString("base"); updateGame = b.getString("update");
            String[] d = b.getStringArray("dlc"); if (d != null) dlcGames.addAll(Arrays.asList(d));
            String[] im = b.getStringArray("imported"); if (im != null) for (String p : im) importedGames.add(new File(p));
        }
        if (ready()) setupShortcut();
        syncPatchedFiles();
        if (ready() && !asked && !wizard) { gameIcon(); if (pendingCaptures().isEmpty()) { startGame(); return; } }
        if (!ready() || wizard) showWizard(); else showDashboard();
        gameIcon();
        if (ready() && !asked && !wizard) startCaptures();
    }
    @Override protected void onSaveInstanceState(Bundle o) {
        super.onSaveInstanceState(o); o.putBoolean("wizard", wizard); o.putInt("step", step); o.putString("base", baseGame); o.putString("update", updateGame);
        o.putStringArray("dlc", dlcGames.toArray(new String[0]));
        String[] im = new String[importedGames.size()]; for (int k = 0; k < im.length; k++) im[k] = importedGames.get(k).getPath(); o.putStringArray("imported", im);
    }
    @Override protected void onResume() { super.onResume(); if (wizard && step == W_ACCESS) renderStep(); }   // back from the all-files permission screen
    @Override public void onBackPressed() {
        if (wizard && running) return;
        if (wizard && step > W_WELCOME && !installedNow) { wizGo(-1); return; }
        if (wizard && ready() && !installedNow) { showDashboard(); return; }   // the guide was started again from the settings screen
        super.onBackPressed();
    }
    private LinearLayout page() {
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(48, 48, 48, 48); root.setGravity(Gravity.CENTER_HORIZONTAL);
        logo = new ImageView(this); logo.setVisibility(android.view.View.GONE); root.addView(logo, new LinearLayout.LayoutParams(320, 320));
        titleView = new TextView(this); titleView.setText(gameTitle()); titleView.setTextSize(28); root.addView(titleView);
        return root;
    }
    private void show(LinearLayout root) { ScrollView sv = new ScrollView(this); sv.addView(root); setContentView(sv); }
    private TextView label(LinearLayout p, String s, int size, int top) { TextView v = new TextView(this); v.setText(s); v.setTextSize(size); v.setPadding(0, top, 0, 8); p.addView(v); return v; }

    // the set-up game: Play, the settings, the guide again, save data
    private void showDashboard() {
        wizard = false; wizBody = null; wizTitle = null; wizBack = wizNext = null;
        LinearLayout root = page();
        if (hasSword() && hasShield()) {
            LinearLayout vGroup = new LinearLayout(this);
            vGroup.setOrientation(LinearLayout.HORIZONTAL);
            vGroup.setGravity(Gravity.CENTER_HORIZONTAL);
            vGroup.setPadding(0, 8, 0, 16);
            Button bShield = new Button(this);
            bShield.setText("Pokémon Shield" + (!isSword() ? "  ●" : ""));
            bShield.setOnClickListener(v -> { setVersion("shield"); showDashboard(); gameIcon(); });
            Button bSword = new Button(this);
            bSword.setText("Pokémon Sword" + (isSword() ? "  ●" : ""));
            bSword.setOnClickListener(v -> { setVersion("sword"); showDashboard(); gameIcon(); });
            vGroup.addView(bShield);
            vGroup.addView(bSword);
            root.addView(vGroup);
        }
        play = button(root, t("Play " + gameTitle(), "Jugar " + gameTitle()), v -> startGame()); play.setEnabled(ready());
        status = new TextView(this); status.setPadding(0, 16, 0, 16); root.addView(status);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(600); root.addView(bar);
        settings(root);
        button(root, t("Run the installation guide again", "Repetir la guía de instalación"), v -> { step = W_WELCOME; installedNow = installFailed = false; showWizard(); });
        button(root, t("View installation log", "Ver registro de instalación"), v -> showLogDialog());
        saveExport = button(root, "Export save data", v -> chooseSaveExport());
        saveImport = button(root, "Import edited save data", v -> chooseSaveImport());
        if (hasSword() && hasShield()) {
            button(root, t("Add Pokémon Shield to the home screen", "Añadir Pokémon Shield al inicio"), v -> pinShortcut("shield"));
            button(root, t("Add Pokémon Sword to the home screen", "Añadir Pokémon Sword al inicio"), v -> pinShortcut("sword"));
        } else {
            shortcut = button(root, t("Add " + gameTitle() + " to the home screen", "Añadir " + gameTitle() + " al inicio"), v -> pinShortcut(currentVersion()));
            String other = isSword() ? "Pokémon Shield" : "Pokémon Sword";
            String otherVer = isSword() ? "shield" : "sword";
            button(root, t("Install " + other + " as second game", "Instalar " + other + " como segundo juego"), v -> {
                setVersion(otherVer);
                baseGame = null; updateGame = null; dlcGames.clear();
                step = W_WELCOME; installedNow = installFailed = false; showWizard();
            });
        }
        show(root); refresh();
    }
    // the installation guide: one screen per thing the install needs (keys, base game, update, DLC), then options, then the install itself
    private void showWizard() {
        wizard = true; play = shortcut = saveExport = saveImport = null;
        if (!defaultsDone) { defaultsDone = true; wizardDefaults(); }
        LinearLayout root = page();
        wizTitle = new TextView(this); wizTitle.setPadding(0, 8, 0, 16); root.addView(wizTitle);
        wizBody = new LinearLayout(this); wizBody.setOrientation(LinearLayout.VERTICAL); root.addView(wizBody);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(600); root.addView(bar);
        status = new TextView(this); status.setPadding(0, 16, 0, 16); root.addView(status);
        LinearLayout nav = new LinearLayout(this); nav.setOrientation(LinearLayout.HORIZONTAL);
        wizBack = new Button(this); wizBack.setText(t("Back", "Atrás")); wizBack.setOnClickListener(v -> wizGo(-1));
        wizNext = new Button(this); wizNext.setOnClickListener(v -> wizAdvance());
        nav.addView(wizBack, new LinearLayout.LayoutParams(0, -2, 1)); nav.addView(wizNext, new LinearLayout.LayoutParams(0, -2, 1));
        root.addView(nav, new LinearLayout.LayoutParams(-1, -2));
        show(root); renderStep();
    }
    // preselects what suits this phone, once per guide run: 20:9 on a wide screen, 60 FPS (a choice the player made earlier is kept)
    private void wizardDefaults() {
        if (envGet("DM_ASPECT").isEmpty()) {
            android.util.DisplayMetrics m = getResources().getDisplayMetrics();
            if ((float) Math.max(m.widthPixels, m.heightPixels) / Math.min(m.widthPixels, m.heightPixels) >= 2.0f) envSet("DM_ASPECT", "20:9");
        }
        if (envGet("DM_FPS_TARGET").isEmpty()) envSet("DM_FPS_TARGET", "60");
    }
    private TextView row(LinearLayout root, String label, String btn, android.view.View.OnClickListener l) {
        LinearLayout r = new LinearLayout(this); r.setOrientation(LinearLayout.HORIZONTAL); r.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this); t.setText(label); t.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1)); r.addView(t);
        Button bt = new Button(this); bt.setText(btn); bt.setOnClickListener(l); r.addView(bt); root.addView(r);
        TextView sel = new TextView(this); sel.setPadding(0, 0, 0, 24); root.addView(sel); return sel;
    }
    private Button button(LinearLayout root, String text, android.view.View.OnClickListener l) { Button bt = new Button(this); bt.setText(text); bt.setOnClickListener(l); root.addView(bt); return bt; }

    // Settings, read by the game at its next start (env.txt): graphics (the console mode the game renders for), aspect ratio and frame rate; shared by the guide's Options step and the settings screen
    private void displayOptions(LinearLayout root) {
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
        TextView al = new TextView(this); al.setText("Aspect ratio"); al.setPadding(0, 16, 0, 0); root.addView(al);
        android.widget.RadioGroup arg = new android.widget.RadioGroup(this);
        String[][] aspects = {
            { "fit", "16:9: original aspect ratio (side bars on 20:9 phones)" },
            { "20:9", "20:9: widescreen (built-in 3D camera fix; optional .pchtxt override)" },
            { "crop", "20:9 Zoom: fills the screen without stretching (crops top and bottom)" },
            { "stretch", "Full screen: stretches to fill any screen" }
        };
        String acur = envGet("DM_ASPECT"); if (acur.isEmpty()) acur = "fit";
        for (int k = 0; k < aspects.length; k++) {
            android.widget.RadioButton b = new android.widget.RadioButton(this); b.setId(1100 + k); b.setText(aspects[k][1]); arg.addView(b);
            if (aspects[k][0].equals(acur)) b.setChecked(true);
        }
        arg.setOnCheckedChangeListener((g, id) -> envSet("DM_ASPECT", aspects[id - 1100][0]));
        root.addView(arg);
        TextView fl = new TextView(this); fl.setText(t("Frame rate", "Velocidad de fotogramas")); fl.setPadding(0, 16, 0, 0); root.addView(fl);
        android.widget.RadioGroup frg = new android.widget.RadioGroup(this);
        // 60 / 90 / 120: the game steps by the real time of each frame (hle_nvn.c vstep_tick), so its speed stays normal: when the phone cannot keep a rate it drops to a half or a third of it
        String[][] fpsModes = {
            { "30", t("30 FPS: original console frame rate", "30 FPS: velocidad original de la consola") },
            { "60", t("60 FPS: smooth, uses more battery (" + gameTitle() + " 1.3.2)", "60 FPS: fluido, gasta más batería (" + gameTitle() + " 1.3.2)") },
            { "90", t("90 FPS: needs a 90 Hz or faster screen; falls back to 45 / 30 when the phone cannot keep up", "90 FPS: requiere pantalla de 90 Hz o más; baja a 45 / 30 si el teléfono no da abasto") },
            { "120", t("120 FPS: needs a 120 Hz screen and a powerful phone; falls back to 60 / 40 when it cannot keep up (game speed stays normal)", "120 FPS: requiere pantalla de 120 Hz y un teléfono potente; baja a 60 / 40 si no da abasto (la velocidad del juego no cambia)") }
        };
        String fcur = envGet("DM_FPS_TARGET"); boolean known = false; for (String[] m : fpsModes) known |= m[0].equals(fcur); if (!known) fcur = "30";
        for (int k = 0; k < fpsModes.length; k++) {
            android.widget.RadioButton b = new android.widget.RadioButton(this); b.setId(1200 + k); b.setText(fpsModes[k][1]); frg.addView(b);
            if (fpsModes[k][0].equals(fcur)) b.setChecked(true);
        }
        frg.setOnCheckedChangeListener((g, id) -> envSet("DM_FPS_TARGET", fpsModes[id - 1200][0]));
        root.addView(frg);
    }
    // the Vulkan driver: the phone's own, or an Adreno driver package (a zip with meta.json and the library, e.g. Mesa Turnip) opened through libadrenotools
    private void driverRows(LinearLayout root) {
        driverText = row(root, "Graphics driver", "Choose", v -> pick(PICK_DRIVER, false));
        button(root, "Use the phone's own driver", v -> { envSet("DM_VK_DRIVER", ""); refresh(); });
    }
    private void settings(LinearLayout root) {
        TextView h = new TextView(this); h.setText("Settings"); h.setTextSize(20); h.setPadding(0, 32, 0, 8); root.addView(h);
        displayOptions(root);
        driverRows(root);
        controllerOptions(root);
        android.widget.CheckBox pin = new android.widget.CheckBox(this); pin.setPadding(0, 16, 0, 0);
        pin.setText(t("Keep the game on the fast CPU cores (can steady the frame rate; uses a little more battery)", "Mantener el juego en los núcleos rápidos de la CPU (puede estabilizar los FPS; gasta algo más de batería)"));
        pin.setChecked("1".equals(envGet("DM_CPU_PIN")));
        pin.setOnCheckedChangeListener((b, on) -> envSet("DM_CPU_PIN", on ? "1" : ""));
        root.addView(pin);
        presentOptions(root);
        modsText = row(root, "Code patches (.pchtxt)", "Choose", v -> pick(PICK_MOD, true));
        button(root, "Clear code patches", v -> { deleteTree(modsDir()); modsDir(); refresh(); });
        slider(root, "Touch controls opacity", "DM_TOUCH_OPACITY", 20, 100, 100, v -> v + " %");
        slider(root, "Touch controls fade after", "DM_TOUCH_FADE", 0, 10, 3, v -> v == 0 ? "never fade" : v + " s without a touch");
    }
    // Vulkan presentation mode (DM_VK_PRESENT, read by nvn_vk.c at the window's creation; falls back to FIFO when the phone lacks the mode)
    private void presentOptions(LinearLayout root) {
        TextView pl = new TextView(this); pl.setText(t("Presentation mode", "Modo de presentación")); pl.setPadding(0, 16, 0, 0); root.addView(pl);
        android.widget.RadioGroup prg = new android.widget.RadioGroup(this);
        String[][] modes = {
            { "mailbox", t("Mailbox: no tearing, low latency (default)", "Mailbox: sin cortes, baja latencia (predeterminado)") },
            { "immediate", t("Immediate: no V-Sync, may tear, lowest latency", "Immediate: sin V-Sync, puede haber cortes, mínima latencia") },
            { "fifo", t("FIFO: strict V-Sync (steadiest, a slow compositor can stall)", "FIFO: V-Sync estricto (más estable, un compositor lento puede frenar el juego)") }
        };
        String cur = envGet("DM_VK_PRESENT"); if (cur.isEmpty()) cur = "mailbox";
        for (int k = 0; k < modes.length; k++) {
            android.widget.RadioButton b = new android.widget.RadioButton(this); b.setId(1300 + k); b.setText(modes[k][1]); prg.addView(b);
            if (modes[k][0].equals(cur)) b.setChecked(true);
        }
        prg.setOnCheckedChangeListener((g, id) -> envSet("DM_VK_PRESENT", modes[id - 1300][0]));
        root.addView(prg);
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

    private TextView controllerText;

    private Map<String, Integer> loadPadMapping() {
        return PadMapping.load(envGet("DM_PAD_LAYOUT"), envGet("DM_PAD_MAPPING"));
    }

    private void savePadMapping(Map<String, Integer> map, String layout) {
        envSet("DM_PAD_LAYOUT", layout);
        if ("custom".equals(layout)) {
            envSet("DM_PAD_MAPPING", PadMapping.serialize(map));
        } else {
            envSet("DM_PAD_MAPPING", "");
        }
    }

    private void controllerOptions(LinearLayout root) {
        TextView cl = new TextView(this);
        cl.setText(t("Physical controller (Gamepad)", "Mando físico (Gamepad)"));
        cl.setTextSize(18);
        cl.setPadding(0, 24, 0, 8);
        root.addView(cl);

        android.widget.RadioGroup crg = new android.widget.RadioGroup(this);
        String[][] padModes = {
            { "default", t("Xbox / Standard layout (face buttons labeled as Switch)", "Distribución Xbox / Estándar (botones con letra Switch)") },
            { "nintendo", t("Nintendo layout (A=A, B=B, X=X, Y=Y direct)", "Distribución Nintendo (A=A, B=B, X=X, Y=Y directa)") },
            { "custom", t("Custom button mapping", "Mapeo personalizado de botones") }
        };
        String curLayout = envGet("DM_PAD_LAYOUT");
        if (curLayout.isEmpty()) {
            curLayout = envGet("DM_PAD_MAPPING").isEmpty() ? "default" : "custom";
        }
        for (int k = 0; k < padModes.length; k++) {
            android.widget.RadioButton b = new android.widget.RadioButton(this);
            b.setId(1300 + k);
            b.setText(padModes[k][1]);
            crg.addView(b);
            if (padModes[k][0].equals(curLayout)) b.setChecked(true);
        }
        crg.setOnCheckedChangeListener((g, id) -> {
            String selected = padModes[id - 1300][0];
            envSet("DM_PAD_LAYOUT", selected);
            if (!"custom".equals(selected)) {
                envSet("DM_PAD_MAPPING", "");
            }
            updateControllerSummary();
        });
        root.addView(crg);

        controllerText = row(root, t("Remap controller buttons", "Remapear botones del mando"), t("Configure", "Configurar"), v -> showRemapDialog(crg));
        updateControllerSummary();
    }

    private void updateControllerSummary() {
        if (controllerText == null) return;
        String layout = envGet("DM_PAD_LAYOUT");
        if ("nintendo".equalsIgnoreCase(layout)) {
            controllerText.setText(t("Active: Nintendo layout (A=A, B=B, X=X, Y=Y)", "Activo: Distribución Nintendo (A=A, B=B, X=X, Y=Y)"));
        } else if ("custom".equalsIgnoreCase(layout)) {
            controllerText.setText(t("Active: Custom button mapping", "Activo: Mapeo personalizado"));
        } else {
            controllerText.setText(t("Active: Xbox / Standard layout (A↔B, X↔Y)", "Activo: Distribución Xbox / Estándar (A↔B, X↔Y)"));
        }
    }

    private void showRemapDialog(android.widget.RadioGroup crg) {
        Map<String, Integer> currentMap = loadPadMapping();
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle(t("Remap controller buttons", "Remapear botones del mando"));

        ScrollView sv = new ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(32, 24, 32, 24);

        TextView info = new TextView(this);
        info.setText(t("Tap any button to remap it, or choose a preset layout:", "Toca cualquier botón para remapearlo, o elige una distribución predefinida:"));
        info.setPadding(0, 0, 0, 16);
        content.addView(info);

        LinearLayout presets = new LinearLayout(this);
        presets.setOrientation(LinearLayout.HORIZONTAL);
        Button btnXbox = new Button(this);
        btnXbox.setText("Xbox / Std");
        Button btnNin = new Button(this);
        btnNin.setText("Nintendo");
        Button btnReset = new Button(this);
        btnReset.setText(t("Reset", "Restablecer"));
        presets.addView(btnXbox, new LinearLayout.LayoutParams(0, -2, 1));
        presets.addView(btnNin, new LinearLayout.LayoutParams(0, -2, 1));
        presets.addView(btnReset, new LinearLayout.LayoutParams(0, -2, 1));
        content.addView(presets);

        TextView btnsHeader = new TextView(this);
        btnsHeader.setText(t("Button bindings:", "Asignación de botones:"));
        btnsHeader.setTextSize(16);
        btnsHeader.setPadding(0, 20, 0, 8);
        content.addView(btnsHeader);

        final Button[] rowButtons = new Button[PadMapping.BUTTONS.length];
        Runnable refreshRows = () -> {
            for (int i = 0; i < PadMapping.BUTTONS.length; i++) {
                PadMapping.PadButton pb = PadMapping.BUTTONS[i];
                Integer kc = currentMap.get(pb.id);
                rowButtons[i].setText(kc != null ? PadMapping.keyName(kc) : t("Not mapped", "No asignado"));
            }
            if (crg != null) {
                String l = envGet("DM_PAD_LAYOUT");
                if ("nintendo".equalsIgnoreCase(l)) crg.check(1301);
                else if ("custom".equalsIgnoreCase(l)) crg.check(1302);
                else crg.check(1300);
            }
            updateControllerSummary();
        };

        boolean es = "es".equals(java.util.Locale.getDefault().getLanguage());
        for (int i = 0; i < PadMapping.BUTTONS.length; i++) {
            final PadMapping.PadButton pb = PadMapping.BUTTONS[i];
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, 8, 0, 8);

            TextView label = new TextView(this);
            label.setText(pb.label(es));
            label.setTextSize(15);
            label.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1));
            row.addView(label);

            Button valBtn = new Button(this);
            rowButtons[i] = valBtn;
            valBtn.setOnClickListener(v -> showButtonCaptureDialog(pb, currentMap, refreshRows));
            row.addView(valBtn);

            content.addView(row);
        }

        btnXbox.setOnClickListener(v -> {
            for (PadMapping.PadButton pb : PadMapping.BUTTONS) currentMap.put(pb.id, pb.defXbox);
            savePadMapping(currentMap, "default");
            refreshRows.run();
        });

        btnNin.setOnClickListener(v -> {
            for (PadMapping.PadButton pb : PadMapping.BUTTONS) currentMap.put(pb.id, pb.defNintendo);
            savePadMapping(currentMap, "nintendo");
            refreshRows.run();
        });

        btnReset.setOnClickListener(v -> {
            for (PadMapping.PadButton pb : PadMapping.BUTTONS) currentMap.put(pb.id, pb.defXbox);
            savePadMapping(currentMap, "default");
            refreshRows.run();
        });

        refreshRows.run();
        sv.addView(content);
        builder.setView(sv);
        builder.setPositiveButton(t("Done", "Listo"), (dialog, which) -> dialog.dismiss());
        builder.show();
    }

    private void showButtonCaptureDialog(PadMapping.PadButton btn, Map<String, Integer> currentMap, Runnable onUpdated) {
        boolean es = "es".equals(java.util.Locale.getDefault().getLanguage());
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle(btn.label(es));

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(48, 32, 48, 32);
        box.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView msg = new TextView(this);
        msg.setText(t("Press any button on your physical controller...\n(or choose from the list below)",
                       "Presiona cualquier botón en el mando físico...\n(o elígelo de la lista abajo)"));
        msg.setTextSize(16);
        msg.setPadding(0, 16, 0, 24);
        msg.setGravity(Gravity.CENTER_HORIZONTAL);
        box.addView(msg);

        Button chooseList = new Button(this);
        chooseList.setText(t("Choose from list", "Elegir de la lista"));
        box.addView(chooseList);

        b.setView(box);
        b.setNegativeButton(t("Cancel", "Cancelar"), null);

        AlertDialog dlg = b.create();

        dlg.setOnKeyListener((dialog, keyCode, event) -> {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                if (keyCode == KeyEvent.KEYCODE_BACK) {
                    dialog.dismiss();
                    return true;
                }
                currentMap.put(btn.id, keyCode);
                savePadMapping(currentMap, "custom");
                onUpdated.run();
                dialog.dismiss();
                return true;
            }
            return false;
        });

        chooseList.setOnClickListener(v -> {
            dlg.dismiss();
            showKeyListDialog(btn, currentMap, onUpdated);
        });

        dlg.show();
    }

    private void showKeyListDialog(PadMapping.PadButton btn, Map<String, Integer> currentMap, Runnable onUpdated) {
        boolean es = "es".equals(java.util.Locale.getDefault().getLanguage());
        String[] names = new String[PadMapping.COMMON_KEYCODES.length];
        for (int i = 0; i < PadMapping.COMMON_KEYCODES.length; i++) {
            names[i] = PadMapping.keyName(PadMapping.COMMON_KEYCODES[i]);
        }

        new AlertDialog.Builder(this)
            .setTitle(btn.label(es))
            .setItems(names, (dialog, which) -> {
                currentMap.put(btn.id, PadMapping.COMMON_KEYCODES[which]);
                savePadMapping(currentMap, "custom");
                onUpdated.run();
            })
            .setNegativeButton(t("Cancel", "Cancelar"), null)
            .show();
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
    private void fillDriverText() {
        if (driverText == null) return;
        String d = envGet("DM_VK_DRIVER");
        driverText.setText(d.isEmpty() ? "the phone's own Vulkan driver" : new File(d).getParentFile().getName() + " (" + new File(d).getName() + ")");
    }
    private void refresh() {
        if (wizard) { renderStep(); return; }
        fillDriverText();
        updateControllerSummary();
        if (modsText != null) { String[] ms = modsDir().list((d, n) -> n.toLowerCase().endsWith(".pchtxt")); if (ms != null) Arrays.sort(ms);
            modsText.setText(ms == null || ms.length == 0 ? "none (optional IPSwitch .pchtxt mods, e.g. 20:9 camera)" : String.join("\n", ms)); }
        play.setEnabled(!running && ready());
        saveExport.setEnabled(!running && setUp());
        saveImport.setEnabled(!running && setUp());
        if (shortcut != null) shortcut.setVisibility(iconFile().exists() ? android.view.View.VISIBLE : android.view.View.GONE);
    }

    // ---- the installation guide ----
    private boolean stepShown(int s) { return s != W_ACCESS || (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()); }
    private void wizGo(int d) {
        if (running) return;
        int s = step + d; while (s > W_WELCOME && s < W_INSTALL && !stepShown(s)) s += d;
        step = Math.max(W_WELCOME, Math.min(W_INSTALL, s)); status.setText(""); renderStep();
    }
    private void wizAdvance() {
        if (running) return;
        if (step < W_INSTALL) wizGo(1); else if (installedNow) startGame(); else startSetup();
    }
    private TextView head(String s) { return label(wizBody, s, 22, 8); }
    private TextView para(String s) { return label(wizBody, s, 16, 8); }
    private Button choose(String text, int what, boolean multi) { Button b = button(wizBody, text, v -> pick(what, multi)); b.setEnabled(!running); return b; }
    private static long sizeOf(String path) { return path == null ? 0 : new File(path).length(); }
    private String nameOrNone(String path, String none) { return path == null ? none : "✓ " + new File(path).getName(); }
    private void renderStep() {
        if (wizBody == null) return;
        while (step < W_INSTALL && !stepShown(step)) step++;   // the permission step disappears once the permission is granted
        wizBody.removeAllViews(); driverText = null; modsText = null;
        int cur = 0, total = 0; for (int s = 0; s <= W_INSTALL; s++) if (stepShown(s)) { total++; if (s <= step) cur = total; }
        wizTitle.setText(t("Step ", "Paso ") + cur + " / " + total);
        bar.setVisibility(step == W_INSTALL ? android.view.View.VISIBLE : android.view.View.GONE);
        boolean keys = new File(keysDir(), "prod.keys").exists(), titleKeys = new File(keysDir(), "title.keys").exists(), ok = true;
        String next = t("Next", "Siguiente");
        switch (step) {
            case W_WELCOME:
                head(t("Install " + gameTitle(), "Instalar " + gameTitle()));
                LinearLayout vSel = new LinearLayout(this);
                vSel.setOrientation(LinearLayout.HORIZONTAL);
                vSel.setGravity(Gravity.CENTER_HORIZONTAL);
                vSel.setPadding(0, 8, 0, 16);
                Button bSh = new Button(this);
                bSh.setText("Pokémon Shield" + (!isSword() ? "  ●" : ""));
                bSh.setOnClickListener(v -> { setVersion("shield"); renderStep(); });
                Button bSw = new Button(this);
                bSw.setText("Pokémon Sword" + (isSword() ? "  ●" : ""));
                bSw.setOnClickListener(v -> { setVersion("sword"); renderStep(); });
                vSel.addView(bSh); vSel.addView(bSw);
                wizBody.addView(vSel);
                para(t("This guide sets the game up from your own dump, asking for one thing at a time:\n\n1. Your console's keys (prod.keys)\n2. The base game (.nsp or .xci)\n3. Update 1.3.2 (.nsp), unless your base file already includes it\n4. DLC (optional)\n\nNothing is uploaded: your files and keys stay on this phone. The conversion takes a long time on a phone, so keep it plugged in with the screen on.",
                       "Esta guía instala el juego a partir de tu propio dump y te pide un archivo a la vez:\n\n1. Las claves de tu consola (prod.keys)\n2. El juego base (.nsp o .xci)\n3. La actualización 1.3.2 (.nsp), salvo que tu archivo base ya la incluya\n4. DLC (opcional)\n\nNo se sube nada: tus archivos y claves se quedan en este teléfono. La conversión tarda mucho en un teléfono: déjalo conectado al cargador y con la pantalla encendida."));
                para(t("Free space on this phone: ", "Espacio libre en este teléfono: ") + gb(new StatFs(gameDir().getPath()).getAvailableBytes()));
                if (setUp() && !chainCurrent()) para(t("This app version translates the game's shaders differently, so the game has to be installed again. Your save data is kept.",
                                                      "Esta versión de la app traduce los shaders de otra forma, así que hay que instalar el juego otra vez. Tus partidas se conservan."));
                next = t("Start", "Comenzar"); break;
            case W_ACCESS:
                head(t("Allow access to files", "Permitir acceso a los archivos"));
                para(t("The game files are several GB. With access to all files the app reads them where they are instead of copying them.\n\nTap the button, allow the access, and come back here.\n\nYou can skip this step, but then every file you choose is copied into the app first (it needs extra free space and time).",
                       "Los archivos del juego pesan varios GB. Con acceso a todos los archivos, la app los lee donde están en lugar de copiarlos.\n\nToca el botón, permite el acceso y vuelve aquí.\n\nPuedes omitir este paso, pero entonces cada archivo que elijas se copiará primero dentro de la app (necesita más espacio libre y tiempo)."));
                button(wizBody, t("Open settings", "Abrir ajustes"), v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
                next = t("Skip", "Omitir"); break;
            case W_KEYS:
                head(t("Console keys", "Claves de la consola"));
                para(t("Choose prod.keys, dumped from your own console (for example with Lockpick_RCM). If your dump needs it, choose title.keys too: you can select both files at once.",
                       "Elige prod.keys, extraído de tu propia consola (por ejemplo con Lockpick_RCM). Si tu dump lo necesita, elige también title.keys: puedes seleccionar los dos archivos a la vez."));
                para((keys ? "✓ prod.keys" : "• prod.keys: " + t("not chosen yet", "aún sin elegir")) + "\n" + (titleKeys ? "✓ title.keys" : "• title.keys: " + t("optional", "opcional")));
                choose(keys ? t("Replace keys", "Cambiar claves") : t("Choose keys", "Elegir claves"), PICK_KEYS, true);
                ok = keys; break;
            case W_BASE:
                head(t("Base game (" + gameTitle() + ")", "Juego base (" + gameTitle() + ")"));
                para(t("Choose the " + gameTitle() + " base game file (.nsp or .xci).", "Elige el archivo del juego base de " + gameTitle() + " (.nsp o .xci)."));
                para(nameOrNone(baseGame, t("• Not chosen yet", "• Aún sin elegir")));
                choose(baseGame == null ? t("Choose base game", "Elegir juego base") : t("Change", "Cambiar"), PICK_BASE, false);
                ok = baseGame != null; break;
            case W_UPDATE:
                head(t("Update 1.3.2 (" + gameTitle() + ")", "Actualización 1.3.2 (" + gameTitle() + ")"));
                para(t("This app needs " + gameTitle() + " version 1.3.2. Choose the update file (.nsp).\n\nSkip this step only if your base file is a dump that already includes 1.3.2: the installer checks the version and tells you if the update is missing.",
                       "Esta app necesita " + gameTitle() + " versión 1.3.2. Elige el archivo de la actualización (.nsp).\n\nOmite este paso solo si tu archivo base ya incluye la 1.3.2: el instalador revisa la versión y te avisa si falta la actualización."));
                para(nameOrNone(updateGame, t("• Not chosen", "• Sin elegir")));
                choose(updateGame == null ? t("Choose update", "Elegir actualización") : t("Change", "Cambiar"), PICK_UPDATE, false);
                if (updateGame == null) next = t("Skip", "Omitir"); break;
            case W_DLC:
                head(t("DLC (" + gameTitle() + ", optional)", "DLC (" + gameTitle() + ", opcional)"));
                para(t("Isle of Armor / Crown Tundra DLC (.nsp) for " + gameTitle() + ". You can select several files at once, or skip this step.", "DLC de Isle of Armor / Crown Tundra (.nsp) para " + gameTitle() + ". Puedes seleccionar varios archivos a la vez u omitir este paso."));
                { StringBuilder sb = new StringBuilder(); for (String d : dlcGames) sb.append("✓ ").append(new File(d).getName()).append('\n');
                  para(dlcGames.isEmpty() ? t("• None chosen", "• Ninguno elegido") : sb.toString().trim()); }
                choose(t("Add DLC files", "Añadir DLC"), PICK_DLC, true);
                if (!dlcGames.isEmpty()) button(wizBody, t("Remove the DLC", "Quitar los DLC"), v -> { for (String d : dlcGames) dropImport(d); dlcGames.clear(); renderStep(); });
                else next = t("Skip", "Omitir");
                break;
            case W_OPTIONS:
                head(t("Options", "Opciones"));
                para(t("The recommended settings for this phone are selected. You can change them later in the app's settings.", "Se eligieron los ajustes recomendados para este teléfono. Podrás cambiarlos después en los ajustes de la app."));
                displayOptions(wizBody); driverRows(wizBody); fillDriverText(); controllerOptions(wizBody);
                break;
            default:   // W_INSTALL
                if (installedNow) {
                    head(t("Done", "Listo"));
                    para(t("✓ " + gameTitle() + " is installed. Press Play.", "✓ " + gameTitle() + " está instalado. Pulsa Jugar."));
                    button(wizBody, t("View installation log", "Ver registro de instalación"), v -> showLogDialog());
                    next = t("Play", "Jugar"); break;
                }
                head(t("Ready to install " + gameTitle(), "Listo para instalar " + gameTitle()));
                long need = sizeOf(baseGame) + sizeOf(updateGame); for (String d : dlcGames) need += sizeOf(d);
                long free = new StatFs(gameDir().getPath()).getAvailableBytes();
                para("✓ prod.keys" + (titleKeys ? " + title.keys" : "") + "\n" + t("Target game: ", "Juego a instalar: ") + gameTitle() + "\n" + t("Base game: ", "Juego base: ") + nameOrNone(baseGame, "-") + "\n" + t("Update: ", "Actualización: ") + nameOrNone(updateGame, t("none (the base file must include 1.3.2)", "ninguna (el archivo base debe incluir la 1.3.2)"))
                     + "\nDLC: " + dlcGames.size() + "\n" + t("Free space: ", "Espacio libre: ") + gb(free));
                if (free < need + (2L << 30)) para(t("⚠ Free space may not be enough: the installer unpacks the game's files (about " + gb(need) + " here). Free some space or it may fail.",
                                                    "⚠ Puede que no alcance el espacio libre: el instalador descomprime los archivos del juego (unos " + gb(need) + " aquí). Libera espacio o podría fallar."));
                para(t("It can take a long time. Keep this screen open and the phone plugged in.", "Puede tardar mucho. Mantén esta pantalla abierta y el teléfono conectado al cargador."));
                button(wizBody, t("View setup log", "Ver registro de instalación"), v -> showLogDialog());
                next = running ? t("Installing...", "Instalando...") : installFailed ? t("Try again", "Reintentar") : t("Install", "Instalar");
                ok = keys && baseGame != null; break;
        }
        wizBack.setEnabled(!running && step > W_WELCOME && !installedNow);
        wizNext.setText(next); wizNext.setEnabled(ok && !running);
    }

    private void pick(int what, boolean multi) {
        if (running) { status.setText(t("Please wait for the current setup or file copy to finish.", "Espera a que termine la instalación o la copia de archivos en curso.")); return; }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multi);
        startActivityForResult(i, what);
    }
    // the file for one role of the install: base game, update or DLC
    private void assign(int role, String path) {
        if (role == PICK_BASE) { dropImport(baseGame); baseGame = path; }
        else if (role == PICK_UPDATE) { dropImport(updateGame); updateGame = path; }
        else if (!dlcGames.contains(path)) dlcGames.add(path);
    }
    private void dropImport(String path) {   // a copy the guide made into the app's storage that is no longer wanted
        if (path == null) return;
        for (File f : new ArrayList<>(importedGames)) if (f.getPath().equals(path)) { f.delete(); importedGames.remove(f); }
    }
    // "prod.keys" (the console's keys: master_key_xx = hex ...), "title.keys" (rights ID = title key, 32 hex digits each) or null when the file is not a keys file, whatever it is called
    private static String classifyKeys(File f) {
        if (f.length() > (8L << 20)) return null;
        int lines = 0; boolean prod = false, title = false;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String l;
            while ((l = r.readLine()) != null) {
                int eq = l.indexOf('='); if (eq <= 0) continue;
                String k = l.substring(0, eq).trim(), v = l.substring(eq + 1).trim();
                if (k.isEmpty() || !v.matches("[0-9A-Fa-f]+")) continue;
                lines++;
                if (k.startsWith("master_key") || k.startsWith("aes_kek_generation_source") || k.equals("header_key")) prod = true;
                else if (k.length() == 32 && k.matches("[0-9A-Fa-f]+")) title = true;
            }
        } catch (IOException e) { return null; }
        return lines == 0 ? null : prod ? "prod.keys" : title ? "title.keys" : null;
    }
    private String titleHint(String name) {
        String ver = detectGameVersion(null, name);
        if ("sword".equals(ver)) return " " + t("(Pokémon Sword)", "(Pokémon Espada)");
        if ("shield".equals(ver)) return " " + t("(Pokémon Shield)", "(Pokémon Escudo)");
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("0100[0-9A-Fa-f]{12}").matcher(name);
        if (!m.find()) return "";
        String id = m.group().toUpperCase();
        if (id.startsWith("01008DB008C2") || id.startsWith("0100ABF00896")) return "";
        return " " + t("(warning: title ID " + m.group() + " is neither Pokémon Shield nor Sword)",
                       "(aviso: el title ID " + m.group() + " no es de Pokémon Shield ni Pokémon Sword)");
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
            logSetup("[PICK] Request=" + req + ", file=" + name + ", URI=" + u);
            if (req == PICK_DRIVER) { installDriver(u, name); continue; }
            if (req == PICK_MOD) {
                String safe = new File(name).getName();
                if (!safe.toLowerCase().endsWith(".pchtxt")) { status.setText(safe + " is not a .pchtxt patch"); continue; }
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(new File(modsDir(), safe))) {
                    copy(in, out); status.setText("Installed patch " + safe + ".");
                    logSetup("[PICK_MOD] Installed patch: " + safe);
                } catch (IOException e) {
                    status.setText("could not read " + safe + ": " + e.getMessage());
                    logSetup("[PICK_MOD] Error reading patch: " + e);
                }
                continue;
            }
            if (req == PICK_KEYS) {   // copied to a temporary file first: what it holds, not its name, says which keys file it is
                long size = documentSize(u);
                logSetup("[PICK_KEYS] Selected " + name + " (" + size + " bytes)");
                if (size > (8L << 20)) {
                    String err = t(name + " is too big to be a keys file (prod.keys is a few KB). Choose prod.keys.", name + " pesa demasiado para ser un archivo de claves (prod.keys pesa pocos KB). Elige prod.keys.");
                    status.setText(err); logSetup("[PICK_KEYS] Error: " + err); continue;
                }
                File tmp = new File(keysDir(), "incoming.tmp");
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(tmp)) { copy(in, out); }
                catch (IOException | SecurityException e) {
                    tmp.delete(); status.setText("could not read " + name + ": " + e.getMessage());
                    logSetup("[PICK_KEYS] Error copying incoming keys: " + e); continue;
                }
                String kind = classifyKeys(tmp);
                if (kind == null) {
                    tmp.delete();
                    String err = t(name + " does not look like a keys file (prod.keys has lines like master_key_00 = ...). Choose prod.keys.", name + " no parece un archivo de claves (prod.keys tiene líneas como master_key_00 = ...). Elige prod.keys.");
                    status.setText(err); logSetup("[PICK_KEYS] Error: " + err); continue;
                }
                File to = new File(keysDir(), kind); to.delete();
                if (!tmp.renameTo(to)) { tmp.delete(); status.setText("could not save " + kind); logSetup("[PICK_KEYS] Could not rename to " + kind); continue; }
                status.setText(t("Saved " + kind + " in the app's private storage.", "Se guardó " + kind + " en el almacenamiento privado de la app."));
                logSetup("[PICK_KEYS] Saved " + kind + " (" + to.length() + " bytes)");
                continue;
            }
            // base game, update or DLC
            String low = name == null ? "" : name.toLowerCase();
            if (!low.endsWith(".nsp") && !low.endsWith(".xci")) {
                String err = t(name + " is not a .nsp or .xci file.", name + " no es un archivo .nsp o .xci.");
                status.setText(err); logSetup("[PICK_GAME] Error: " + err); continue;
            }
            long sz = documentSize(u);
            logSetup("[PICK_GAME] File: " + name + " (size: " + sz + " bytes / " + gb(Math.max(0, sz)) + ")");
            String detected = detectGameVersion(u, name);
            logSetup("[PICK_GAME] Detected version: " + detected);
            if (detected != null) setVersion(detected);
            String p = pathOf(u);
            logSetup("[PICK_GAME] Resolved path: " + (p != null ? p : "(null - fallback to app copy)"));
            if (p == null) imports.add(new PickedGame(u, name, req)); else assign(req, p);
            status.setText(t("Selected ", "Seleccionado ") + name + titleHint(name));
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
                        String cand = readableFile(new File(root, relative).getPath());
                        if (cand != null) return cand;
                    }
                }
            } catch (IllegalArgumentException e) { }
        }
        try (Cursor c = getContentResolver().query(u, new String[] { "_data" }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int column = c.getColumnIndex("_data");
                if (column >= 0) {
                    String cand = readableFile(c.getString(column));
                    if (cand != null) return cand;
                }
            }
        } catch (IllegalArgumentException | SecurityException e) { }
        String name = nameOf(u);
        if (name != null) {
            long size = documentSize(u);
            File[] candidates = {
                new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), name),
                new File("/storage/emulated/0/Download", name),
                new File("/storage/emulated/0/Downloads", name),
                new File("/storage/emulated/0/Documents", name),
                new File(Environment.getExternalStorageDirectory(), name)
            };
            for (File c : candidates) {
                if (c.isFile() && c.canRead() && (size <= 0 || c.length() == size)) {
                    logSetup("[PATH] Direct match found for " + name + ": " + c.getPath());
                    return c.getPath();
                }
            }
        }
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
        logSetup("[IMPORT] importGames started for " + imports.size() + " file(s)");
        status.setText(t("Copying " + imports.size() + " selected file" + (imports.size() == 1 ? "" : "s") + " into app storage; large files need extra free space...",
                         "Copiando " + imports.size() + " archivo" + (imports.size() == 1 ? "" : "s") + " seleccionado" + (imports.size() == 1 ? "" : "s") + " al almacenamiento de la app; los archivos grandes necesitan espacio libre extra..."));
        new Thread(() -> {
            String error = null;
            try {
                File dir = new File(gameDir(), "imported");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create app storage for the selected game file at " + dir.getPath());
                for (PickedGame picked : imports) {
                    logSetup("[IMPORT] Copying " + picked.name + " (role=" + picked.role + ", uri=" + picked.uri + ")");
                    File destination = uniqueImportFile(dir, picked.name);
                    long size = documentSize(picked.uri);
                    long available = new StatFs(dir.getPath()).getAvailableBytes();
                    logSetup("[IMPORT] " + picked.name + ": size=" + size + " bytes (" + gb(Math.max(0, size)) + "), storage available=" + available + " bytes (" + gb(available) + ")");
                    if (size >= 0 && available < size + (512L << 20)) {
                        String noSpace = "not enough free space to copy " + picked.name + "; available: " + gb(available) + ", required: " + gb(size + (512L << 20));
                        logSetup("[IMPORT] ERROR: " + noSpace);
                        throw new IOException(noSpace);
                    }
                    runOnUiThread(() -> status.setText(t("Copying ", "Copiando ") + picked.name + " (" + (size >= 0 ? size / (1L << 20) + " MB" : "size unknown") + ")..."));
                    try (InputStream in = getContentResolver().openInputStream(picked.uri);
                         OutputStream out = new BufferedOutputStream(new FileOutputStream(destination))) {
                        if (in == null) throw new IOException("the file provider could not open " + picked.name);
                        byte[] buffer = new byte[1 << 20]; int n;
                        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                    } catch (IOException | SecurityException e) {
                        logSetup("[IMPORT] Error copying stream for " + picked.name + ": " + e);
                        destination.delete();
                        throw e;
                    }
                    if (!destination.isFile() || destination.length() == 0) {
                        destination.delete();
                        throw new IOException("the selected file was empty or could not be copied: " + picked.name);
                    }
                    picked.dest = destination;
                    logSetup("[IMPORT] Copied " + picked.name + " successfully to " + destination.getPath() + " (" + destination.length() + " bytes)");
                }
            } catch (Exception e) {
                error = e.getMessage();
                logSetup("[IMPORT] Exception during import: " + e);
            }
            final String failure = error;
            runOnUiThread(() -> {
                running = false;
                int copied = 0;
                for (PickedGame picked : imports) if (picked.dest != null) {
                    String detected = detectGameVersion(picked.dest);
                    if (detected != null) setVersion(detected);
                    assign(picked.role, picked.dest.getPath());
                    importedGames.add(picked.dest);
                    copied++;
                }
                if (failure == null) {
                    status.setText(t("Copied into app storage and selected.", "Copiado al almacenamiento de la app y seleccionado."));
                    logSetup("[IMPORT] All files copied and assigned successfully.");
                } else {
                    status.setText(t("Could not import game file: " + failure, "Error al importar archivo: " + failure));
                    logSetup("[IMPORT] Failed: " + failure);
                    showLogDialog();
                }
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
        i.setType("application/zip"); i.putExtra(Intent.EXTRA_TITLE, isSword() ? "PokemonSword-save.zip" : "PokemonShield-save.zip");
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
        File verSave = new File(gameDir(), "save_" + currentVersion());
        final File save = (!verSave.exists() && !"sword".equalsIgnoreCase(currentVersion())) ? new File(gameDir(), "save") : verSave;
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

    private void startSetup() {
        if (!new File(keysDir(), "prod.keys").exists() || baseGame == null) {
            String m = t("Choose the keys and the base game first.", "Elige primero las claves y el juego base.");
            status.setText(m);
            logSetup("[SETUP] Cannot start: " + m);
            return;
        }
        installedNow = installFailed = false;
        bar.setMax(600); bar.setProgress(0); status.setText(t("Preparing...", "Preparando..."));
        logSetup("==================== START SETUP ====================");
        logSetup("Target version: " + currentVersion() + " (isSword=" + isSword() + ")");
        logSetup("Base game: " + baseGame + " (exists=" + new File(baseGame).exists() + ", size=" + new File(baseGame).length() + ")");
        logSetup("Update game: " + (updateGame != null ? updateGame + " (exists=" + new File(updateGame).exists() + ", size=" + new File(updateGame).length() + ")" : "none"));
        for (String d : dlcGames) logSetup("DLC: " + d + " (exists=" + new File(d).exists() + ", size=" + new File(d).length() + ")");
        List<String> cmd = new ArrayList<>();
        cmd.add("--keys"); cmd.add(keysDir().getPath()); cmd.add("--base"); cmd.add(baseGame);
        if (updateGame != null) { cmd.add("--update"); cmd.add(updateGame); }
        for (String d : dlcGames) { cmd.add("--dlc"); cmd.add(d); }
        cmd.add("--game"); cmd.add(isSword() ? "0100ABF008968000" : "01008DB008C2C000");
        runSetup(cmd, err -> {
            bar.setProgress(err == null ? 600 : bar.getProgress());
            installedNow = err == null; installFailed = err != null;
            if (err == null) {
                status.setText(t("Ready. The setup log is in Downloads/" + setupLogFile(currentVersion()).getName(),
                                 "¡Listo! El registro completo está en Descargas/" + setupLogFile(currentVersion()).getName()));
                logSetup("[SETUP] Completed successfully!");
                for (File file : importedGames) file.delete();
                importedGames.clear();
                new File(gameDir(), "imported").delete();
                baseGame = updateGame = null; dlcGames.clear();
                if (gameIcon() != null) pinShortcut();
            } else {
                status.setText(t("Setup failed: " + err + "\n(Detailed log saved to Downloads: " + setupLogFile(currentVersion()).getName() + ")",
                                 "Error en la instalación: " + err + "\n(Registro detallado guardado en Descargas: " + setupLogFile(currentVersion()).getName() + ")"));
                logSetup("[SETUP] ERROR: " + err);
            }
        });
    }
    // shaders the game saved on an earlier run (nvn_capture/<key>.code) and no setup has translated yet (captures_done lists those already done)
    private List<String> pendingCaptures() { return pendingCaptures(gameDir()); }
    static List<String> pendingCaptures(File gameDir) {   // GameActivity asks too
        String[] all = new File(gameDir, "nvn_capture").list((d, n) -> n.endsWith(".code")); List<String> out = new ArrayList<>(); if (all == null) return out;
        Set<String> done = new HashSet<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(gameDir, "captures_done")))) { String l; while ((l = r.readLine()) != null) done.add(l.trim()); } catch (IOException e) { }
        for (String n : all) if (!done.contains(n)) out.add(n);
        return out;
    }
    private void startCaptures() {
        List<String> todo = pendingCaptures(); if (todo.isEmpty()) return;
        bar.setMax(300); bar.setProgress(0); status.setText("Adding " + todo.size() + " shader" + (todo.size() == 1 ? "" : "s") + " the game made last time...");
        runSetup(new ArrayList<>(Arrays.asList("--captures", new File(gameDir(), "nvn_capture").getPath(), "--game", isSword() ? "0100ABF008968000" : "01008DB008C2C000")), err -> {
            if (err != null) { status.setText("Could not add the new shaders (" + err + "); the game still plays without them."); return; }
            try (FileWriter w = new FileWriter(new File(gameDir(), "captures_done"), true)) { for (String n : todo) w.write(n + "\n"); } catch (IOException e) { }
            startGame();
        });
    }
    // setup_game.py with the given arguments (plus the folders every run needs): progress from its "STEP n/total what" lines (100 bar units a step) and, inside a step,
    // its "PROGRESS done/total" lines (the shader translation, hours on a phone); done(null) or done(the reason it failed) on the UI thread.
    // The screen stays on meanwhile: once it goes dark Android slows or stops the work
    private void runSetup(List<String> args, Consumer<String> done) {
        running = true; refresh();
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            String err = null; int[] stepNo = {1}; String[] stepName = {""};
            List<String> lastOutputLines = new ArrayList<>();
            try {
                File py = unpackPython(), dm = unpackScripts(), bin = binDir();
                File work = new File(gameDir(), "setup_work"); work.mkdirs();
                List<String> cmd = new ArrayList<>();
                String lib = getApplicationInfo().nativeLibraryDir;
                cmd.add(lib + "/libpython_exe.so"); cmd.add(new File(dm, "android/tools/setup_game.py").getPath());
                cmd.addAll(args);
                cmd.add("--out"); cmd.add(gameDir().getPath()); cmd.add("--work"); cmd.add(work.getPath()); cmd.add("--bin"); cmd.add(bin.getPath());
                File ext = new File(dm, "android/app/assets/nintendo_ext.ttf"); if (ext.exists() && !args.contains("--captures")) { cmd.add("--ext-font"); cmd.add(ext.getPath()); }
                logSetup("[RUN] Invoking command: " + cmd);
                ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true);
                pb.environment().put("PYTHONHOME", py.getPath()); pb.environment().put("PYTHONPATH", new File(dm, "android/pyshim").getPath());
                pb.environment().put("DM_GAME", isSword() ? "0100ABF008968000" : "01008DB008C2C000");
                pb.environment().put("DM_GAME_VERSION", isSword() ? "sword" : "shield");
                pb.environment().put("LD_LIBRARY_PATH", bin.getPath() + ":" + lib); pb.environment().put("LIBCAPSTONE_PATH", lib); pb.environment().put("NX_NATIVE_LIB_DIR", lib);
                pb.environment().put("DOTNET_OPENSSL_VERSION_OVERRIDE", "3"); pb.environment().put("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1");
                pb.environment().put("TMPDIR", getCacheDir().getPath()); pb.environment().put("J", String.valueOf(Math.max(2, Runtime.getRuntime().availableProcessors())));
                Process p = pb.start();
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream())); String ln;
                while ((ln = r.readLine()) != null) {
                    final String l = ln;
                    logSetup("[PROC] " + l);
                    synchronized (lastOutputLines) {
                        if (lastOutputLines.size() > 50) lastOutputLines.remove(0);
                        lastOutputLines.add(l);
                    }
                    if (l.startsWith("STEP ")) {
                        String[] f = l.split(" ", 3); int n = Integer.parseInt(f[1].split("/")[0]); stepNo[0] = n; stepName[0] = f[2];
                        runOnUiThread(() -> { bar.setProgress((n - 1) * 100); status.setText(f[2] + "..."); });
                    } else if (l.startsWith("PROGRESS ")) {
                        String[] f = l.substring(9).trim().split("/"); long a = Long.parseLong(f[0]), b = Math.max(1, Long.parseLong(f[1])); int pct = (int) Math.min(100, a * 100 / b), n = stepNo[0]; String s = stepName[0];
                        runOnUiThread(() -> { bar.setProgress((n - 1) * 100 + pct); status.setText(s + "... " + pct + "%\nThis takes a long time on a phone: keep this screen open, ideally plugged in."); });
                    } else if (l.startsWith("FAILED")) {
                        err = l.substring(6).trim();
                    } else if (err != null) {
                        err += "\n" + l;
                    }
                }
                int exitCode = p.waitFor();
                logSetup("[PROC] Process exited with code: " + exitCode);
                if (exitCode != 0 && err == null) {
                    StringBuilder sb = new StringBuilder();
                    synchronized (lastOutputLines) {
                        int start = Math.max(0, lastOutputLines.size() - 8);
                        for (int k = start; k < lastOutputLines.size(); k++) {
                            if (sb.length() > 0) sb.append("\n");
                            sb.append(lastOutputLines.get(k));
                        }
                    }
                    err = sb.length() > 0 ? sb.toString() : "setup stopped (exit code " + exitCode + ")";
                }
            } catch (Exception e) {
                err = e.toString();
                logSetup("[PROC] Exception: " + e);
            }
            final String e2 = err;
            runOnUiThread(() -> { getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); running = false; done.accept(e2); refresh(); });
        }).start();
    }
    private File iconFile() {
        File f = new File(gameDir(), isSword() ? "icon_sword.jpg" : "icon_shield.jpg");
        return f.exists() ? f : new File(gameDir(), "icon.jpg");
    }
    // the game's icon from the player's dump: this screen and the recent-apps card
    @SuppressWarnings("deprecation")
    private Bitmap gameIcon() {
        Bitmap b = null;
        if (inApk()) {
            try (InputStream is = getAssets().open("game/icon.jpg")) { b = BitmapFactory.decodeStream(is); } catch (Exception e) { }
        }
        if (b == null && iconFile().exists()) b = BitmapFactory.decodeFile(iconFile().getPath());
        if (b == null) return null;
        setTaskDescription(new ActivityManager.TaskDescription(gameTitle(), b));
        if (logo != null) { logo.setImageBitmap(b); logo.setVisibility(android.view.View.VISIBLE); }
        return b;
    }
    // once set up the app is the game: it starts straight into it. A long press on its icon offers "Set up again" (another dump, an update, the layout of the setup)
    private void setupShortcut() {
        ShortcutManager sm = getSystemService(ShortcutManager.class); if (sm == null) return;
        Intent again = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN).putExtra("setup", true);
        ShortcutInfo si = new ShortcutInfo.Builder(this, "setup").setShortLabel("Settings").setLongLabel("Settings and setup")
            .setIcon(Icon.createWithResource(this, android.R.drawable.ic_menu_manage)).setIntent(again).build();
        List<ShortcutInfo> all = new ArrayList<>();
        all.add(si);
        if (hasShield()) {
            Intent shGo = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN).putExtra("game_version", "shield");
            File shIco = new File(gameDir(), "icon_shield.jpg"); if (!shIco.exists()) shIco = new File(gameDir(), "icon.jpg");
            Icon ic = shIco.exists() ? Icon.createWithBitmap(BitmapFactory.decodeFile(shIco.getPath())) : Icon.createWithResource(this, android.R.drawable.ic_media_play);
            all.add(new ShortcutInfo.Builder(this, "play_shield").setShortLabel("Shield").setLongLabel("Play Pokémon Shield").setIcon(ic).setIntent(shGo).build());
        }
        if (hasSword()) {
            Intent swGo = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN).putExtra("game_version", "sword");
            File swIco = new File(gameDir(), "icon_sword.jpg"); if (!swIco.exists()) swIco = new File(gameDir(), "icon.jpg");
            Icon ic = swIco.exists() ? Icon.createWithBitmap(BitmapFactory.decodeFile(swIco.getPath())) : Icon.createWithResource(this, android.R.drawable.ic_media_play);
            all.add(new ShortcutInfo.Builder(this, "play_sword").setShortLabel("Sword").setLongLabel("Play Pokémon Sword").setIcon(ic).setIntent(swGo).build());
        }
        try { sm.setDynamicShortcuts(all); } catch (Exception e) { }
    }
    // a home screen shortcut with the game's icon (the launcher asks the player to confirm), also offered on a long press of the app's icon
    private void pinShortcut(String ver) {
        boolean isSw = "sword".equalsIgnoreCase(ver);
        String name = isSw ? "Pokémon Sword" : "Pokémon Shield";
        File icFile = new File(gameDir(), isSw ? "icon_sword.jpg" : "icon_shield.jpg");
        if (!icFile.exists()) icFile = new File(gameDir(), "icon.jpg");
        Bitmap b = icFile.exists() ? BitmapFactory.decodeFile(icFile.getPath()) : gameIcon();
        ShortcutManager sm = getSystemService(ShortcutManager.class); if (b == null || sm == null) return;
        Intent go = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN).putExtra("game_version", isSw ? "sword" : "shield");
        String sid = isSw ? "game_sword" : "game_shield";
        ShortcutInfo si = new ShortcutInfo.Builder(this, sid).setShortLabel(name).setLongLabel("Play " + name).setIcon(Icon.createWithBitmap(b)).setIntent(go).build();
        try { sm.addDynamicShortcuts(Arrays.asList(si)); } catch (Exception e) { }
        for (ShortcutInfo p : sm.getPinnedShortcuts()) if (p.getId().equals(sid)) { sm.updateShortcuts(Arrays.asList(si)); status.setText(name + " shortcut is on the home screen."); return; }
        if (sm.isRequestPinShortcutSupported()) sm.requestPinShortcut(si, null); else status.setText("This launcher cannot add shortcuts: long-press the app's icon for " + name + ".");
    }
    private void pinShortcut() { pinShortcut(currentVersion()); }
    private void syncPatchedFiles() {
        try {
            File filesDir = gameDir();
            File commonDir = new File(filesDir, "patched");
            String[] tags = { "sword", "shield" };
            for (String tag : tags) {
                File verDir = new File(filesDir, "patched_" + tag);
                if (verDir.exists() && verDir.isDirectory()) {
                    File mainFixups = new File(verDir, "main.fixups.tsv");
                    if (!mainFixups.exists() && commonDir.exists() && commonDir.isDirectory()) {
                        File commonFixups = new File(commonDir, "main.fixups.tsv");
                        if (commonFixups.exists()) {
                            File[] list = commonDir.listFiles();
                            if (list != null) {
                                for (File src : list) {
                                    if (src.isFile()) {
                                        File dst = new File(verDir, src.getName());
                                        if (!dst.exists() || dst.length() == 0) {
                                            try (FileInputStream in = new FileInputStream(src);
                                                 FileOutputStream out = new FileOutputStream(dst)) {
                                                copy(in, out);
                                            } catch (Exception ignored) { }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            File activeVerDir = new File(filesDir, "patched_" + currentVersion());
            if (activeVerDir.exists() && activeVerDir.isDirectory() && commonDir.exists() && commonDir.isDirectory()) {
                File activeFixups = new File(activeVerDir, "main.fixups.tsv");
                if (activeFixups.exists()) {
                    File commonFixups = new File(commonDir, "main.fixups.tsv");
                    if (!commonFixups.exists()) {
                        File[] list = activeVerDir.listFiles();
                        if (list != null) {
                            for (File src : list) {
                                if (src.isFile()) {
                                    File dst = new File(commonDir, src.getName());
                                    if (!dst.exists() || dst.length() == 0) {
                                        try (FileInputStream in = new FileInputStream(src);
                                             FileOutputStream out = new FileOutputStream(dst)) {
                                            copy(in, out);
                                        } catch (Exception ignored) { }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } catch (Exception ignored) { }
    }
    private void startGame() {
        setVersion(currentVersion());
        syncPatchedFiles();
        GameActivity.viaSetup = true;
        startActivity(new Intent(this, GameActivity.class));
        finish();
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
        File stamp = new File(to, ".unpacked");
        boolean matchesVer = false;
        try (BufferedReader r = new BufferedReader(new FileReader(stamp))) {
            if (Long.parseLong(r.readLine().trim()) == ver) matchesVer = true;
        } catch (Exception ignored) { }

        // Sanity check: if it's dm.zip, ensure both game profiles are actually present on disk!
        if (matchesVer && "dm.zip".equals(asset)) {
            File swProfile = new File(to, "games/0100ABF008968000/game.tsv");
            File shProfile = new File(to, "games/01008DB008C2C000/game.tsv");
            if (!swProfile.exists() || !shProfile.exists()) {
                logSetup("[UNPACK] dm.zip stamp matched version " + ver + " but game profile(s) missing; forcing re-unpack");
                matchesVer = false;
            }
        }

        if (matchesVer) return to;

        logSetup("[UNPACK] Unpacking " + asset + " to " + to.getPath() + " (ver=" + ver + ")...");
        to.mkdirs();
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(getAssets().open(asset)))) { ZipEntry e;
            while ((e = z.getNextEntry()) != null) { File f = new File(to, e.getName()); if (!f.getCanonicalPath().startsWith(to.getCanonicalPath())) continue;
                if (e.isDirectory()) { f.mkdirs(); continue; } f.getParentFile().mkdirs(); try (OutputStream o = new FileOutputStream(f)) { copy(z, o); } } }
        try (FileWriter w = new FileWriter(stamp)) { w.write(ver + "\n"); }
        logSetup("[UNPACK] Successfully unpacked " + asset);
        return to;
    }
}
