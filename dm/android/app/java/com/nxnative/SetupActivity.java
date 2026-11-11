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
import android.content.Context;
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
import android.widget.Toast;
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
    // static: the setup runs in its own thread and outlives this screen; a new screen (Android may still make one, despite configChanges in the manifest, which keeps it
    // across rotation: #42) must not start a second setup next to it
    private static volatile boolean running;
    private String unofficial() { String[] app = GameSettings.app(this); return T.t(this, "Unofficial build: not supported. Official %s releases come only from %s/releases.", app[0], app[1]); }

    private File gameDir() { return GameSettings.dir(this); }   // the chosen game's folder (GameSettings)
    private String[] game() { return GameSettings.game(this); }
    private String gameName() { return game()[1]; }
    // What the engine noted about the last run (android_main.c boot_guard, notice.txt: kind TAB text): mods switched off after a crash (told, the game still starts);
    // a crash starting the graphics on the phone's own driver (a dialog offering a driver package for this GPU; returns true: the game is not started straight away)
    private boolean notices() {
        File f = new File(gameDir(), "notice.txt"); if (!f.exists()) return false;
        List<String[]> ns = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) { String l; while ((l = r.readLine()) != null) { String[] k = l.split("\t", 2); if (k.length == 2) ns.add(k); } } catch (IOException e) { }
        f.delete(); boolean block = false;
        for (String[] n : ns) {
            if (n[0].equals("mods")) Toast.makeText(this, n[1], Toast.LENGTH_LONG).show();
            else if (n[0].equals("driver")) { block = true; final String gpu = n[1];
                new android.app.AlertDialog.Builder(this).setTitle(T.t(this, "The game closed while starting its graphics"))
                    .setMessage(T.t(this, "Last time, %s closed while starting its graphics on this phone's own driver (%s). ", gameName(), gpu) + driverAdvice(this, gpu))
                    .setPositiveButton(T.t(this, "Choose a driver"), (d, w) -> pick(PICK_DRIVER, false))
                    .setNeutralButton(T.t(this, "Start anyway"), (d, w) -> startGame())
                    .setNegativeButton(T.t(this, "Close"), null).show(); }
        }
        return block;
    }
    // (translated: the dialog came half in English in the Portuguese app, #82)
    private static String driverAdvice(android.content.Context c, String gpu) {
        String g = gpu.toLowerCase(java.util.Locale.ROOT);
        if (g.contains("adreno")) return T.t(c, "A recent Mesa Turnip package for Adreno usually fixes this: choose its .zip as the graphics driver.");
        if (g.matches(".*mali-g(57|68|77|78|52|72|76).*")) return T.t(c, "FristOneRR's PanVK driver supports this Mali: download its .zip from the developer's GitHub Releases and choose it as the graphics driver.");
        return T.t(c, "No replacement driver supports this GPU yet. Please report it with the log in Download (the -log-previous.txt file), so the app can be fixed for it.");
    }
    // a game set up (its folder has the ready file and the shader pack): the dual launcher's list
    private boolean isReady(String id) {
        File d = id.equalsIgnoreCase(GameSettings.games(this).get(0)[0]) ? GameSettings.root(this) : new File(GameSettings.root(this), id);
        return new File(d, "ready").exists() && new File(d, "shaders.pak").exists();
    }
    private List<String> readyGames() { List<String> r = new ArrayList<>(); for (String[] g : GameSettings.games(this)) if (isReady(g[0])) r.add(g[0]); return r; }
    private File keysDir() { File d = new File(getFilesDir(), "keys"); d.mkdirs(); return d; }
    private boolean setUp() { return new File(gameDir(), "ready").exists() && new File(gameDir(), "shaders.pak").exists(); }
    // set up for this app version: the shader pack's translation chain (the ready file) is the one the engine was built for (assets/shader_chain, build_apk.py);
    // an update that changes the chain otherwise starts the game with a pack the engine refuses (every 3D shader "SPIR-V missing")
    private boolean ready() { return setUp() && chainCurrent(); }
    private boolean chainCurrent() {
        String want, have;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("shader_chain")))) { want = r.readLine(); } catch (IOException e) { return true; }   // an older build without the asset
        try (BufferedReader r = new BufferedReader(new FileReader(new File(gameDir(), "ready")))) { have = r.readLine(); } catch (IOException e) { return false; }
        return want != null && have != null && want.trim().equals(have.trim());
    }

    @Override protected void onCreate(Bundle b) {
        if (GameActivity.alive) {   // the game is running (in the background): the icon goes back to it
            super.onCreate(b); startActivity(new Intent(this, GameActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)); finish(); return; }
        super.onCreate(b);
        if (getIntent().getStringExtra("menu") != null) {   // am start ... --es menu 1: the in-game settings panel over this screen (a look at it without the game, over adb)
            setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE); setContentView(new android.view.View(this));
            getWindow().getDecorView().post(() -> GameMenu.show(this, 0)); return;
        }
        // official release or not (BuildSigner): into env.txt every start, so the game's log says which; an unofficial build is told so before the game starts
        String[] signers = BuildSigner.digests(this); boolean official = BuildSigner.official(signers);
        envSet("DM_BUILD_SIGNER", BuildSigner.label(signers));
        if (!official) Toast.makeText(this, unofficial(), Toast.LENGTH_LONG).show();
        boolean asked = getIntent().getBooleanExtra("setup", false);
        // which game: a game's home screen shortcut names it; from the app's icon, with more than one game set up, this screen lets the player choose (the dual launcher)
        String want = getIntent().getStringExtra("game");
        if (want != null) GameSettings.choose(this, want);
        // the app's icon with two games: the launcher, once one of them is set up; before that (the first start after installing) the setup screen, which says what to do
        else if (!running && !asked && GameSettings.games(this).size() > 1 && !readyGames().isEmpty()) { launcher(official); return; }
        else if (!running && !asked) { List<String> ready = readyGames(); if (ready.size() == 1) GameSettings.choose(this, ready.get(0)); else if (ready.size() > 1) asked = true; }
        cleanLeftovers();   // (before the game may start straight away: a stopped setup's leftovers, in the background)
        if (ready()) setupShortcut();
        if (ready() && !asked && !running && notices()) asked = true;   // the last run crashed starting its graphics: this screen offers a driver instead of starting the game
        if (ready() && !asked && !running) { gameIcon(); if (!pendingWork(gameDir())) { startGame(); return; } }
        setupScreen(official);
        if (ready() && !asked) startCaptures();
    }
    // The setup screen, in the launcher's look: what a player sees first after installing. Three numbered steps (game files, keys, set up), then Play once the game is
    //   set up; the rest (which game, save data, the home screen shortcut, the graphics driver) under them, the driver folded away until asked for
    private float dp;
    private LinearLayout more, saveBox; private Button clear, anyway; private volatile boolean offerAnyway;
    private void setupScreen(boolean official) {
        dp = getResources().getDisplayMetrics().density;
        if (getActionBar() != null) getActionBar().hide();
        boolean first = readyGames().isEmpty(), many = GameSettings.games(this).size() > 1; int acc = accent(game()[0]);
        getWindow().setStatusBarColor(0xFF0B0C10); getWindow().setNavigationBarColor(0xFF0B0C10);
        restoreImported();
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(px(20), px(28), px(20), px(28));
        logo = new ImageView(this); logo.setVisibility(android.view.View.GONE); logo.setScaleType(ImageView.ScaleType.CENTER_CROP); logo.setClipToOutline(true);
        android.graphics.drawable.GradientDrawable lc = new android.graphics.drawable.GradientDrawable(); lc.setCornerRadius(18 * dp); logo.setBackground(lc);
        root.addView(logo, new LinearLayout.LayoutParams(px(112), px(112)));
        // the first start: the app's name and the games it runs; later, the game this screen sets up
        TextView title = text(root, first && many ? getApplicationInfo().loadLabel(getPackageManager()).toString() : gameName(), 26, 0xFFF2F3F5, true); title.setGravity(Gravity.CENTER);
        title.setPadding(0, px(10), 0, 0);
        if (!official) { TextView u = text(root, unofficial(), 13, 0xffff5252, false); u.setGravity(Gravity.CENTER); }
        TextView note = text(root, first ? T.t(this, "Welcome! To play, set up %s from your own Switch dump. It takes three steps.", many ? gamesNamed() : gameName())
                                           + "\n" + T.t(this, "No game files or keys come with this app; yours never leave the phone.")
                                         : T.t(this, "Set the game up from your own dump. No game files or keys come with this app; yours never leave the phone."), 14, 0xFFA9AFBA, false);
        note.setGravity(Gravity.CENTER); note.setPadding(0, px(6), 0, px(6));
        // the author, linked to the official page (a copy uploaded elsewhere keeps it unless the APK is changed, and a changed APK says "Unofficial build" above)
        TextView by = text(root, "SimipoB", 13, acc, false); by.setGravity(Gravity.CENTER); by.setPadding(0, 0, 0, px(6));
        by.setPaintFlags(by.getPaintFlags() | android.graphics.Paint.UNDERLINE_TEXT_FLAG);
        by.setOnClickListener(v -> { try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/simipoB/Galar-Native-Android"))); } catch (Exception e) { } });
        LinearLayout c1 = card(root, "1", T.t(this, "Your game files"), acc);
        text(c1, T.t(this, "The base game (.nsp or .xci), the %s update and any DLC. You can choose them all at once.", GameSettings.version(this)), 13, 0xFFA9AFBA, false);
        gameText = text(c1, "", 14, 0xFFF2F3F5, false); gameText.setPadding(0, px(8), 0, px(8));
        LinearLayout r1 = hrow(c1); pill(r1, T.t(this, "Choose game files"), acc, 0xFFFFFFFF, v -> pick(PICK_GAME, true));
        clear = pill(r1, T.t(this, "Clear"), 0xFF2A2E37, 0xFFC4C9D2, v -> clearGames());
        LinearLayout c2 = card(root, "2", T.t(this, "Your keys"), acc);
        text(c2, T.t(this, "prod.keys from your own Switch (and title.keys if your dump needs it)."), 13, 0xFFA9AFBA, false);
        keysText = text(c2, "", 14, 0xFFF2F3F5, false); keysText.setPadding(0, px(8), 0, px(8));
        pill(c2, T.t(this, "Choose keys"), acc, 0xFFFFFFFF, v -> pick(PICK_KEYS, true));
        LinearLayout c3 = card(root, "3", T.t(this, "Set up"), acc);
        text(c3, T.t(this, "This takes a long time on a phone: keep this screen open, ideally plugged in."), 13, 0xFFA9AFBA, false);
        setup = pill(c3, first ? T.t(this, "Set up the game") : T.t(this, "Set up again"), acc, 0xFFFFFFFF, v -> startSetup(false));
        ((LinearLayout.LayoutParams) setup.getLayoutParams()).topMargin = px(10);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(600); bar.setProgressTintList(android.content.res.ColorStateList.valueOf(acc));
        c3.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        status = text(c3, "", 14, 0xFFF2F3F5, false); status.setPadding(0, px(8), 0, 0);
        anyway = pill(c3, T.t(this, "Set up anyway"), 0xFF2A2E37, 0xFFF2F3F5, v -> new android.app.AlertDialog.Builder(this).setTitle(T.t(this, "Set up anyway?"))
            .setMessage(T.t(this, "The damaged parts of your files are kept as they are. The game may crash or show glitches where it uses them. Copying your files again in full is the real fix."))
            .setPositiveButton(T.t(this, "Set up anyway"), (d, w) -> startSetup(true)).setNegativeButton(T.t(this, "Cancel"), null).show());
        ((LinearLayout.LayoutParams) anyway.getLayoutParams()).topMargin = px(8); anyway.setVisibility(android.view.View.GONE);
        play = pill(root, T.t(this, "Play"), acc, 0xFFFFFFFF, v -> startGame()); play.setTextSize(18);
        ((LinearLayout.LayoutParams) play.getLayoutParams()).topMargin = px(16);
        // the rest
        more = new LinearLayout(this); more.setOrientation(LinearLayout.VERTICAL); root.addView(more, new LinearLayout.LayoutParams(-1, -2));
        if (many && !first) {   // the game switcher: each game has its own setup, saves, settings and mods (setup itself goes by what the files are)
            LinearLayout cg = card(more, null, T.t(this, "Game"), acc);
            android.widget.RadioGroup gs = new android.widget.RadioGroup(this); gs.setOrientation(LinearLayout.VERTICAL);
            List<String[]> all = GameSettings.games(this); String cur = game()[0];
            for (int k = 0; k < all.size(); k++) { android.widget.RadioButton rb = radio(gs, 7000 + k, all.get(k)[1] + (isReady(all.get(k)[0]) ? "" : " " + T.t(this, "(not set up)")), acc);
                if (all.get(k)[0].equals(cur)) rb.setChecked(true); }
            gs.setOnCheckedChangeListener((g, id) -> { if (running) { status.setText(T.t(this, "Wait for the current setup to finish.")); return; }
                GameSettings.choose(this, all.get(id - 7000)[0]); Intent it = getIntent(); it.putExtra("setup", true); it.removeExtra("game"); recreate(); });
            cg.addView(gs);
        }
        saveBox = card(more, null, T.t(this, "Save data"), acc);
        LinearLayout r4 = hrow(saveBox);
        saveExport = pill(r4, T.t(this, "Export save data"), 0xFF2A2E37, 0xFFF2F3F5, v -> chooseSaveExport());
        saveImport = pill(r4, T.t(this, "Import edited save data"), 0xFF2A2E37, 0xFFF2F3F5, v -> chooseSaveImport());
        shortcut = pill(saveBox, T.t(this, "Add %s to the home screen", gameName()), 0xFF2A2E37, 0xFFF2F3F5, v -> pinShortcut());
        ((LinearLayout.LayoutParams) shortcut.getLayoutParams()).topMargin = px(8);
        // the Vulkan driver: the phone's own, or an Adreno driver package (a zip with meta.json and the library, e.g. Mesa Turnip) opened through libadrenotools;
        // most players never need it, so it stays folded away unless a driver package is in use
        LinearLayout cd = card(more, null, T.t(this, "Graphics driver"), acc);
        driverText = text(cd, "", 14, 0xFFF2F3F5, false);
        LinearLayout drv = new LinearLayout(this); drv.setOrientation(LinearLayout.VERTICAL);
        LinearLayout r5 = hrow(drv); pill(r5, T.t(this, "Choose"), 0xFF2A2E37, 0xFFF2F3F5, v -> pick(PICK_DRIVER, false));
        pill(r5, T.t(this, "Use the phone's own driver"), 0xFF2A2E37, 0xFFF2F3F5, v -> { DriverPackages.useSystem(this); refresh(); });
        boolean custom = !envGet("DM_VK_DRIVER").isEmpty(); drv.setVisibility(custom ? android.view.View.VISIBLE : android.view.View.GONE);
        TextView open = text(cd, T.t(this, "Change the driver (only if the game does not start)"), 13, acc, true); open.setPadding(0, px(8), 0, px(4));
        open.setVisibility(custom ? android.view.View.GONE : android.view.View.VISIBLE);
        open.setOnClickListener(v -> { drv.setVisibility(android.view.View.VISIBLE); open.setVisibility(android.view.View.GONE); });
        cd.addView(drv); ((LinearLayout.LayoutParams) r5.getLayoutParams()).topMargin = px(8);
        maliOptions(drv);
        TextView sn = text(more, T.t(this, "Settings (picture, graphics, textures, controller buttons, touch controls, mods) are in the game: tap the menu button at the top left, or hold − on a controller for 3 seconds for the controller buttons."), 13, 0xFF8A909B, false);
        sn.setPadding(px(4), px(16), px(4), 0);
        ScrollView sv = new ScrollView(this); sv.setFillViewport(true); sv.addView(root);
        sv.setBackground(new android.graphics.drawable.GradientDrawable(android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM, new int[] { mix(acc, 0xFF0B0C10, 0.8f), 0xFF0B0C10, 0xFF0B0C10 }));
        setContentView(sv);
        refresh(); gameIcon();
    }
    private int px(float v) { return (int) (v * dp); }
    private String gamesNamed() { List<String> n = new ArrayList<>(); for (String[] g : GameSettings.games(this)) n.add(g[1]); return String.join(" " + T.t(this, "or") + " ", n); }
    // the game's colour: the launcher's cards, this screen's buttons
    static int accent(String id) { String up = id.toUpperCase(java.util.Locale.ROOT);
        return up.startsWith("0100ABF") ? 0xFF2E9BFF : up.startsWith("01008DB") ? 0xFFE0306E : up.startsWith("01008F6") ? 0xFF9B5CFF : up.startsWith("0100A3D") ? 0xFFFF4D3D : 0xFF3DDC97; }
    private TextView text(LinearLayout parent, String s, float size, int color, boolean bold) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(size); t.setTextColor(color); if (bold) t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        parent.addView(t, new LinearLayout.LayoutParams(-1, -2)); return t;
    }
    // a step: a rounded panel with its number in the game's colour, and its title
    private LinearLayout card(LinearLayout parent, String num, String title, int acc) {
        LinearLayout c = new LinearLayout(this); c.setOrientation(LinearLayout.VERTICAL); c.setPadding(px(16), px(14), px(16), px(16));
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable(); bg.setColor(0xE61C1F26); bg.setCornerRadius(18 * dp); bg.setStroke(px(1), 0x33FFFFFF);
        c.setBackground(bg);
        LinearLayout h = new LinearLayout(this); h.setOrientation(LinearLayout.HORIZONTAL); h.setGravity(Gravity.CENTER_VERTICAL); h.setPadding(0, 0, 0, px(6));
        if (num != null) { TextView n = new TextView(this); n.setText(num); n.setTextSize(14); n.setTextColor(0xFFFFFFFF); n.setTypeface(android.graphics.Typeface.DEFAULT_BOLD); n.setGravity(Gravity.CENTER);
            android.graphics.drawable.GradientDrawable nb = new android.graphics.drawable.GradientDrawable(); nb.setShape(android.graphics.drawable.GradientDrawable.OVAL); nb.setColor(acc); n.setBackground(nb);
            LinearLayout.LayoutParams nl = new LinearLayout.LayoutParams(px(26), px(26)); nl.rightMargin = px(10); h.addView(n, nl); }
        TextView t = new TextView(this); t.setText(title); t.setTextSize(17); t.setTextColor(0xFFF2F3F5); t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD); h.addView(t);
        c.addView(h);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = px(12); parent.addView(c, lp); return c;
    }
    private LinearLayout hrow(LinearLayout parent) { LinearLayout r = new LinearLayout(this); r.setOrientation(LinearLayout.HORIZONTAL); r.setGravity(Gravity.CENTER_VERTICAL);
        parent.addView(r, new LinearLayout.LayoutParams(-1, -2)); return r; }
    // a button in the launcher's style: rounded, filled, no capitals; dimmed while it cannot be used (on())
    private Button pill(LinearLayout parent, String s, int bg, int fg, android.view.View.OnClickListener l) {
        Button b = new Button(this); b.setText(s); b.setAllCaps(false); b.setTextSize(15); b.setTextColor(fg); b.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        b.setStateListAnimator(null); b.setMinHeight(px(44)); b.setMinimumHeight(px(44)); b.setPadding(px(16), px(8), px(16), px(8));
        android.graphics.drawable.GradientDrawable d = new android.graphics.drawable.GradientDrawable(); d.setCornerRadius(22 * dp); d.setColor(bg); b.setBackground(d);
        b.setOnClickListener(l);
        boolean inRow = parent.getOrientation() == LinearLayout.HORIZONTAL;
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(inRow ? 0 : -1, -2, inRow ? 1 : 0); if (inRow && parent.getChildCount() > 0) lp.leftMargin = px(8);
        parent.addView(b, lp); return b;
    }
    private static void on(android.view.View v, boolean enabled) { v.setEnabled(enabled); v.setAlpha(enabled ? 1f : 0.4f); }
    private android.widget.RadioButton radio(android.widget.RadioGroup g, int id, String s, int acc) {
        android.widget.RadioButton rb = new android.widget.RadioButton(this); rb.setId(id); rb.setText(s); rb.setTextColor(0xFFF2F3F5);
        rb.setButtonTintList(android.content.res.ColorStateList.valueOf(acc)); g.addView(rb); return rb;
    }
    // back from setup opened from the launcher: the launcher again, not out of the app
    @Override public void onBackPressed() {
        if (status != null && !running && GameSettings.games(this).size() > 1 && !readyGames().isEmpty()) { startActivity(new Intent(this, SetupActivity.class)); finish(); return; }
        super.onBackPressed();
    }
    // The launcher: the app's games side by side (their own icon, name and colour), one tap to play; a game not set up yet opens its setup. Setup and the save tools
    //   are one tap further ("Set up or manage a game"), so the screen the player sees every day is just the games
    private void launcher(boolean official) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        if (getActionBar() != null) getActionBar().hide();   // full screen: no title bar, no system bars (Immersive), the games fill it
        Immersive.apply(this);
        float dp = getResources().getDisplayMetrics().density;
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics(); int sw = Math.max(dm.widthPixels, dm.heightPixels), sh = Math.min(dm.widthPixels, dm.heightPixels);
        List<String[]> all = GameSettings.games(this);
        int[] acc = new int[all.size()];
        for (int k = 0; k < all.size(); k++) acc[k] = accent(all.get(k)[0]);
        // the background: the games' colours, faint, from one side to the other over near-black
        android.graphics.drawable.GradientDrawable back = new android.graphics.drawable.GradientDrawable(android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
            new int[] { mix(acc[0], 0xFF0B0C10, 0.78f), 0xFF0B0C10, mix(acc[acc.length - 1], 0xFF0B0C10, 0.78f) });
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setGravity(Gravity.CENTER); root.setBackground(back);
        root.setPadding((int) (24 * dp), (int) (10 * dp), (int) (24 * dp), (int) (10 * dp));
        TextView title = new TextView(this); title.setText(getApplicationInfo().loadLabel(getPackageManager())); title.setTextSize(22); title.setTextColor(0xFFF2F3F5);
        title.setTypeface(android.graphics.Typeface.DEFAULT_BOLD); title.setGravity(Gravity.CENTER); root.addView(title);
        if (!official) { TextView u = new TextView(this); u.setText(unofficial()); u.setTextColor(0xffff5252); u.setTextSize(12); u.setGravity(Gravity.CENTER); root.addView(u); }
        // the cards share the screen: as wide as two fit with room around them, the icon as tall as the screen allows under the title and above the link
        int cardW = Math.min((sw - (int) (48 * dp)) / all.size() - (int) (28 * dp), (int) (sh * 0.62f));
        int iconS = Math.min(cardW - (int) (28 * dp), sh - (int) (210 * dp));
        LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL); row.setGravity(Gravity.CENTER); row.setPadding(0, (int) (10 * dp), 0, (int) (6 * dp));
        for (int k = 0; k < all.size(); k++) {
            String[] g = all.get(k); String id = g[0]; boolean ok = isReady(id); int accent = acc[k];
            LinearLayout card = new LinearLayout(this); card.setOrientation(LinearLayout.VERTICAL); card.setGravity(Gravity.CENTER_HORIZONTAL);
            card.setPadding((int) (14 * dp), (int) (14 * dp), (int) (14 * dp), (int) (14 * dp));
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable(); bg.setColor(0xE61C1F26); bg.setCornerRadius(22 * dp); bg.setStroke((int) (2 * dp), accent);
            card.setBackground(bg); card.setElevation(8 * dp);
            ImageView icon = new ImageView(this); icon.setScaleType(ImageView.ScaleType.CENTER_CROP); icon.setClipToOutline(true);
            android.graphics.drawable.GradientDrawable clip = new android.graphics.drawable.GradientDrawable(); clip.setCornerRadius(16 * dp); clip.setColor(0xFF2A2E37); icon.setBackground(clip);
            File d = k == 0 ? GameSettings.root(this) : new File(GameSettings.root(this), id);
            android.graphics.Bitmap bm = android.graphics.BitmapFactory.decodeFile(new File(d, "icon.jpg").getPath()); if (bm != null) icon.setImageBitmap(bm);
            card.addView(icon, new LinearLayout.LayoutParams(iconS, iconS));
            TextView name = new TextView(this); name.setText(g[1]); name.setTextSize(17); name.setTextColor(0xFFF2F3F5); name.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            name.setGravity(Gravity.CENTER); name.setSingleLine(true); name.setPadding(0, (int) (10 * dp), 0, (int) (8 * dp)); card.addView(name);
            TextView go = new TextView(this); go.setText(T.t(this, ok ? "Play" : "Set up")); go.setTextSize(15); go.setTextColor(ok ? 0xFFFFFFFF : 0xFF9AA0AB); go.setGravity(Gravity.CENTER);
            go.setTypeface(android.graphics.Typeface.DEFAULT_BOLD); go.setPadding(0, (int) (9 * dp), 0, (int) (9 * dp));
            android.graphics.drawable.GradientDrawable gb = new android.graphics.drawable.GradientDrawable(); gb.setCornerRadius(20 * dp); gb.setColor(ok ? accent : 0xFF262A33); go.setBackground(gb);
            card.addView(go, new LinearLayout.LayoutParams(iconS, -2));
            card.setOnClickListener(v -> { GameSettings.choose(this, id);   // Play goes the way a home screen shortcut goes, not straight to the game: captured shaders are translated into the pack first (pendingWork)
                if (ok) { Intent it = new Intent(this, SetupActivity.class); it.putExtra("game", id); startActivity(it); finish(); } else openSetup(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.leftMargin = lp.rightMargin = (int) (14 * dp); row.addView(card, lp);
        }
        root.addView(row);
        TextView manage = new TextView(this); manage.setText(T.t(this, "Set up or manage a game")); manage.setTextColor(0xFFC4C9D2); manage.setTextSize(14); manage.setGravity(Gravity.CENTER);
        manage.setPadding((int) (18 * dp), (int) (7 * dp), (int) (18 * dp), (int) (7 * dp));
        android.graphics.drawable.GradientDrawable mb = new android.graphics.drawable.GradientDrawable(); mb.setCornerRadius(16 * dp); mb.setColor(0x33FFFFFF); manage.setBackground(mb);
        manage.setOnClickListener(v -> openSetup());
        LinearLayout.LayoutParams ml = new LinearLayout.LayoutParams(-2, -2); ml.topMargin = (int) (6 * dp); root.addView(manage, ml);
        setContentView(root);
    }
    private void openSetup() { Intent it = new Intent(this, SetupActivity.class); it.putExtra("setup", true); startActivity(it); finish(); }
    private static int mix(int a, int b, float t) {   // a toward b by t (0..1), per channel
        int r = (int) (((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t), g = (int) (((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t), bl = (int) ((a & 255) * (1 - t) + (b & 255) * t);
        return 0xFF000000 | r << 16 | g << 8 | bl;
    }

    private String envGet(String key) { return GameSettings.get(this, key); }
    private void envSet(String key, String value) { GameSettings.set(this, key, value); }
    // Another driver: the pipeline cache the game kept (android_main.c DM_VK_PCACHE) was compiled by the old one. Drivers are meant to refuse a cache that is not
    // theirs, but an experimental one (PanVK) may not, so it starts empty
    private void driverChanged() { new File(gameDir(), "vk_pipeline_cache.bin").delete(); }
    // The Mali driver's switches (MaliDriver), shown while a PanVK package is the driver: env.txt, read before the driver opens, so they reach it as its environment
    private LinearLayout maliBox;
    private void maliOptions(LinearLayout root) {
        maliBox = new LinearLayout(this); maliBox.setOrientation(LinearLayout.VERTICAL); root.addView(maliBox);
        TextView h = new TextView(this); h.setText(T.t(this, "Mali driver (PanVK) options")); h.setTextColor(0xFFF2F3F5); h.setPadding(0, 16, 0, 0); maliBox.addView(h);
        check(maliBox, "Driver log (for bug reports)", MaliDriver.LOG, "1");
        check(maliBox, "Fix for the driver not starting (some Exynos and Mali-G52 phones)", MaliDriver.STRIDE, "56");
        check(maliBox, "Late stencil (fixes black triangles or missing light; can be darker and slower)", MaliDriver.STENCIL, "1");
        TextView hl = new TextView(this); hl.setText(T.t(this, "GPU memory reported to the game")); hl.setTextColor(0xFFF2F3F5); hl.setPadding(0, 8, 0, 0); maliBox.addView(hl);
        android.widget.RadioGroup rg = new android.widget.RadioGroup(this); String cur = envGet(MaliDriver.HEAP);
        for (int k = 0; k < MaliDriver.HEAP_MB.length; k++) {
            android.widget.RadioButton b = new android.widget.RadioButton(this); b.setId(3000 + k); b.setTextColor(0xFFF2F3F5);
            b.setText(k == 0 ? "Driver's choice (a quarter of the RAM)" : MaliDriver.HEAP_MB[k] + " MB" + (k == 2 ? " (fixes missing textures on 4 GB phones)" : "")); rg.addView(b);
            if (MaliDriver.HEAP_MB[k].equals(cur)) b.setChecked(true);
        }
        rg.setOnCheckedChangeListener((g, id) -> envSet(MaliDriver.HEAP, MaliDriver.HEAP_MB[id - 3000]));
        maliBox.addView(rg);
    }
    private void check(LinearLayout root, String label, String key, String on) {
        android.widget.CheckBox c = new android.widget.CheckBox(this); c.setText(label); c.setTextColor(0xFFF2F3F5); c.setChecked(on.equals(envGet(key)));
        c.setOnCheckedChangeListener((b, v) -> envSet(key, v ? on : "")); root.addView(c);
    }
    // a driver package into the app's private storage (libadrenotools loads drivers only from there): its meta.json names the library
    private void installDriver(Uri u, String name) { status.setText(DriverPackages.install(this, u, name)); refresh(); }   // (DriverPackages: shared with the in-game menu)
    private static void deleteTree(File f) { File[] k = f.listFiles(); if (k != null) for (File c : k) deleteTree(c); f.delete(); }
    private static long treeSize(File f) { if (f.isFile()) return f.length(); long s = 0; File[] k = f.listFiles(); if (k != null) for (File c : k) s += treeSize(c); return s; }
    // What a setup that stopped leaves behind (#47: 26 GB for a 10 GB game): its unpacked game in setup_work (about 12 GB; any later setup unpacks the game again, so it
    //   is never read once the app has restarted) and its scan, and the loose shader folder (vk/) of setups made before the shader pack. Not while a setup runs, nor
    //   within 10 minutes of its log changing (a setup still going); a translation a later "Set up again" can keep (translated.json, setup_game.py) stays
    private void cleanLeftovers() {
        if (running) return;
        final List<File> dirs = new ArrayList<>(); File root = GameSettings.root(this); List<String[]> gs = GameSettings.games(this);
        for (int i = 0; i < gs.size(); i++) dirs.add(i == 0 ? root : new File(root, gs.get(i)[0]));
        new Thread(() -> {
            long freed = 0;
            for (File game : dirs) {
                File work = new File(game, "setup_work"), log = new File(work, "setup.log");
                if (running || (log.isFile() && System.currentTimeMillis() - log.lastModified() < 10 * 60 * 1000L)) continue;
                boolean keepXlate = new File(work, "translated.json").isFile();
                File[] w = work.listFiles();
                if (w != null) for (File f : w) { String n = f.getName();
                    boolean drop = n.equals("extract") || n.equals("shader_pkg") || n.equals("cas") || n.equals("aoc") || n.startsWith("dmsh_")
                        || (!keepXlate && (n.equals("vk") || n.equals("shader_cache")));
                    if (drop && !running) { freed += treeSize(f); deleteTree(f); } }
                File vk = new File(game, "vk");
                if (vk.isDirectory() && new File(game, "shaders.pak").isFile() && new File(game, "ready").isFile() && !running) { freed += treeSize(vk); deleteTree(vk); }
            }
            if (freed > (64L << 20)) { final String gb = String.format(java.util.Locale.ROOT, "%.1f", freed / 1e9);
                runOnUiThread(() -> { if (status != null && status.getText().length() == 0) status.setText(T.t(this, "Freed %s GB left over from an earlier setup.", gb)); }); }
        }).start();
    }
    private void refresh() {
        if (driverText != null) { String d = envGet("DM_VK_DRIVER"); driverText.setText(d.isEmpty() ? T.t(this, "the phone's own Vulkan driver") : new File(d).getParentFile().getName() + " (" + new File(d).getName() + ")");
                                  if (maliBox != null) maliBox.setVisibility(MaliDriver.isPanvk(d) ? android.view.View.VISIBLE : android.view.View.GONE); }
        if (!running) { List<String> n = new ArrayList<>(); for (String g : games) n.add("✓ " + new File(g).getName());
            gameText.setText(games.isEmpty() ? T.t(this, "none chosen") : String.join("\n", n)); }
        boolean keys = new File(keysDir(), "prod.keys").exists();
        keysText.setText(keys ? "✓ prod.keys" + (new File(keysDir(), "title.keys").exists() ? " + title.keys" : "") + " " + T.t(this, "(kept in the app's private storage)") : T.t(this, "none chosen"));
        on(setup, !running && keys && !games.isEmpty()); on(play, !running && ready()); on(clear, !running && !games.isEmpty());
        play.setVisibility(ready() ? android.view.View.VISIBLE : android.view.View.GONE);
        on(saveExport, !running && setUp()); on(saveImport, !running && setUp());
        saveBox.setVisibility(setUp() || iconFile().exists() ? android.view.View.VISIBLE : android.view.View.GONE);
        if (running && status.getText().length() == 0)   // a new screen while the setup (or a copy) another one started still runs
            status.setText(T.t(this, "Setup is still running in the background. Wait a few minutes, then close and reopen the app to see whether it has finished."));
        if (!running && setUp() && !chainCurrent() && status.getText().length() == 0)
            status.setText(T.t(this, "This app version translates the game's shaders differently: choose your files and press Set up the game again (the save data is kept)."));
        shortcut.setVisibility(iconFile().exists() ? android.view.View.VISIBLE : android.view.View.GONE);
    }
    // all-files access, asked for once a screen: back from Android's settings the picker opens by itself; refused, the files chosen are copied into the app instead
    private boolean askedAccess, pickAfterAccess;
    @Override protected void onResume() {
        super.onResume();
        if (pickAfterAccess) { pickAfterAccess = false; pick(PICK_GAME, true); }
    }
    private void pick(int what, boolean multi) {
        if (running) { status.setText(T.t(this, "Please wait for the current setup or file copy to finish.")); return; }
        if (what == PICK_GAME && Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager() && !askedAccess) {   // read the dump in place: all-files access
            askedAccess = true; pickAfterAccess = true;
            status.setText(T.t(this, "Allow access to all files so the app can read your dump where it is (it is several GB; nothing is copied)."));
            try { startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))); return; }
            catch (android.content.ActivityNotFoundException e) { pickAfterAccess = false; } }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multi);
        startActivityForResult(i, what);
    }
    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (res != RESULT_OK) return;
        if (data == null) { status.setText(T.t(this, "The file picker returned no selected file. Try the phone's Files app and select a file from Downloads or Documents.")); return; }
        if (req == PICK_SAVE_EXPORT) { exportSave(data.getData()); return; }
        if (req == PICK_SAVE_IMPORT) { importSave(data.getData()); return; }
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) for (int k = 0; k < data.getClipData().getItemCount(); k++) uris.add(data.getClipData().getItemAt(k).getUri()); else if (data.getData() != null) uris.add(data.getData());
        if (uris.isEmpty()) { status.setText(T.t(this, "The file picker did not return a file. Try the phone's Files app and select a file from Downloads or Documents.")); return; }
        List<PickedGame> imports = new ArrayList<>(); List<String> problems = new ArrayList<>();
        for (Uri u : uris) {
            String name = nameOf(u);
            if (req == PICK_DRIVER) { installDriver(u, name); continue; }
            if (req == PICK_KEYS) { String why = installKeys(u, name); if (why != null) problems.add(why); }
            else {
                String why = gameFileProblem(u, name); if (why != null) { problems.add(why); continue; }
                String p = pathOf(u);
                if (p == null) imports.add(new PickedGame(u, name));
                else if (!games.contains(p)) games.add(p);
            }
        }
        refresh();
        if (!problems.isEmpty()) status.setText(String.join("\n", problems));
        else if (req == PICK_GAME && imports.isEmpty()) { String why = mixedGames(); status.setText(why != null ? why : ""); }
        if (!imports.isEmpty()) {
            importGames(imports);
        }
    }
    // a key file, by its contents: prod.keys names its keys (header_key, master_key_00, ...); title.keys is lines of title ID = title key. null, or why not
    private String installKeys(Uri u, String name) {
        byte[] b;
        try (InputStream in = getContentResolver().openInputStream(u)) {
            if (in == null) return T.t(this, "Could not read %s.", name);
            ByteArrayOutputStream o = new ByteArrayOutputStream(); byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) { o.write(buf, 0, n); if (o.size() > (4 << 20)) return T.t(this, "%s is not a key file: choose prod.keys (and title.keys).", name); }
            b = o.toByteArray();
        } catch (IOException | SecurityException e) { return T.t(this, "Could not read %s.", name) + " " + e.getMessage(); }
        String t = new String(b, java.nio.charset.StandardCharsets.ISO_8859_1).toLowerCase(java.util.Locale.ROOT);
        String to = t.contains("header_key") || t.contains("master_key_") || t.contains("key_area_key") ? "prod.keys"
                  : t.matches("(?s)\\s*[0-9a-f]{32}\\s*=\\s*[0-9a-f]{32}.*") ? "title.keys" : null;
        if (to == null) return T.t(this, "%s is not a key file: choose prod.keys (and title.keys).", name);
        try (OutputStream out = new FileOutputStream(new File(keysDir(), to))) { out.write(b); } catch (IOException e) { return T.t(this, "Could not read %s.", name) + " " + e.getMessage(); }
        return null;
    }
    // a game file the setup can read: an .nsp (PFS0) or .xci (HEAD at 0x100); compressed ones (.nsz / .xcz) and anything else are said so now, not after minutes of setup
    private String gameFileProblem(Uri u, String name) {
        String n = name == null ? "" : name.toLowerCase(java.util.Locale.ROOT);
        if (n.endsWith(".nsz") || n.endsWith(".xcz")) return T.t(this, "%s is compressed (NSZ): decompress it to .nsp / .xci first, then choose that.", name);
        if (n.endsWith(".keys") || n.endsWith(".txt")) return T.t(this, "%s is not a game file: choose it under Your keys.", name);
        byte[] h = new byte[0x10000]; int got = 0;
        try (InputStream in = getContentResolver().openInputStream(u)) {
            if (in == null) return null;   // (the copy or the setup says what is wrong)
            int r; while (got < h.length && (r = in.read(h, got, h.length - got)) > 0) got += r;
        } catch (IOException | SecurityException e) { return null; }
        boolean pfs0 = got >= 4 && h[0] == 'P' && h[1] == 'F' && h[2] == 'S' && h[3] == '0', xci = got >= 0x104 && h[0x100] == 'H' && h[0x101] == 'E' && h[0x102] == 'A' && h[0x103] == 'D';
        if (!pfs0 && !xci) return T.t(this, "%s is not a Switch game file (.nsp or .xci).", name);
        if (pfs0 && got >= 0x10) {   // the files it holds (offset, size) end where the NSP should: shorter, a copy or download stopped early
            java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(h, 0, got).order(java.nio.ByteOrder.LITTLE_ENDIAN); int cnt = b.getInt(4), str = b.getInt(8);
            if (cnt > 0 && 0x10 + 0x18L * cnt <= got && str >= 0) {
                long end = 0; for (int i = 0; i < cnt; i++) end = Math.max(end, b.getLong(0x10 + 0x18 * i) + b.getLong(0x10 + 0x18 * i + 8));
                long want = 0x10 + 0x18L * cnt + str + end, have = sizeOf(u);
                if (have >= 0 && have < want) return T.t(this, "%s is incomplete: it holds %s of its %s MB. It was cut short while being copied or downloaded: copy the whole file again.", name, have >> 20, want >> 20);
            }
        }
        return null;
    }
    private long sizeOf(Uri u) { String p = pathOf(u); return p != null ? new File(p).length() : documentSize(u); }
    // the title ID in a dump's file name ([0100ABF008968000]), or -1
    private static long titleId(String path) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\[(0100[0-9A-Fa-f]{12})\\]").matcher(new File(path).getName());
        return m.find() ? Long.parseUnsignedLong(m.group(1), 16) : -1;
    }
    // the game a title ID belongs to: an update is the game's ID + 0x800, a DLC the game's ID + 0x1000 + its number
    private static long appOf(long tid) { return ((tid >> 12) & 1) == 1 ? (tid & ~0xFFFL) - 0x1000 : tid & ~0xFFFL; }
    // files of two different games chosen together (Sword's base game and Shield's update, say): said before the setup, which takes one game at a time
    private String mixedGames() {
        Set<Long> apps = new HashSet<>(); for (String g : games) { long t = titleId(g); if (t >= 0) apps.add(appOf(t)); }
        if (apps.size() < 2) return null;
        List<String> names = new ArrayList<>();
        for (long a : apps) { String nm = String.format("%016X", a); for (String[] g : GameSettings.games(this)) if (g[0].equalsIgnoreCase(nm)) nm = g[1]; names.add(nm); }
        return T.t(this, "These files are from different games (%s): set up one game at a time. Tap Clear and choose the files of one game.", String.join(", ", names));
    }
    // Clear: the files chosen so far, and the copies of them in the app's storage
    private void clearGames() {
        if (running) return;
        for (File f : importedGames) f.delete();
        importedGames.clear(); games.clear(); status.setText(""); refresh();
    }
    // copies made for an earlier setup that did not finish (the app closed, the setup failed): chosen again rather than left taking space
    private File importDir() { return new File(GameSettings.root(this), "imported"); }
    private void restoreImported() {
        if (running) return;
        File[] fs = importDir().listFiles(); if (fs == null) return;
        for (File f : fs) { if (!f.isFile() || f.length() == 0) { f.delete(); continue; } if (!games.contains(f.getPath())) { games.add(f.getPath()); importedGames.add(f); } }
    }
    private String nameOf(Uri u) { try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) { if (c != null && c.moveToFirst()) return c.getString(0); } catch (Exception e) { } return u.getLastPathSegment(); }
    // Resolve document-provider paths for in-place reads; copying multi-gigabyte game files into the app is not practical.
    private String pathOf(Uri u) {
        if ("file".equals(u.getScheme())) return readableFile(u.getPath());
        if (DocumentsContract.isDocumentUri(this, u)) {
            try {
                String id = DocumentsContract.getDocumentId(u);
                if (id.startsWith("raw:")) return readableFile(id.substring(4));
                // the picker's Downloads entry: msf:<MediaStore id> (Android 10+), whose _data the media index knows; a copy of a several-GB dump is the fallback
                if (id.startsWith("msf:")) {
                    try (Cursor c = getContentResolver().query(android.provider.MediaStore.Files.getContentUri("external", Long.parseLong(id.substring(4))), new String[] { "_data" }, null, null, null)) {
                        if (c != null && c.moveToFirst() && !c.isNull(0)) { String p = readableFile(c.getString(0)); if (p != null) return p; }
                    } catch (NumberFormatException | SecurityException e) { }
                }
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
        gameText.setText(T.t(this, "Copying %d selected file(s) into app storage...", imports.size()));
        status.setText(T.t(this, "Copying %d selected game file(s) into app storage; large files need extra free space...", imports.size()));
        new Thread(() -> {
            List<File> copied = new ArrayList<>();
            String error = null;
            try {
                File dir = importDir();
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
                    if (size >= 0 && destination.length() != size) { long got = destination.length(); destination.delete();
                        throw new IOException(T.t(this, "the copy of %s stopped early (%s of %s MB): choose it again, ideally from the phone's own storage", picked.name, got >> 20, size >> 20)); }
                    copied.add(destination);
                }
            } catch (IOException | SecurityException | IllegalArgumentException e) { error = e.getMessage(); }
            final String failure = error;
            runOnUiThread(() -> {
                running = false;
                for (File file : copied) { games.add(file.getPath()); importedGames.add(file); }
                String mixed = mixedGames();
                if (failure == null) status.setText(mixed != null ? mixed : T.t(this, "Game file(s) copied into app storage and selected."));
                else status.setText(T.t(this, "Could not import game file: %s", failure) + (copied.isEmpty() ? "" : " " + T.t(this, "Previously copied selections are ready.")));
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
        i.setType("application/zip"); i.putExtra(Intent.EXTRA_TITLE, game()[2] + "-save.zip");
        startActivityForResult(i, PICK_SAVE_EXPORT);
    }
    private void chooseSaveImport() {   // any file: a ZIP a browser or a chat app saved is often typed octet-stream / x-zip-compressed and the picker greyed it out
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_SAVE_IMPORT);
    }
    private void importSave(Uri source) {
        if (source == null) { status.setText(T.t(this, "Choose the save ZIP you edited or exported.")); return; }
        running = true; refresh(); status.setText(T.t(this, "Importing save archive..."));
        new Thread(() -> {
            String result;
            try (InputStream input = getContentResolver().openInputStream(source)) {
                if (input == null) throw new IOException("the file provider could not open the selected archive");
                int files = SaveImport.install(input, new File(gameDir(), "save_import"), nameOf(source));
                result = T.t(this, "Imported %d save file(s) into %s (%s). Tap Play to load it; your current save is kept as a backup.", files, game()[1], SaveImport.lastFiles);
            } catch (IOException | SecurityException e) {
                result = T.t(this, "Could not import save data: %s", e.getMessage());
            }
            final String message = result;
            runOnUiThread(() -> { running = false; status.setText(message); refresh(); });
        }).start();
    }
    private void exportSave(Uri destination) {
        if (destination == null) return;
        File save = new File(gameDir(), "save");
        status.setText(T.t(this, "Exporting save data..."));
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
        for (String g : games) { String n = new File(g).getName().toUpperCase(); long t = titleId(g);
            if (t >= 0 && ((t >> 12) & 1) == 1) dlc.add(g);
            else if (t >= 0 && (t & 0xFFF) == 0x800) upd = g;
            else if (t >= 0 && (t & 0xFFF) == 0 && base == null) base = g;
            else if (n.contains("[UPD]") || n.contains("800]")) upd = g; else if (n.contains("[DLC]") || n.contains("[AOC]")) dlc.add(g);
            else if (base == null || new File(g).length() > new File(base).length()) { if (base != null) dlc.add(base); base = g; } else dlc.add(g); }
        if (base == null && upd != null) { base = upd; upd = null; }   // (only an update chosen, or an update holding the base game: the unpacker says which it is)
        if (base == null && !dlc.isEmpty()) base = dlc.remove(0);
        List<String> a = new ArrayList<>(); a.add(base); a.add(upd); a.addAll(dlc); return a.toArray(new String[0]);
    }
    private void startSetup(boolean allowDamaged) {
        if (running || games.isEmpty()) return;
        offerAnyway = false; anyway.setVisibility(android.view.View.GONE);
        String mixed = mixedGames(); if (mixed != null) { status.setText(mixed); return; }
        for (String g : games) if (!new File(g).canRead()) { status.setText(T.t(this, "Could not read %s.", new File(g).getName()) + " " + T.t(this, "Tap Clear and choose your files again.")); return; }
        // Sword and Shield run on update 1.3.2 only: a base game named [v0] chosen without any update is said now, not after the unpacking (only for .nsp files named
        //   with their title IDs: an .xci, or a dump named otherwise, can hold the update, and the setup checks the version itself)
        { boolean nsp = true, named = true, hasUpd = false, v0 = false;
          for (String f : games) { String n = new File(f).getName().toLowerCase(java.util.Locale.ROOT); long t = titleId(f);
              nsp &= n.endsWith(".nsp"); named &= t >= 0; if (t >= 0 && (t & 0xFFF) == 0x800) hasUpd = true; if (t >= 0 && (t & 0xFFF) == 0 && n.contains("[v0]")) v0 = true; }
          if (nsp && named && v0 && !hasUpd) { status.setText(T.t(this, "Your files are the base game only: this app needs the %s update too. Choose the update (.nsp, its title ID ends in 800) as well.", GameSettings.version(this))); return; } }
        bar.setMax(600); bar.setProgress(0); status.setText(T.t(this, "Preparing..."));
        String[] g = sortGames(); List<String> cmd = new ArrayList<>();
        cmd.add("--keys"); cmd.add(keysDir().getPath()); cmd.add("--base"); cmd.add(g[0]);
        if (g[1] != null) { cmd.add("--update"); cmd.add(g[1]); }
        for (int k = 2; k < g.length; k++) { cmd.add("--dlc"); cmd.add(g[k]); }
        if (allowDamaged) cmd.add("--allow-damaged");
        resumes = 0; workerCap = 0; runFullSetup(cmd);
    }
    // Android's memory killer can stop the whole setup on phones that guard memory closely (#51 / #52: "setup stopped (137)" during the shader translation): it is
    //   started again with --resume, up to 3 times, with half the workers each time; the setup goes on from the translation (setup_game.py resume.json)
    private int resumes, workerCap;
    private void runFullSetup(List<String> cmd) {
        runSetup(cmd, err -> {
            if (err != null && err.contains("(137)") && resumes < 3) {
                resumes++; workerCap = Math.max(1, (workerCap > 0 ? workerCap : setupWorkers()) / 2);
                status.setText(T.t(this, "Android stopped the setup to free memory: going on from where it was (%s of 3)...", resumes));
                List<String> again = new ArrayList<>(cmd); if (!again.contains("--resume")) again.add("--resume"); runFullSetup(again); return;
            }
            if (err != null && offerAnyway) anyway.setVisibility(android.view.View.VISIBLE);
            bar.setProgress(err == null ? 600 : bar.getProgress());
            status.setText(err == null ? T.t(this, "Ready: tap Play.") : T.t(this, "Setup failed: %s", err) + "\n" + T.t(this, "The setup log is in Android/data/%s/files/setup_work.", getPackageName()));
            if (err == null) {
                for (File file : importedGames) file.delete();
                importedGames.clear();
                importDir().delete(); games.clear();
                if (gameIcon() != null) pinShortcut();
                startRest();
            }
        });
    }
    // Fast setup (setup_game.py --rest): the shaders the setup left for later are translated from here in the background at full speed; Play works meanwhile (the run
    //   then makes way for the game by itself: game_running) and GameActivity carries on with whatever is left beside the game
    private void startRest() {
        if (!new File(gameDir(), "rest.json").exists()) return;
        new Thread(() -> {
            try {
                Process p = startTool(this, Arrays.asList("--rest"), workers()); if (p == null) return;
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream())); String l;
                while ((l = r.readLine()) != null) {
                    if (!l.startsWith("PROGRESS ")) continue;
                    String[] f = l.substring(9).trim().split("/"); long done = Long.parseLong(f[0]), all = Math.max(1, Long.parseLong(f[1])); int pct = (int) Math.min(100, done * 100 / all);
                    runOnUiThread(() -> { if (!running) status.setText(T.t(this, "Ready: tap Play.") + "\n" + T.t(this, "The rest of the shaders are being translated in the background (%s%%): until they are done, a few things may not show the first time they appear.", pct)); });
                }
                p.waitFor();
                runOnUiThread(() -> { if (!running && !new File(gameDir(), "rest.json").exists()) status.setText(T.t(this, "Ready: tap Play.")); });
            } catch (Exception e) { }
        }, "rest of the shaders").start();
    }
    private int workers() { return workerCap > 0 ? workerCap : setupWorkers(); }
    // shaders the game saved on an earlier run (nvn_capture/<key>.code) and no setup has translated yet (captures_done lists those already done)
    private List<String> pendingCaptures() { return pendingCaptures(gameDir()); }
    static List<String> pendingCaptures(File gameDir) {   // GameActivity asks too
        String[] all = new File(gameDir, "nvn_capture").list((d, n) -> n.endsWith(".code")); List<String> out = new ArrayList<>(); if (all == null) return out;
        Set<String> done = new HashSet<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(gameDir, "captures_done")))) { String l; while ((l = r.readLine()) != null) done.add(l.trim()); } catch (IOException e) { }
        for (String n : all) if (!done.contains(n)) out.add(n);
        return out;
    }
    // a setup step's name (setup_game.py STEP lines) in the player's language: a number in it ("translate 2 shaders") is the %s of one translation
    private String step(String s) { java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\d+").matcher(s); return m.find() ? T.t(this, s.replaceFirst("\\d+", "%s"), m.group()) : T.t(this, s); }
    // work before the game starts: shaders captured last time, or mods switched on or changed since their shaders were last read (GameActivity asks too)
    static boolean pendingWork(File gameDir) { return !pendingCaptures(gameDir).isEmpty() || modShaderState(gameDir) != null; }
    // the RomFS folders of the mods that are on (as android_main.c mods_romfs picks them) and a fingerprint of their files; null when nothing changed since the
    // last read (mod_shaders_done holds the fingerprint then) or no mod has RomFS files
    static String[] modShaderState(File gameDir) {
        File mods = new File(gameDir, "mods"); List<String> roots = new ArrayList<>(); File top = new File(mods, "romfs"); if (top.isDirectory()) roots.add(top.getPath());
        String[] names = mods.list(); if (names != null) { Arrays.sort(names);
            for (String n : names) { File r = new File(new File(mods, n), "romfs"); if (!n.startsWith(".") && !n.startsWith("-") && !n.equals("romfs") && r.isDirectory()) roots.add(r.getPath()); } }
        long[] fp = { 1469598103934665603L };
        for (String r : roots) fingerprint(new File(r), r.length(), fp);
        String sig = roots.isEmpty() ? "" : Long.toHexString(fp[0]) + " " + String.join(":", roots), done = "";
        try (BufferedReader rd = new BufferedReader(new FileReader(new File(gameDir, "mod_shaders_done")))) { String l = rd.readLine(); if (l != null) done = l.trim(); } catch (IOException e) { }
        // (no mods left but a Trinity index from earlier: a run with no roots deletes it, setup_game.py trinity_index)
        if (roots.isEmpty()) return new File(mods, ".trinity_index").exists() ? new String[] { "", "" } : null;
        return sig.equals(done) ? null : new String[] { String.join(":", roots), sig };
    }
    private static void fingerprint(File d, int cut, long[] fp) {   // every file's path, size and time
        File[] fs = d.listFiles(); if (fs == null) return; Arrays.sort(fs);
        for (File f : fs) { if (f.isDirectory()) { fingerprint(f, cut, fp); continue; }
            String s = f.getPath().substring(cut) + "|" + f.length() + "|" + f.lastModified(); for (int i = 0; i < s.length(); i++) fp[0] = (fp[0] ^ s.charAt(i)) * 1099511628211L; }
    }
    private void startModShaders() {
        String[] st = modShaderState(gameDir()); if (st == null) { startGame(); return; }
        bar.setMax(300); bar.setProgress(0); status.setText(T.t(this, "Reading the shaders of your mods..."));
        runSetup(new ArrayList<>(Arrays.asList("--mod-shaders", st[0])), err -> {
            try (FileWriter w = new FileWriter(new File(gameDir(), "mod_shaders_done"))) { w.write(st[1] + "\n"); } catch (IOException e) { }   // (also after a failure: not tried again each start)
            if (err != null) Toast.makeText(this, T.t(this, "Could not add your mods' shaders (%s); the game still plays, some mod effects may not show.", err), Toast.LENGTH_LONG).show();
            startGame();
        });
    }
    private void startCaptures() {
        List<String> todo = pendingCaptures(); if (todo.isEmpty()) { startModShaders(); return; }
        bar.setMax(300); bar.setProgress(0); status.setText(T.t(this, "Adding %d shader(s) the game made last time...", todo.size()));
        runSetup(new ArrayList<>(Arrays.asList("--captures", new File(gameDir(), "nvn_capture").getPath())), err -> {
            if (err != null) { status.setText(T.t(this, "Could not add the new shaders (%s); the game still plays without them.", err)); return; }
            try (FileWriter w = new FileWriter(new File(gameDir(), "captures_done"), true)) { for (String n : todo) w.write(n + "\n"); } catch (IOException e) { }
            startModShaders();
        });
    }
    // setup_game.py with the given arguments (plus the folders every run needs): progress from its "STEP n/total what" lines (100 bar units a step) and, inside a step,
    // its "PROGRESS done/total" lines (the shader translation, hours on a phone); done(null) or done(the reason it failed) on the UI thread.
    // The screen stays on meanwhile: once it goes dark Android slows or stops the work
    // What the setup is doing, with the time it has been running under it (a ticker, every second): an hour-long step whose number stays at 99% (the last shaders,
    //   then the writing of the results, #56) still shows that it is alive
    private String runText = ""; private long runStart; private final android.os.Handler ticker = new android.os.Handler(android.os.Looper.getMainLooper());
    private void show(String s) { runText = s; tick(); }
    private void tick() {
        if (!running) { status.setText(runText); return; }
        long t = (android.os.SystemClock.elapsedRealtime() - runStart) / 1000;
        String el = t >= 3600 ? String.format(java.util.Locale.ROOT, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60) : String.format(java.util.Locale.ROOT, "%d:%02d", t / 60, t % 60);
        status.setText(runText + "\n" + T.t(this, "Running for %s", el));
    }
    private final Runnable tickRun = new Runnable() { public void run() { if (!running) return; tick(); ticker.postDelayed(this, 1000); } };
    private void runSetup(List<String> args, Consumer<String> done) {
        running = true; refresh();
        runStart = android.os.SystemClock.elapsedRealtime(); runText = String.valueOf(status.getText()); ticker.removeCallbacks(tickRun); ticker.post(tickRun);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            String err = null; int[] stepNo = {1}, stepOf = {6}; String[] stepName = {""}; Process p = null;
            try {
                File py = unpackPython(), dm = unpackScripts(), bin = binDir();
                File work = new File(gameDir(), "setup_work"); work.mkdirs();
                List<String> cmd = new ArrayList<>();
                String lib = getApplicationInfo().nativeLibraryDir;
                cmd.add(lib + "/libpython_exe.so"); cmd.add(new File(dm, "android/tools/setup_game.py").getPath());
                cmd.addAll(args);
                cmd.add("--out"); cmd.add(gameDir().getPath()); cmd.add("--work"); cmd.add(work.getPath()); cmd.add("--bin"); cmd.add(bin.getPath());
                File ext = new File(dm, "android/app/assets/nintendo_ext.ttf"); if (ext.exists() && !args.contains("--captures")) { cmd.add("--ext-font"); cmd.add(ext.getPath()); }
                if (!args.contains("--captures")) {   // the dump says which game it is: setup puts it in that game's place (GAME line below), one of each game
                    StringBuilder ids = new StringBuilder(); for (String[] g : GameSettings.games(this)) ids.append(ids.length() > 0 ? "," : "").append(g[0]);
                    cmd.add("--games"); cmd.add(ids.toString()); cmd.add("--root"); cmd.add(GameSettings.root(this).getPath()); }
                ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true); toolEnv(this, pb, py, dm, bin, workers());
                p = pb.start();
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream())); String ln;
                while ((ln = r.readLine()) != null) { final String l = ln; try {
                    if (l.startsWith("STEP ")) { String[] f = l.split(" ", 3); String[] nn = f[1].split("/"); int n = Integer.parseInt(nn[0]), of = Integer.parseInt(nn[1]); stepNo[0] = n; stepName[0] = f[2]; stepOf[0] = of;
                        runOnUiThread(() -> { bar.setProgress((n - 1) * 100); show(T.t(this, "Step %s of %s: %s", n, of, step(f[2])) + "..."); }); }
                    else if (l.startsWith("PROGRESS ")) {
                        String[] f = l.substring(9).trim().split("/"); long a = Long.parseLong(f[0]), b = Math.max(1, Long.parseLong(f[1])); int pct = (int) Math.min(100, a * 100 / b), n = stepNo[0]; String s = stepName[0];
                        int of = stepOf[0];
                        runOnUiThread(() -> { bar.setProgress((n - 1) * 100 + pct); show(T.t(this, "Step %s of %s: %s", n, of, step(s)) + "... " + pct + "%\n"
                            + (pct >= 99 ? T.t(this, "Almost done with this step: its last part shows no progress and can take several minutes.") : T.t(this, "This takes a long time on a phone: keep this screen open, ideally plugged in."))); }); }
                    else if (l.startsWith("GAME ")) { String id = l.substring(5).trim(); GameSettings.choose(this, id);
                        runOnUiThread(() -> show(T.t(this, "Your files are %s: setting it up...", GameSettings.game(this)[1]))); }
                    else if (l.startsWith("OFFER allow-damaged")) offerAnyway = true;
                    else if (l.startsWith("FAILED")) err = l.substring(6).trim(); } catch (RuntimeException e) { } }
                if (p.waitFor() != 0 && err == null) err = "setup stopped (" + p.exitValue() + ")";
            } catch (Exception e) { err = e.toString(); if (p != null) p.destroy(); }
            final String e2 = err;
            runOnUiThread(() -> { getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); running = false; ticker.removeCallbacks(tickRun); done.accept(e2); refresh(); });
        }).start();
    }
    // Setup's parallel workers (the shader scan and translation): one per core, but at most one per 1.5 GB of the phone's memory and 4 on phones whose profile is a
    // lighter one (handheld mode), whose memory the low-memory killer guards closely (a scan worker was killed at 8, #43)
    // Setup workers: one a core (at most 8); how many run at once is decided while they run, by the memory the phone has free then (dmsh_gate.c, shader_split.py
    // mem_wait): the first always runs, each other one only with DM_GATE_FLOOR_MB + DM_GATE_MB free. A phone with 4 GB gets through with one or two, a 16 GB one runs all.
    // Without the gate (an older build of the tools): the old rule, by total memory
    private int setupWorkers() {
        int j = Math.min(8, Runtime.getRuntime().availableProcessors());
        if (!new File(getApplicationInfo().nativeLibraryDir, "libdmsh_gate.so").exists()) {
            android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo(); ((ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mi);
            j = Math.min(j, (int) (mi.totalMem / (1536L << 20))); String[] p = GameSettings.profile(this); if (p != null && p[2].equals("low")) j = Math.min(j, 4);
        }
        return Math.max(1, j);
    }
    // memory kept free for the phone itself: Android's own low-memory line (MemoryInfo.threshold) with room above it, at least 300 MB
    static long gateFloorMb(Context c) {
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo(); ((ActivityManager) c.getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mi);
        return Math.max(300, (mi.threshold >> 20) + 200);
    }
    // the environment setup_game.py runs in: Python, the scripts, the tools (bin first: the .NET unpacker dlopens libssl.so.3 / libcrypto.so.3, which binDir() links to
    // the OpenSSL 3 that ships with CPython for Android), the workers and the memory gate (dmsh_gate.c)
    static void toolEnv(Context c, ProcessBuilder pb, File py, File dm, File bin, int workers) {
        String lib = c.getApplicationInfo().nativeLibraryDir; java.util.Map<String, String> e = pb.environment();
        e.put("PYTHONHOME", py.getPath()); e.put("DM_GAME", GameSettings.game(c)[0]); e.put("PYTHONPATH", new File(dm, "android/pyshim").getPath());
        e.put("LD_LIBRARY_PATH", bin.getPath() + ":" + lib); e.put("LIBCAPSTONE_PATH", lib); e.put("NX_NATIVE_LIB_DIR", lib);
        e.put("DOTNET_OPENSSL_VERSION_OVERRIDE", "3"); e.put("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1");
        e.put("TMPDIR", c.getCacheDir().getPath()); e.put("J", String.valueOf(workers));
        e.put("DM_GATE_MB", "600"); e.put("DM_GATE_FLOOR_MB", String.valueOf(gateFloorMb(c)));
    }
    // setup_game.py off this screen (GameActivity's live shaders), with the tools as the last setup here left them; null when there are none yet
    static Process startTool(Context c, List<String> args, int workers) throws IOException {
        File files = c.getFilesDir(), py = new File(files, "python"), dm = new File(files, "dm"), bin = new File(files, "bin"), game = GameSettings.dir(c), work = new File(game, "setup_work");
        File script = new File(dm, "android/tools/setup_game.py"); if (!script.isFile() || !new File(bin, "dmsh_xlate").exists()) return null;
        work.mkdirs();
        List<String> cmd = new ArrayList<>(Arrays.asList(c.getApplicationInfo().nativeLibraryDir + "/libpython_exe.so", script.getPath())); cmd.addAll(args);
        cmd.addAll(Arrays.asList("--out", game.getPath(), "--work", work.getPath(), "--bin", bin.getPath()));
        ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true); toolEnv(c, pb, py, dm, bin, workers);
        return pb.start();
    }
    private File iconFile() { return new File(gameDir(), "icon.jpg"); }
    // the game's icon from the player's dump: this screen and the recent-apps card
    @SuppressWarnings("deprecation")
    private Bitmap gameIcon() {
        Bitmap b = iconFile().exists() ? BitmapFactory.decodeFile(iconFile().getPath()) : null; if (b == null) return null;
        setTaskDescription(new ActivityManager.TaskDescription(gameName(), b));
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
        // one shortcut per game, naming it (the first game keeps the id "game" its players' shortcut already has)
        String sid = game()[0].equalsIgnoreCase(GameSettings.games(this).get(0)[0]) ? "game" : "game_" + game()[0];
        Intent go = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_MAIN).putExtra("game", game()[0]);
        ShortcutInfo si = new ShortcutInfo.Builder(this, sid).setShortLabel(gameName()).setLongLabel("Play " + gameName()).setIcon(Icon.createWithBitmap(b)).setIntent(go).build();
        try { sm.addDynamicShortcuts(Arrays.asList(si)); } catch (Exception e) { }
        for (ShortcutInfo p : sm.getPinnedShortcuts()) if (p.getId().equals(sid)) { sm.updateShortcuts(Arrays.asList(si)); status.setText(T.t(this, "The %s shortcut is on the home screen.", gameName())); return; }
        if (sm.isRequestPinShortcutSupported()) sm.requestPinShortcut(si, null); else status.setText(T.t(this, "This launcher cannot add shortcuts: long-press the app's icon for %s.", gameName()));
    }
    // the executables the setup runs, under the names the scripts call them by (links into the app's native library folder, where Android allows execution);
    // also OpenSSL 3 under the names the .NET unpacker dlopens (libssl.so.3 / libcrypto.so.3), so it does not pick up the system's BoringSSL
    private File binDir() throws IOException {
        File d = new File(getFilesDir(), "bin"); d.mkdirs(); String lib = getApplicationInfo().nativeLibraryDir;
        String[][] links = { { "nx_extract", "libnx_extract.so" }, { "dmsh_xlate", "libdmsh_xlate.so" }, { "spirv-cross", "libspirv_cross.so" }, { "dmsh_gate", "libdmsh_gate.so" }, { "dmoodle", "libdmoodle.so" },
                             { "libssl.so.3", "libssl_python.so" }, { "libcrypto.so.3", "libcrypto_python.so" } };
        for (String[] l : links) { File f = new File(d, l[0]); f.delete(); if (!new File(lib, l[1]).exists() && (l[0].equals("dmsh_gate") || l[0].equals("dmoodle"))) continue; try { android.system.Os.symlink(lib + "/" + l[1], f.getPath()); } catch (Exception e) { throw new IOException("link " + l[0] + ": " + e); } }
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
