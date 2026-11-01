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
import android.view.KeyEvent;
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
    private TextView status, wizTitle; private ProgressBar bar; private Button play, shortcut, saveExport, saveImport, wizBack, wizNext; private ImageView logo;
    private LinearLayout wizBody;
    private boolean wizard, installedNow, installFailed, defaultsDone; private int step;
    private volatile boolean running;
    private static String t(String en, String es) { return "es".equals(java.util.Locale.getDefault().getLanguage()) ? es : en; }   // the guide's texts: English, Spanish on Spanish phones
    private static String gb(long bytes) { return String.format(java.util.Locale.US, "%.1f GB", bytes / 1073741824.0); }
    private File gameDir() { return getExternalFilesDir(null); }
    private File modsDir() { File d = new File(gameDir(), "mods"); d.mkdirs(); return d; }
    private File keysDir() { File d = new File(getFilesDir(), "keys"); d.mkdirs(); return d; }
    private boolean inApk() {
        try (InputStream is = getAssets().open("game/ready")) { return is != null; } catch (IOException e) { return false; }
    }
    private boolean setUp() { return inApk() || (new File(gameDir(), "ready").exists() && new File(gameDir(), "shaders.pak").exists()); }
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
            try (BufferedReader r = new BufferedReader(new FileReader(new File(gameDir(), "ready")))) { have = r.readLine(); } catch (IOException e) { return false; }
        }
        return want != null && have != null && want.trim().equals(have.trim());
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        boolean asked = getIntent().getBooleanExtra("setup", false);
        if (b != null) {   // the guide's progress survives the process being recreated while a picker is open
            wizard = b.getBoolean("wizard"); step = b.getInt("step"); baseGame = b.getString("base"); updateGame = b.getString("update");
            String[] d = b.getStringArray("dlc"); if (d != null) dlcGames.addAll(Arrays.asList(d));
            String[] im = b.getStringArray("imported"); if (im != null) for (String p : im) importedGames.add(new File(p));
        }
        if (ready()) setupShortcut();
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
        TextView title = new TextView(this); title.setText("Pokémon Shield"); title.setTextSize(28); root.addView(title);
        return root;
    }
    private void show(LinearLayout root) { ScrollView sv = new ScrollView(this); sv.addView(root); setContentView(sv); }
    private TextView label(LinearLayout p, String s, int size, int top) { TextView v = new TextView(this); v.setText(s); v.setTextSize(size); v.setPadding(0, top, 0, 8); p.addView(v); return v; }

    // the set-up game: Play, the settings, the guide again, save data
    private void showDashboard() {
        wizard = false; wizBody = null; wizTitle = null; wizBack = wizNext = null;
        LinearLayout root = page();
        play = button(root, "Play", v -> startGame()); play.setEnabled(ready());
        status = new TextView(this); status.setPadding(0, 16, 0, 16); root.addView(status);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(600); root.addView(bar);
        settings(root);
        button(root, t("Run the installation guide again", "Repetir la guía de instalación"), v -> { step = W_WELCOME; installedNow = installFailed = false; showWizard(); });
        saveExport = button(root, "Export save data", v -> chooseSaveExport());
        saveImport = button(root, "Import edited save data", v -> chooseSaveImport());
        shortcut = button(root, "Add Pokémon Shield to the home screen", v -> pinShortcut());
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
            { "60", t("60 FPS: smooth, uses more battery (Shield 1.3.2)", "60 FPS: fluido, gasta más batería (Shield 1.3.2)") },
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
        modsText = row(root, "Code patches (.pchtxt)", "Choose", v -> pick(PICK_MOD, true));
        button(root, "Clear code patches", v -> { deleteTree(modsDir()); modsDir(); refresh(); });
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
        shortcut.setVisibility(iconFile().exists() ? android.view.View.VISIBLE : android.view.View.GONE);
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
                head(t("Install Pokémon Shield", "Instalar Pokémon Shield"));
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
                head(t("Base game", "Juego base"));
                para(t("Choose the Pokémon Shield base game file (.nsp or .xci).", "Elige el archivo del juego base de Pokémon Shield (.nsp o .xci)."));
                para(nameOrNone(baseGame, t("• Not chosen yet", "• Aún sin elegir")));
                choose(baseGame == null ? t("Choose base game", "Elegir juego base") : t("Change", "Cambiar"), PICK_BASE, false);
                ok = baseGame != null; break;
            case W_UPDATE:
                head(t("Update 1.3.2", "Actualización 1.3.2"));
                para(t("This app needs Pokémon Shield version 1.3.2. Choose the update file (.nsp).\n\nSkip this step only if your base file is a dump that already includes 1.3.2: the installer checks the version and tells you if the update is missing.",
                       "Esta app necesita Pokémon Shield versión 1.3.2. Elige el archivo de la actualización (.nsp).\n\nOmite este paso solo si tu archivo base ya incluye la 1.3.2: el instalador revisa la versión y te avisa si falta la actualización."));
                para(nameOrNone(updateGame, t("• Not chosen", "• Sin elegir")));
                choose(updateGame == null ? t("Choose update", "Elegir actualización") : t("Change", "Cambiar"), PICK_UPDATE, false);
                if (updateGame == null) next = t("Skip", "Omitir"); break;
            case W_DLC:
                head(t("DLC (optional)", "DLC (opcional)"));
                para(t("Isle of Armor / Crown Tundra DLC (.nsp). You can select several files at once, or skip this step.", "DLC de Isle of Armor / Crown Tundra (.nsp). Puedes seleccionar varios archivos a la vez u omitir este paso."));
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
                    para(t("✓ Pokémon Shield is installed. Press Play.", "✓ Pokémon Shield está instalado. Pulsa Jugar."));
                    next = t("Play", "Jugar"); break;
                }
                head(t("Ready to install", "Listo para instalar"));
                long need = sizeOf(baseGame) + sizeOf(updateGame); for (String d : dlcGames) need += sizeOf(d);
                long free = new StatFs(gameDir().getPath()).getAvailableBytes();
                para("✓ prod.keys" + (titleKeys ? " + title.keys" : "") + "\n" + t("Base game: ", "Juego base: ") + nameOrNone(baseGame, "-") + "\n" + t("Update: ", "Actualización: ") + nameOrNone(updateGame, t("none (the base file must include 1.3.2)", "ninguna (el archivo base debe incluir la 1.3.2)"))
                     + "\nDLC: " + dlcGames.size() + "\n" + t("Free space: ", "Espacio libre: ") + gb(free));
                if (free < need + (2L << 30)) para(t("⚠ Free space may not be enough: the installer unpacks the game's files (about " + gb(need) + " here). Free some space or it may fail.",
                                                    "⚠ Puede que no alcance el espacio libre: el instalador descomprime los archivos del juego (unos " + gb(need) + " aquí). Libera espacio o podría fallar."));
                para(t("It can take a long time. Keep this screen open and the phone plugged in.", "Puede tardar mucho. Mantén esta pantalla abierta y el teléfono conectado al cargador."));
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
    private String titleHint(String name) {   // a title ID in the file name that is not Shield's (01008DB008C2C000 base, ...800 update, ...D00x DLC)
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("0100[0-9A-Fa-f]{12}").matcher(name);
        return m.find() && !m.group().toUpperCase().startsWith("01008DB008C2") ? " " + t("(warning: its title ID " + m.group() + " is not Pokémon Shield's 01008DB008C2C000)", "(aviso: su title ID " + m.group() + " no es el de Pokémon Shield, 01008DB008C2C000)") : "";
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
            if (req == PICK_MOD) {
                String safe = new File(name).getName();
                if (!safe.toLowerCase().endsWith(".pchtxt")) { status.setText(safe + " is not a .pchtxt patch"); continue; }
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(new File(modsDir(), safe))) { copy(in, out); status.setText("Installed patch " + safe + "."); }
                catch (IOException e) { status.setText("could not read " + safe + ": " + e.getMessage()); }
                continue;
            }
            if (req == PICK_KEYS) {   // copied to a temporary file first: what it holds, not its name, says which keys file it is
                long size = documentSize(u);
                if (size > (8L << 20)) { status.setText(t(name + " is too big to be a keys file (prod.keys is a few KB). Choose prod.keys.", name + " pesa demasiado para ser un archivo de claves (prod.keys pesa pocos KB). Elige prod.keys.")); continue; }
                File tmp = new File(keysDir(), "incoming.tmp");
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(tmp)) { copy(in, out); }
                catch (IOException | SecurityException e) { tmp.delete(); status.setText("could not read " + name + ": " + e.getMessage()); continue; }
                String kind = classifyKeys(tmp);
                if (kind == null) { tmp.delete(); status.setText(t(name + " does not look like a keys file (prod.keys has lines like master_key_00 = ...). Choose prod.keys.", name + " no parece un archivo de claves (prod.keys tiene líneas como master_key_00 = ...). Elige prod.keys.")); continue; }
                File to = new File(keysDir(), kind); to.delete();
                if (!tmp.renameTo(to)) { tmp.delete(); status.setText("could not save " + kind); continue; }
                status.setText(t("Saved " + kind + " in the app's private storage.", "Se guardó " + kind + " en el almacenamiento privado de la app."));
                continue;
            }
            // base game, update or DLC
            String low = name == null ? "" : name.toLowerCase();
            if (!low.endsWith(".nsp") && !low.endsWith(".xci")) { status.setText(t(name + " is not a .nsp or .xci file.", name + " no es un archivo .nsp o .xci.")); continue; }
            String p = pathOf(u);
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
        status.setText(t("Copying " + imports.size() + " selected file" + (imports.size() == 1 ? "" : "s") + " into app storage; large files need extra free space...",
                         "Copiando " + imports.size() + " archivo" + (imports.size() == 1 ? "" : "s") + " seleccionado" + (imports.size() == 1 ? "" : "s") + " al almacenamiento de la app; los archivos grandes necesitan espacio libre extra..."));
        new Thread(() -> {
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
                    runOnUiThread(() -> status.setText(t("Copying ", "Copiando ") + picked.name + " (" + (size >= 0 ? size / (1L << 20) + " MB" : "size unknown") + ")..."));
                    try (InputStream in = getContentResolver().openInputStream(picked.uri);
                         OutputStream out = new BufferedOutputStream(new FileOutputStream(destination))) {
                        if (in == null) throw new IOException("the file provider could not open " + picked.name);
                        byte[] buffer = new byte[1 << 20]; int n;
                        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                    } catch (IOException | SecurityException e) { destination.delete(); throw e; }
                    if (!destination.isFile() || destination.length() == 0) { destination.delete(); throw new IOException("the selected file was empty or could not be copied: " + picked.name); }
                    picked.dest = destination;
                }
            } catch (IOException | SecurityException | IllegalArgumentException e) { error = e.getMessage(); }
            final String failure = error;
            runOnUiThread(() -> {
                running = false;
                int copied = 0;
                for (PickedGame picked : imports) if (picked.dest != null) { assign(picked.role, picked.dest.getPath()); importedGames.add(picked.dest); copied++; }
                if (failure == null) status.setText(t("Copied into app storage and selected.", "Copiado al almacenamiento de la app y seleccionado."));
                else status.setText("Could not import game file: " + failure + (copied == 0 ? "" : " Previously copied selections are ready."));
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

    private void startSetup() {
        if (!new File(keysDir(), "prod.keys").exists() || baseGame == null) { status.setText(t("Choose the keys and the base game first.", "Elige primero las claves y el juego base.")); return; }
        installedNow = installFailed = false;
        bar.setMax(600); bar.setProgress(0); status.setText(t("Preparing...", "Preparando..."));
        List<String> cmd = new ArrayList<>();
        cmd.add("--keys"); cmd.add(keysDir().getPath()); cmd.add("--base"); cmd.add(baseGame);
        if (updateGame != null) { cmd.add("--update"); cmd.add(updateGame); }
        for (String d : dlcGames) { cmd.add("--dlc"); cmd.add(d); }
        runSetup(cmd, err -> {
            bar.setProgress(err == null ? 600 : bar.getProgress());
            installedNow = err == null; installFailed = err != null;
            status.setText(err == null ? "Ready. The setup log is in Android/data/" + getPackageName() + "/files/setup_work." : "Setup failed: " + err);
            if (err == null) {
                for (File file : importedGames) file.delete();
                importedGames.clear();
                new File(gameDir(), "imported").delete();
                baseGame = updateGame = null; dlcGames.clear();
                if (gameIcon() != null) pinShortcut();
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
        runSetup(new ArrayList<>(Arrays.asList("--captures", new File(gameDir(), "nvn_capture").getPath())), err -> {
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
                    if (l.startsWith("STEP ")) { String[] f = l.split(" ", 3); int n = Integer.parseInt(f[1].split("/")[0]); stepNo[0] = n; stepName[0] = f[2];
                        runOnUiThread(() -> { bar.setProgress((n - 1) * 100); status.setText(f[2] + "..."); }); }
                    else if (l.startsWith("PROGRESS ")) {
                        String[] f = l.substring(9).trim().split("/"); long a = Long.parseLong(f[0]), b = Math.max(1, Long.parseLong(f[1])); int pct = (int) Math.min(100, a * 100 / b), n = stepNo[0]; String s = stepName[0];
                        runOnUiThread(() -> { bar.setProgress((n - 1) * 100 + pct); status.setText(s + "... " + pct + "%\nThis takes a long time on a phone: keep this screen open, ideally plugged in."); }); }
                    else if (l.startsWith("FAILED")) err = l.substring(6).trim(); }
                if (p.waitFor() != 0 && err == null) err = "setup stopped (" + p.exitValue() + ")";
            } catch (Exception e) { err = e.toString(); }
            final String e2 = err;
            runOnUiThread(() -> { getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); running = false; done.accept(e2); refresh(); });
        }).start();
    }
    private File iconFile() { return new File(gameDir(), "icon.jpg"); }
    // the game's icon from the player's dump: this screen and the recent-apps card
    @SuppressWarnings("deprecation")
    private Bitmap gameIcon() {
        Bitmap b = null;
        if (inApk()) {
            try (InputStream is = getAssets().open("game/icon.jpg")) { b = BitmapFactory.decodeStream(is); } catch (Exception e) { }
        }
        if (b == null && iconFile().exists()) b = BitmapFactory.decodeFile(iconFile().getPath());
        if (b == null) return null;
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
    private void startGame() { GameActivity.viaSetup = true; startActivity(new Intent(this, GameActivity.class)); finish(); }
}
