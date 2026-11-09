// The in-game settings: one panel that slides in from the right over the dimmed game (opened by the touch controls' ☰ button, touch_ui.c; by holding − and + together on
// a controller for 1 s; or by holding − for 3 s, which opens it on the controller buttons: android_main.c front_open_menu). Tabs down its left side, each setting a card with
// its choices as chips and what the chosen one does under them; mods, the graphics driver, the controller buttons and the online room are pages inside the panel. Nothing
// pops up over the game: a setting that applies from the next start says so on its card, other news goes to the panel's status line. Every change is written to env.txt
// (GameSettings); the picture, the touch controls, the controller buttons, rumble and the performance overlay change at once (nativeSetting). Every text goes through T.t
package com.nxnative;

import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class GameMenu {
    private GameMenu() { }
    private static native boolean nativeSetting(String key, String value);
    private static native void nativeEditTouch();

    // ---------- look ----------
    private static final int BG = 0xF0121419, CARD = 0xFF1C1F26, CARD_LINE = 0xFF2A2E37, TEXT = 0xFFF2F3F5, SUB = 0xFF9AA0AB, CHIP = 0xFF262A33, WARN = 0xFFFFB74D;
    private static int accent(Activity a) {   // the game's colour: Sword blue, Shield magenta, Violet purple, Scarlet red
        String id = GameSettings.game(a)[0].toUpperCase(java.util.Locale.ROOT);
        return id.startsWith("0100ABF") ? 0xFF2E9BFF : id.startsWith("01008DB") ? 0xFFE0306E : id.startsWith("01008F6") ? 0xFF9B5CFF : id.startsWith("0100A3D") ? 0xFFFF4D3D : 0xFF3DDC97;
    }
    private static int dp(Activity a, float v) { return Math.round(v * a.getResources().getDisplayMetrics().density); }
    private static GradientDrawable round(int color, float r, int stroke, int strokeColor) {
        GradientDrawable g = new GradientDrawable(); g.setColor(color); g.setCornerRadius(r); if (stroke > 0) g.setStroke(stroke, strokeColor); return g;
    }
    private static TextView text(Activity a, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(a); t.setText(s); t.setTextSize(sp); t.setTextColor(color); if (bold) t.setTypeface(Typeface.DEFAULT_BOLD); return t;
    }
    private static String t(Activity a, String en, Object... args) { return T.t(a, en, args); }

    // ---------- the panel ----------
    private static Dialog dlg; private static Activity act; private static LinearLayout rail, body; private static ScrollView scroll; private static TextView status;
    private static String page = "display"; private static final Set<String> RESTART = new HashSet<>();   // settings changed that apply from the next start
    private static final String[][] TABS = { { "display", "Display" }, { "perf", "Performance" }, { "controls", "Controls" }, { "mods", "Mods" }, { "saves", "Saves" },
                                             { "online", "Online" }, { "system", "System" } };

    // android_main.c: what 0 = the menu, 1 = the controller buttons
    public static void show(Activity a, int what) { a.runOnUiThread(() -> open(a, what == 1 ? "remap" : null)); }

    private static void open(Activity a, String start) {
        if (dlg != null && dlg.isShowing()) { if (start != null) go(start); return; }
        act = a; if (start != null) page = start; else if (page.equals("remap") || page.equals("driver") || page.equals("order")) page = "display";
        Dialog d = new Dialog(a); d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        int sw = a.getResources().getDisplayMetrics().widthPixels, sh = a.getResources().getDisplayMetrics().heightPixels;
        LinearLayout panel = new LinearLayout(a); panel.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable pbg = new GradientDrawable(); pbg.setColor(BG); pbg.setCornerRadii(new float[] { dp(a, 22), dp(a, 22), 0, 0, 0, 0, dp(a, 22), dp(a, 22) }); panel.setBackground(pbg);
        // header: the game, a close button
        LinearLayout head = new LinearLayout(a); head.setGravity(Gravity.CENTER_VERTICAL); head.setPadding(dp(a, 20), dp(a, 14), dp(a, 12), dp(a, 8));
        View dot = new View(a); dot.setBackground(round(accent(a), dp(a, 6), 0, 0)); head.addView(dot, new LinearLayout.LayoutParams(dp(a, 10), dp(a, 10)));
        TextView title = text(a, GameSettings.game(a)[1], 19, TEXT, true); title.setPadding(dp(a, 10), 0, 0, 0); head.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        TextView close = text(a, "✕", 20, SUB, false); close.setPadding(dp(a, 14), dp(a, 6), dp(a, 14), dp(a, 6)); close.setOnClickListener(v -> d.dismiss()); head.addView(close);
        panel.addView(head);
        LinearLayout mid = new LinearLayout(a); mid.setOrientation(LinearLayout.HORIZONTAL);
        rail = new LinearLayout(a); rail.setOrientation(LinearLayout.VERTICAL); rail.setPadding(dp(a, 10), dp(a, 4), dp(a, 6), dp(a, 10));
        ScrollView rs = new ScrollView(a); rs.addView(rail); mid.addView(rs, new LinearLayout.LayoutParams(dp(a, 150), -1));
        scroll = new ScrollView(a); body = new LinearLayout(a); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(dp(a, 8), dp(a, 4), dp(a, 18), dp(a, 18));
        scroll.addView(body); mid.addView(scroll, new LinearLayout.LayoutParams(0, -1, 1));
        panel.addView(mid, new LinearLayout.LayoutParams(-1, 0, 1));
        status = text(a, "", 13, SUB, false); status.setPadding(dp(a, 20), dp(a, 6), dp(a, 20), dp(a, 12)); status.setVisibility(View.GONE); panel.addView(status);
        d.setContentView(panel);
        Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        w.setLayout(Math.min(sw, Math.max(dp(a, 560), (int) (sw * 0.6f))), sh); w.setGravity(Gravity.END | Gravity.FILL_VERTICAL);
        w.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND); w.setDimAmount(0.45f);
        w.setWindowAnimations(android.R.style.Animation_InputMethod);   // (slides in from the edge)
        hideBars(w);
        d.setOnKeyListener((x, kc, e) -> remapKey(kc, e));
        d.setOnDismissListener(x -> { dlg = null; RESTART.clear(); capture = null; });
        dlg = d; d.show(); hideBars(w);
        go(page);
    }
    @SuppressWarnings("deprecation")
    private static void hideBars(Window w) {   // the game stays full screen behind the panel (Immersive.java's flags on the panel's own window)
        w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
    private static void say(String s) {   // the status line (in place of toasts over the game)
        if (status == null) return;
        status.setText(s); status.setVisibility(s.isEmpty() ? View.GONE : View.VISIBLE);
    }
    private static void go(String p) {
        page = p; Activity a = act; if (a == null || body == null) return;
        rail.removeAllViews();
        String top = p.equals("remap") ? "controls" : p.equals("driver") ? "system" : p.equals("order") ? "mods" : p;
        for (String[] tb : TABS) {
            boolean on = tb[0].equals(top);
            TextView r = text(a, t(a, tb[1]), 15, on ? TEXT : SUB, on); r.setPadding(dp(a, 14), dp(a, 11), dp(a, 10), dp(a, 11));
            r.setBackground(on ? round((accent(a) & 0x00FFFFFF) | 0x38000000, dp(a, 12), 0, 0) : null);
            r.setOnClickListener(v -> { say(""); capture = null; go(tb[0]); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 4); rail.addView(r, lp);
        }
        body.removeAllViews();
        switch (p) {
            case "display": display(a); break;
            case "perf": perf(a); break;
            case "controls": controls(a); break;
            case "remap": remap(a); break;
            case "mods": mods(a); break;
            case "order": order(a); break;
            case "saves": saves(a); break;
            case "online": online(a); break;
            case "driver": driver(a); break;
            default: system(a);
        }
    }
    private static void redraw() { int y = scroll.getScrollY(); go(page); scroll.post(() -> scroll.scrollTo(0, y)); }   // (the same page, kept where it was scrolled)

    // ---------- building blocks ----------
    private static void heading(Activity a, String s) {
        TextView h = text(a, s, 13, SUB, true); h.setAllCaps(true); h.setLetterSpacing(0.08f); h.setPadding(dp(a, 6), dp(a, 14), 0, dp(a, 6)); body.addView(h);
    }
    private static LinearLayout card(Activity a) {
        LinearLayout c = new LinearLayout(a); c.setOrientation(LinearLayout.VERTICAL); c.setBackground(round(CARD, dp(a, 16), 1, CARD_LINE)); c.setPadding(dp(a, 16), dp(a, 12), dp(a, 16), dp(a, 14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 10); body.addView(c, lp); return c;
    }
    private static LinearLayout titleRow(Activity a, LinearLayout c, String title, String tag) {
        LinearLayout r = new LinearLayout(a); r.setGravity(Gravity.CENTER_VERTICAL);
        r.addView(text(a, title, 16, TEXT, true), new LinearLayout.LayoutParams(0, -2, 1));
        if (tag != null) { TextView tg = text(a, tag, 11, accent(a), true); tg.setBackground(round(0, dp(a, 10), dp(a, 1), accent(a))); tg.setPadding(dp(a, 8), dp(a, 2), dp(a, 8), dp(a, 2)); r.addView(tg); }
        c.addView(r); return r;
    }
    private static TextView pill(Activity a, String label, boolean on) {
        TextView p = text(a, label, 13, on ? Color.WHITE : SUB, true); p.setPadding(dp(a, 14), dp(a, 5), dp(a, 14), dp(a, 5));
        p.setBackground(on ? round(accent(a), dp(a, 14), 0, 0) : round(CHIP, dp(a, 14), 1, CARD_LINE)); return p;
    }
    // a choice: { value, chip label, what it does }; the chosen one's line under the chips. Applied at once when the engine can (nativeSetting), else tagged "Applies next start"
    private static void choice(Activity a, String key, String title, String[][] opts) {
        LinearLayout c = card(a); String cur = GameSettings.get(a, key);
        titleRow(a, c, title, RESTART.contains(key) ? t(a, "Applies next start") : null);
        HorizontalScrollView hs = new HorizontalScrollView(a); hs.setHorizontalScrollBarEnabled(false);
        LinearLayout chips = new LinearLayout(a); chips.setPadding(0, dp(a, 10), 0, dp(a, 6)); hs.addView(chips); c.addView(hs);
        TextView desc = text(a, "", 13, SUB, false); c.addView(desc);
        boolean found = false; for (String[] o : opts) if (o[0].equals(cur)) found = true;
        for (String[] o : opts) {
            boolean on = o[0].equals(cur) || (!found && o == opts[0]);
            TextView ch = text(a, o[1], 14, on ? Color.WHITE : TEXT, on); ch.setPadding(dp(a, 16), dp(a, 8), dp(a, 16), dp(a, 8));
            ch.setBackground(on ? round(accent(a), dp(a, 18), 0, 0) : round(CHIP, dp(a, 18), 1, CARD_LINE));
            ch.setOnClickListener(v -> { if (!o[0].equals(GameSettings.get(a, key))) set(a, key, o[0]); redraw(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.rightMargin = dp(a, 8); chips.addView(ch, lp);
            if (on) desc.setText(o.length > 2 ? o[2] : "");
        }
        if (desc.getText().length() == 0) desc.setVisibility(View.GONE);
    }
    private static void action(Activity a, String title, String sub, Runnable r) {
        LinearLayout c = card(a); LinearLayout row = titleRow(a, c, title, null);
        row.addView(text(a, "›", 22, SUB, false));
        if (sub != null && !sub.isEmpty()) { TextView s = text(a, sub, 13, SUB, false); s.setPadding(0, dp(a, 4), 0, 0); c.addView(s); }
        c.setOnClickListener(v -> r.run());
    }
    private static void slider(Activity a, String title, String key, int min, int max, int def, java.util.function.IntFunction<String> fmt) {
        LinearLayout c = card(a); LinearLayout row = titleRow(a, c, title, null);
        int v; try { v = Integer.parseInt(GameSettings.get(a, key)); } catch (NumberFormatException e) { v = def; }
        v = Math.max(min, Math.min(max, v));
        TextView val = text(a, fmt.apply(v), 14, 0xFFF2F2F5, true); row.addView(val);
        SeekBar sb = new SeekBar(a); sb.setMax(max - min); sb.setProgress(v - min); sb.setPadding(dp(a, 10), dp(a, 14), dp(a, 10), dp(a, 4));
        // neutral, not the game's accent all over (a purple bar and knob on Paldea were too much): a thin rounded grey track, the filled part near-white, a white knob with a soft ring
        android.graphics.drawable.GradientDrawable track = new android.graphics.drawable.GradientDrawable(); track.setColor(0x33FFFFFF); track.setCornerRadius(dp(a, 3)); track.setSize(0, dp(a, 4));
        android.graphics.drawable.GradientDrawable fill = new android.graphics.drawable.GradientDrawable(); fill.setColor(0xFFE6E6EB); fill.setCornerRadius(dp(a, 3)); fill.setSize(0, dp(a, 4));
        android.graphics.drawable.LayerDrawable bar = new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] { track,
            new android.graphics.drawable.ClipDrawable(fill, android.view.Gravity.START, android.graphics.drawable.ClipDrawable.HORIZONTAL) });
        bar.setId(0, android.R.id.background); bar.setId(1, android.R.id.progress);
        bar.setLayerHeight(0, dp(a, 4)); bar.setLayerHeight(1, dp(a, 4)); bar.setLayerGravity(0, android.view.Gravity.CENTER_VERTICAL); bar.setLayerGravity(1, android.view.Gravity.CENTER_VERTICAL);
        sb.setProgressDrawable(bar); sb.setMinHeight(dp(a, 4)); sb.setMaxHeight(dp(a, 4));
        android.graphics.drawable.GradientDrawable knob = new android.graphics.drawable.GradientDrawable(); knob.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        knob.setColor(0xFFFFFFFF); knob.setStroke(dp(a, 3), 0x40FFFFFF); knob.setSize(dp(a, 20), dp(a, 20));
        sb.setThumb(knob); sb.setThumbOffset(dp(a, 10)); sb.setSplitTrack(false); sb.setBackground(null);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int p, boolean user) { val.setText(fmt.apply(p + min)); if (user) set(a, key, String.valueOf(p + min)); }
            public void onStartTrackingTouch(SeekBar b) { }
            public void onStopTrackingTouch(SeekBar b) { }
        });
        c.addView(sb);
    }
    private static void note(Activity a, String s) { TextView n = text(a, s, 13, SUB, false); n.setPadding(dp(a, 6), dp(a, 2), dp(a, 6), dp(a, 10)); body.addView(n); }
    private static TextView button(Activity a, LinearLayout parent, String label, boolean primary, Runnable r) {
        TextView b = text(a, label, 14, primary ? Color.WHITE : TEXT, true); b.setGravity(Gravity.CENTER); b.setPadding(dp(a, 16), dp(a, 9), dp(a, 16), dp(a, 9));
        b.setBackground(primary ? round(accent(a), dp(a, 18), 0, 0) : round(CHIP, dp(a, 18), 1, CARD_LINE)); b.setOnClickListener(v -> r.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.rightMargin = dp(a, 8); lp.topMargin = dp(a, 8); parent.addView(b, lp); return b;
    }
    private static EditText field(Activity a, LinearLayout parent, String hint) {
        EditText e = new EditText(a); e.setHint(hint); e.setSingleLine(); e.setTextColor(TEXT); e.setHintTextColor(SUB); e.setTextSize(15);
        e.setBackground(round(CHIP, dp(a, 12), 1, CARD_LINE)); e.setPadding(dp(a, 12), dp(a, 10), dp(a, 12), dp(a, 10));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(a, 8); parent.addView(e, lp); return e;
    }

    private static void set(Activity a, String key, String value) {
        String was = GameSettings.get(a, key);
        GameSettings.set(a, key, value);
        boolean now;
        if (key.equals("DM_PERF_OVERLAY")) { if (a instanceof GameActivity) ((GameActivity) a).perfOverlay(value.equals("on")); now = true; }
        else if (key.equals("DM_LANG")) now = true;
        else if (key.equals("DM_RUMBLE")) now = !value.equals("off") && !was.equals("off");   // (controller / phone: at once; off and back on: the game's motors, from the next start)
        else { try { now = nativeSetting(key, value); } catch (UnsatisfiedLinkError e) { now = false; } }
        if (!now) RESTART.add(key);
    }

    // ---------- the pages ----------
    private static void display(Activity a) {
        heading(a, t(a, "Screen"));
        choice(a, "DM_ASPECT", t(a, "Picture"), new String[][] { { "", "16:9", t(a, "16:9, as the game is made") }, { "fill", t(a, "Fill"), t(a, "Fill the screen (stretched)") },
                                                                    { "20:9", "20:9", t(a, "20:9, for a widescreen mod") } });
        String rec = GameSettings.profile(a)[6].equals("sgsr") ? "SGSR" : t(a, "Sharp");
        choice(a, "DM_SCALER", t(a, "Upscaling"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s", rec) },
            { "sharp", t(a, "Sharp"), t(a, "Sharp: the picture upscaled with sharpening (lighter settings keep their detail)") },
            { "sgsr", "SGSR", t(a, "Snapdragon Game Super Resolution (Qualcomm's upscaler)") }, { "smooth", t(a, "Smooth"), t(a, "Smooth: a plain stretch") } });
        heading(a, t(a, "Overlay"));
        choice(a, "DM_PERF_OVERLAY", t(a, "Performance overlay"), new String[][] { { "", t(a, "Off"), "" }, { "on", t(a, "On"), t(a, "On: fps, temperature, GPU, memory, battery") } });
    }
    private static void perf(Activity a) {
        String[] p = GameSettings.profile(a);
        note(a, t(a, "This phone: %s", p[1]));
        heading(a, t(a, "Speed"));
        String g = t(a, p[2].equals("high") ? "High" : p[2].equals("auto") ? "Adaptive" : "Low");
        String[][] gfx = { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s", g) }, { "high", t(a, "High"), t(a, "High: TV mode, 1080p, full effects") },
            { "low", t(a, "Low"), t(a, "Low: handheld mode, 720p, lighter") }, { "auto", t(a, "Adaptive"), t(a, "Adaptive: High, a lighter picture while the phone is hot or slow") },
            // Lowest: handheld mode with the game's 3D render scale capped (Pokémon Sword / Shield: the engine's hle_resscale.c; Scarlet / Violet have no such scale)
            { "lowest", t(a, "Lowest"), t(a, "Lowest: handheld mode, 3D at 960x540, coolest") } };
        choice(a, "DM_GRAPHICS", t(a, "Graphics"), GameSettings.paldea(a) ? java.util.Arrays.copyOf(gfx, 4) : gfx);
        if (GameSettings.has60(a)) choice(a, "DM_FPS_TARGET", t(a, "Frame rate"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s fps", p[5]) },
            { "30", "30 fps", t(a, "30 fps, as the game is made") }, { "60", "60 fps", GameSettings.paldea(a) ? t(a, "Up to 60 fps: faster where the phone can, game speed stays right (Fl4sh's Dynamic FPS mod)")
                                                                                               : t(a, "60 fps: smoother; drops to 720p when needed to hold 60 (with Graphics on Auto or Adaptive)") } });
        heading(a, t(a, "Memory"));
        choice(a, "DM_VK_TEX_QUALITY", t(a, "Textures"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s", t(a, p[3].equals("half") ? "Half" : "Full")) },
            { "full", t(a, "Full"), t(a, "Full: the game's textures as they are") }, { "half", t(a, "Half"), t(a, "Half: large textures at half size, less memory and GPU work") } });
        choice(a, "DM_VK_BC_TRANSCODE", t(a, "Texture format"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: compact on GPUs without BC (Mali, Xclipse, PowerVR), converted once and kept") },
            { "off", t(a, "Full decode"), t(a, "Full decode: more memory, no conversion (Snapdragon is not affected either way)") } });
    }
    private static void controls(Activity a) {
        heading(a, t(a, "Controller"));
        choice(a, "DM_PAD_LAYOUT", t(a, "Controller buttons"), new String[][] { { "", t(a, "By position"), t(a, "By position, as on a Switch pad") },
                                                                                 { "labels", t(a, "By label"), t(a, "By label (the button marked A is A)") } });
        action(a, t(a, "Remap controller buttons"), t(a, "Or hold − for 3 s on a controller. Hold − and + together for 1 s to open this menu."), () -> go("remap"));
        choice(a, "DM_RUMBLE", t(a, "Rumble"), new String[][] { { "", t(a, "On"), t(a, "On: the controller's motor, else the phone's") }, { "pad", t(a, "Controllers only"), "" },
                                                                 { "off", t(a, "Off"), "" } });
        heading(a, t(a, "Touch controls"));
        slider(a, t(a, "Opacity"), "DM_TOUCH_OPACITY", 20, 100, 100, v -> v + " %");
        slider(a, t(a, "Fade after"), "DM_TOUCH_FADE", 0, 10, 3, v -> v == 0 ? t(a, "never fade") : t(a, "%d s without a touch", v));
        action(a, t(a, "Move the touch controls"), t(a, "Drag a control to move it, pinch to resize, double-tap to hide. Hold − for 2 s to save (hold + to reset)."), () -> {
            if (dlg != null) dlg.dismiss(); try { nativeEditTouch(); } catch (UnsatisfiedLinkError e) { } });
    }
    // Remapping (PadMapping.java, DM_PAD_MAPPING): tap a Switch button, then press the controller key for it (the panel takes the key: remapKey)
    private static String capture;
    private static void remap(Activity a) {
        Map<String, Integer> map = PadMapping.parse(GameSettings.get(a, "DM_PAD_MAPPING"));
        heading(a, t(a, "Remap controller buttons"));
        note(a, t(a, "Tap a button, then press the controller key you want for it."));
        for (String[] btn : PadMapping.BUTTONS) {
            LinearLayout c = card(a); LinearLayout row = titleRow(a, c, btn[1], null);
            Integer kc = map.get(btn[0]); boolean wait = btn[0].equals(capture);
            row.addView(text(a, wait ? t(a, "Press a key…") : kc != null ? PadMapping.keyName(kc) : t(a, "default"), 14, wait ? accent(a) : SUB, wait));
            if (wait) c.setBackground(round(CARD, dp(a, 16), dp(a, 2), accent(a)));
            c.setOnClickListener(x -> { capture = btn[0]; redraw(); });
        }
        LinearLayout r = new LinearLayout(a); body.addView(r);
        button(a, r, t(a, "Reset all"), false, () -> { set(a, "DM_PAD_MAPPING", ""); capture = null; say(t(a, "Controller buttons reset.")); redraw(); });
        button(a, r, t(a, "Done"), true, () -> { capture = null; go("controls"); });
    }
    private static boolean remapKey(int kc, KeyEvent e) {
        if (capture == null || e.getAction() != KeyEvent.ACTION_DOWN || !PadMapping.isPadKey(e)) return false;
        Map<String, Integer> map = PadMapping.parse(GameSettings.get(act, "DM_PAD_MAPPING"));
        map.values().removeIf(v -> v == kc);   // a key drives one Switch button
        map.put(capture, kc); set(act, "DM_PAD_MAPPING", PadMapping.serialize(map)); capture = null;
        redraw(); return true;
    }

    // ---------- mods ----------
    // <files>/mods/<mod>/romfs (files that replace the game's) and <mod>/exefs/*.pchtxt (code patches for this game version), as the Switch emulators lay them out;
    // loose .pchtxt files in mods/ work too. Switching one on or off renames its folder (a leading "-" switches it off, android_main.c / loader.c); applies at the next start.
    // Load order: mods/order.txt, one name a line (without the "-"), first = wins where two mods replace the same file (android_main.c mods_romfs); mods it does not list
    // come after, by name. Notes: <mod>/notes.txt (the mod's own readme when the zip has one, or what the player writes)
    private static File modsDir(Activity a) { return new File(GameSettings.dir(a), "mods"); }
    private static String plain(File f) { String n = f.getName(); return n.startsWith("-") ? n.substring(1) : n; }
    private static List<String> order(File dir) {
        List<String> o = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(dir, "order.txt")))) { String l; while ((l = r.readLine()) != null) if (!l.trim().isEmpty()) o.add(l.trim()); } catch (IOException e) { }
        return o;
    }
    private static void saveOrder(Activity a, List<File> ms) {
        try (FileWriter w = new FileWriter(new File(modsDir(a), "order.txt"))) { for (File f : ms) w.write(plain(f) + "\n"); } catch (IOException e) { }
    }
    private static List<File> modList(Activity a) {
        File[] fs = modsDir(a).listFiles(); List<File> all = new ArrayList<>(); if (fs == null) return all;
        Arrays.sort(fs); for (File f : fs) if (f.isDirectory() && !f.getName().startsWith(".") && !f.getName().equals("romfs")) all.add(f);
        List<File> out = new ArrayList<>();
        for (String n : order(modsDir(a))) for (File f : all) if (plain(f).equals(n) && !out.contains(f)) out.add(f);
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
    private static void mods(Activity a) {
        List<File> ms = modList(a); Map<File, Map<File, Integer>> cf = conflicts(ms);
        heading(a, t(a, "Mods (%d)", ms.size()));
        if (RESTART.contains("mods")) note(a, t(a, "Mods change from the next start."));
        LinearLayout top = new LinearLayout(a); body.addView(top);
        button(a, top, t(a, "Import a mod (.zip)"), true, () -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_MOD));
        if (ms.size() > 1) button(a, top, t(a, "Order and notes"), false, () -> go("order"));
        if (ms.isEmpty()) { note(a, t(a, "No mods yet.") + " " + where(a)); return; }
        for (File m : ms) {
            boolean on = !m.getName().startsWith("-");
            LinearLayout c = card(a); LinearLayout row = titleRow(a, c, plain(m), null); row.addView(pill(a, on ? t(a, "On") : t(a, "Off"), on));
            if (cf.containsKey(m)) { TextView w = text(a, "⚠ " + t(a, "shares files with %d other mod(s)", cf.get(m).size()), 13, WARN, false); w.setPadding(0, dp(a, 4), 0, 0); c.addView(w); }
            c.setOnClickListener(v -> { if (m.renameTo(new File(m.getParentFile(), on ? "-" + plain(m) : plain(m)))) RESTART.add("mods"); redraw(); });
        }
        note(a, where(a));
    }
    private static String where(Activity a) {
        return t(a, "Put each mod in its own folder in Android/data/%s/files/mods/ — <mod>/romfs for files, <mod>/exefs for .pchtxt code patches (they must be made for %s %s).",
                 a.getPackageName(), GameSettings.game(a)[1], GameSettings.version(a));
    }
    // the load order: higher wins; each mod's notes, the files it shares, moving it, deleting it
    private static void order(Activity a) {
        List<File> ms = modList(a); Map<File, Map<File, Integer>> cf = conflicts(ms);
        heading(a, t(a, "Load order: higher wins"));
        for (int i = 0; i < ms.size(); i++) {
            File m = ms.get(i); final int k = i; LinearLayout c = card(a);
            titleRow(a, c, (i + 1) + ". " + plain(m) + (m.getName().startsWith("-") ? " (" + t(a, "off") + ")" : ""), null);
            String n = ""; try { n = new String(java.nio.file.Files.readAllBytes(new File(m, "notes.txt").toPath()), java.nio.charset.StandardCharsets.UTF_8).trim(); } catch (IOException e) { }
            TextView nt = text(a, n.isEmpty() ? t(a, "No notes.") : n, 13, SUB, false); nt.setMaxLines(6); nt.setPadding(0, dp(a, 4), 0, 0); c.addView(nt);
            Map<File, Integer> sh = cf.get(m);
            if (sh != null) for (Map.Entry<File, Integer> e : sh.entrySet()) {
                boolean wins = ms.indexOf(m) < ms.indexOf(e.getKey());
                TextView w = text(a, "⚠ " + t(a, wins ? "Replaces %d of the same files as %s: this mod's are used (it is higher)." : "Replaces %d of the same files as %s: that mod's are used (it is higher).", e.getValue(), plain(e.getKey())), 13, WARN, false);
                w.setPadding(0, dp(a, 4), 0, 0); c.addView(w);
            }
            LinearLayout r = new LinearLayout(a); c.addView(r);
            if (i > 0) button(a, r, "↑ " + t(a, "Move up"), false, () -> { java.util.Collections.swap(ms, k, k - 1); saveOrder(a, ms); RESTART.add("mods"); redraw(); });
            if (i < ms.size() - 1) button(a, r, "↓ " + t(a, "Move down"), false, () -> { java.util.Collections.swap(ms, k, k + 1); saveOrder(a, ms); RESTART.add("mods"); redraw(); });
            button(a, r, t(a, "Edit notes"), false, () -> notes(a, m));
            button(a, r, t(a, "Delete"), false, () -> confirm(a, t(a, "Delete this mod from the phone?"), () -> { deleteTree(m); ms.remove(m); saveOrder(a, ms); RESTART.add("mods"); go("order"); }));
        }
        LinearLayout r = new LinearLayout(a); body.addView(r); button(a, r, t(a, "Done"), true, () -> go("mods"));
    }
    private static void notes(Activity a, File m) {
        body.removeAllViews(); heading(a, plain(m) + " · " + t(a, "Notes"));
        LinearLayout c = card(a); EditText e = field(a, c, ""); e.setSingleLine(false); e.setMinLines(5); e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        try { e.setText(new String(java.nio.file.Files.readAllBytes(new File(m, "notes.txt").toPath()), java.nio.charset.StandardCharsets.UTF_8)); } catch (IOException x) { }
        LinearLayout r = new LinearLayout(a); body.addView(r);
        button(a, r, t(a, "Save"), true, () -> { try (FileWriter fw = new FileWriter(new File(m, "notes.txt"))) { fw.write(e.getText().toString()); } catch (IOException x) { } go("order"); });
        button(a, r, t(a, "Cancel"), false, () -> go("order"));
    }
    private static void confirm(Activity a, String q, Runnable yes) {   // asked in a card at the top of the page, not a pop-up
        LinearLayout c = new LinearLayout(a); c.setOrientation(LinearLayout.VERTICAL); c.setBackground(round(CARD, dp(a, 16), dp(a, 2), WARN)); c.setPadding(dp(a, 16), dp(a, 12), dp(a, 16), dp(a, 12));
        c.addView(text(a, q, 15, TEXT, true)); LinearLayout r = new LinearLayout(a); c.addView(r);
        button(a, r, t(a, "Delete"), true, yes); button(a, r, t(a, "Cancel"), false, () -> go(page));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 10); body.addView(c, 0, lp); scroll.scrollTo(0, 0);
    }

    // ---------- saves, online, system ----------
    private static void saves(Activity a) {
        heading(a, t(a, "Saves"));
        action(a, t(a, "Export save data"), t(a, "Save in the game first: the export is the last save the game wrote."),
               () -> pick(a, Intent.ACTION_CREATE_DOCUMENT, GameSettings.game(a)[2] + "-save.zip", REQ_EXPORT));
        action(a, t(a, "Import save data"), t(a, "Your current save is kept as a backup."), () -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_IMPORT));
    }
    // Online room (hle_relay.c, dm/server): local play with players elsewhere through a room server someone hosts (their own Railway service, or any PC); everyone enters the
    // same server address and room code. Applies from the next start; mods stay off for local play, as on Wi-Fi
    private static void online(Activity a) {
        heading(a, t(a, "Online room"));
        note(a, t(a, "Play the game's local link battles, trades and raids with friends anywhere: everyone enters the same room server and room code. Host your own server (dm/server in the project: Railway or any PC). Mods must be off."));
        String room = GameSettings.get(a, "DM_RELAY_ROOM");
        LinearLayout c = card(a); titleRow(a, c, room.isEmpty() ? t(a, "off (local play on this Wi-Fi)") : t(a, "Room: %s", room), RESTART.contains("DM_RELAY_ROOM") ? t(a, "Applies next start") : null);
        EditText srv = field(a, c, t(a, "server (address:port)")); srv.setText(GameSettings.get(a, "DM_RELAY"));
        EditText code = field(a, c, t(a, "room code")); code.setText(room);
        LinearLayout r = new LinearLayout(a); c.addView(r);
        button(a, r, t(a, "Join"), true, () -> { String sv = srv.getText().toString().trim(), cd = code.getText().toString().trim();
            if (sv.isEmpty() || cd.isEmpty()) { say(t(a, "Enter both the server and the room code.")); return; }
            GameSettings.set(a, "DM_RELAY", sv); GameSettings.set(a, "DM_RELAY_ROOM", cd.replaceAll("\\s+", "")); RESTART.add("DM_RELAY_ROOM"); go("online"); });
        if (!room.isEmpty()) button(a, r, t(a, "Leave room"), false, () -> { GameSettings.set(a, "DM_RELAY_ROOM", ""); RESTART.add("DM_RELAY_ROOM"); go("online"); });
    }
    private static void system(Activity a) {
        heading(a, t(a, "Graphics driver"));
        action(a, t(a, "Graphics driver"), DriverPackages.label(DriverPackages.current(a)), () -> go("driver"));
        heading(a, t(a, "Language"));
        String[][] l = new String[T.LANGS.length][]; for (int i = 0; i < l.length; i++) l[i] = new String[] { T.LANGS[i][0], i == 0 ? t(a, "Auto") : T.LANGS[i][1], i == 0 ? t(a, "Auto (the phone's language)") : "" };
        choice(a, "DM_LANG", t(a, "Language"), l);
        heading(a, t(a, "Background"));
        choice(a, "DM_BACKGROUND", t(a, "Leaving the game"), new String[][] { { "", t(a, "Keep playing"), t(a, "Keep playing: the game and its sound go on in the background") },
            { "off", t(a, "Pause"), t(a, "Pause: the game stops while it is not on screen") } });
        // the replay on its own thread (hle_nvn.c DM_NVN_ASYNC): the game's thread hands each frame's graphics work over instead of translating it itself (about a quarter
        //   of a frame on a slow CPU, #99). Off by default until players have run it on many phones
        heading(a, t(a, "Performance"));
        choice(a, "DM_NVN_ASYNC", t(a, "Faster rendering (experimental)"), new String[][] { { "", t(a, "Off"), "" },
            { "1", t(a, "On"), t(a, "On: the graphics work runs on its own thread, next to the game's. Not tested on every phone yet: if the game misbehaves, turn it off and tell us on GitHub") } });
        // a frame capture for a bug report (nvn_vk.c, Frame capture): the engine reads capture.req (frames to wait, so the menu can be closed first) and writes the ZIP
        heading(a, t(a, "Bug reports"));
        action(a, t(a, "Capture a frame"), t(a, "Close this menu within 3 seconds and stay where the problem shows: the next frame is saved, step by step, as pictures and notes in Download (a ZIP ending in frame-capture.zip). Attach it to your issue."),
               () -> { try (java.io.FileWriter w = new java.io.FileWriter(new File(GameSettings.dir(a), "capture.req"))) { w.write("90"); } catch (IOException e) { }
                       android.widget.Toast.makeText(a, t(a, "Capturing in 3 seconds: close the menu."), android.widget.Toast.LENGTH_LONG).show(); });
        // Mali's white scenes (#47 #53 #64 #68 #69 #72 #75): switches the engine has for narrowing it down, which players could only set by editing env.txt; each one a
        //   tester tries (with a capture) rules a cause in or out. Shown on Mali phones only
        if (DriverPackages.gpu(a).contains("mali")) {
            heading(a, t(a, "White screen tests (Mali)"));
            note(a, t(a, "Try one at a time, restart the game, and tell us on GitHub whether the picture changed (a capture helps)."));
            choice(a, "DM_VK_UBO_SNAPSHOT", t(a, "Copy the game's uniform data"), new String[][] { { "", t(a, "Off"), "" },
                { "1", t(a, "On"), t(a, "On: each draw's uniform data is copied when it is recorded, instead of the GPU reading the game's memory directly") } });
            choice(a, "DM_VK_FLOAT_CONTROLS", t(a, "Exact infinities in shaders"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: on with ARM's driver (v0.52)") },
                { "0", t(a, "Off"), t(a, "Off: the shaders as before v0.52") } });
            choice(a, "DM_VK_NO_PASS_MERGE", t(a, "Merge render passes"), new String[][] { { "", t(a, "On"), "" },
                { "1", t(a, "Off"), t(a, "Off: every render target change starts a new pass") } });
        }
    }
    // the graphics driver: the phone's own, an installed package (Turnip, PanVK...), a new package from the file picker, or the releases page for this GPU; from the next start
    private static void driver(Activity a) {
        List<String[]> pk = DriverPackages.installed(a); String cur = DriverPackages.current(a); String[] src = DriverPackages.source(a);
        heading(a, t(a, "Graphics driver"));
        if (RESTART.contains("driver")) note(a, t(a, "The new driver is used from the next start."));
        driverRow(a, t(a, "The phone's own driver"), cur.isEmpty(), () -> DriverPackages.useSystem(a));
        for (String[] p : pk) driverRow(a, p[0], p[1].equals(cur), () -> { String why = DriverPackages.mismatch(a, new File(p[1]));
            if (why != null) android.widget.Toast.makeText(a, t(a, "Not used:") + " " + why, android.widget.Toast.LENGTH_LONG).show(); else DriverPackages.use(a, new File(p[1])); });
        LinearLayout r = new LinearLayout(a); body.addView(r);
        button(a, r, t(a, "Install a driver package (.zip)"), true, () -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_DRIVER));
        if (src != null) button(a, r, src[0], false, () -> a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(src[1]))));
        if (DriverPackages.isTurnip(cur)) {
            heading(a, t(a, "Turnip options (from the next start)"));
            Set<String> on = DriverPackages.turnipOptions(a);
            for (String[] o : DriverPackages.TURNIP_OPTIONS) {
                boolean v = on.contains(o[0]); LinearLayout c = card(a); LinearLayout row = titleRow(a, c, o[1], null); row.addView(pill(a, v ? t(a, "On") : t(a, "Off"), v));
                c.setOnClickListener(x -> { if (v) on.remove(o[0]); else on.add(o[0]); DriverPackages.setTurnipOptions(a, on); RESTART.add("driver"); redraw(); });
            }
        }
        LinearLayout d = new LinearLayout(a); body.addView(d); button(a, d, t(a, "Done"), false, () -> go("system"));
    }
    private static void driverRow(Activity a, String name, boolean on, Runnable use) {
        LinearLayout c = card(a); LinearLayout row = titleRow(a, c, name, null);
        if (on) { row.addView(text(a, "✓", 18, accent(a), true)); c.setBackground(round(CARD, dp(a, 16), dp(a, 2), accent(a))); }
        c.setOnClickListener(v -> { use.run(); RESTART.add("driver"); redraw(); });
    }

    // ---------- files: save data out and in, mods and drivers in (the system file picker; GameActivity.onActivityResult hands the answer here) ----------
    static final int REQ_EXPORT = 101, REQ_IMPORT = 102, REQ_MOD = 103, REQ_DRIVER = 104;
    private static void pick(Activity a, String action, String name, int req) {
        // (to open, any file: ZIPs saved by browsers and chat apps are often typed otherwise, and the picker greyed them out; a save ZIP is checked when read)
        Intent i = new Intent(action); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType(Intent.ACTION_OPEN_DOCUMENT.equals(action) ? "*/*" : "application/zip");
        if (name != null) i.putExtra(Intent.EXTRA_TITLE, name);
        a.startActivityForResult(i, req);
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
                        int n = SaveImport.install(in, new File(files, "save_import"), displayName(a, u));
                        msg = t(a, "Imported %d save file(s). Close the game and start it again to load it (your current save is kept as a backup).", n) + " (" + SaveImport.lastFiles + ")"; }
                } else {
                    String name = displayName(a, u).replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9 ._()\\[\\]-]", "_").trim();
                    if (name.isEmpty() || name.startsWith(".") || name.startsWith("-")) name = "mod";
                    File dest = new File(new File(files, "mods"), name);
                    try (InputStream in = a.getContentResolver().openInputStream(u)) { int n = installMod(in, dest);
                        msg = t(a, "Mod \"%s\" installed (%d files). It is used from the next start.", name, n); }
                }
            } catch (IOException | SecurityException e) {
                msg = t(a, req == REQ_EXPORT ? "Could not export the save: %s" : req == REQ_IMPORT ? "Could not import the save: %s" : "Could not install the mod: %s", e.getMessage()); }
            final String m = msg;
            a.runOnUiThread(() -> {   // (the answer in the panel's status line, on the page it belongs to)
                if (dlg == null) open(a, null);
                if (req == REQ_MOD) { RESTART.add("mods"); go("mods"); } else if (req == REQ_DRIVER) go("driver"); else go("saves");
                say(m);
            });
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
        // a mod packed from the game's own top folders, without a romfs/ above them (Team Seri's Scarlet / Violet mods: world/scene/...): those go under romfs/
        for (int i = 0; i < parts.length - 1; i++) if (ROMFS_TOPS.contains(parts[i].toLowerCase(java.util.Locale.ROOT))) { StringBuilder sb = new StringBuilder("romfs");
            for (int j = i; j < parts.length; j++) { if (parts[j].equals("..")) return null; sb.append('/').append(parts[j]); } return sb.toString(); }
        String last = parts[parts.length - 1];
        return last.toLowerCase(java.util.Locale.ROOT).endsWith(".pchtxt") && !last.contains("..") ? "exefs/" + last : null;
    }
    private static final Set<String> ROMFS_TOPS = new HashSet<>(Arrays.asList("world", "chara", "pokemon", "field", "field_graphic", "ui", "message", "script", "effect",
        "avalon", "ai", "gfx", "obj", "system_resource", "demo", "sound", "shader", "arc", "bin"));
    private static void deleteTree(File f) { File[] cs = f.listFiles(); if (cs != null) for (File c : cs) deleteTree(c); f.delete(); }
}
