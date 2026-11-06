// The game's activity: Android's NativeActivity (the engine, libnxnative.so) plus what only Java can see: which input devices are connected.
// A controller connected (Bluetooth / USB gamepad or joystick, e.g. a Joy-Con pair) hides the touch controls; disconnecting the last one shows them again.
// A touch on the screen still brings them back while a controller is connected (touch_ui.c).
package com.nxnative;

import android.app.NativeActivity;
import android.content.Intent;
import android.hardware.input.InputManager;
import android.os.Bundle;
import java.io.File;
import android.view.InputDevice;

public final class GameActivity extends NativeActivity implements InputManager.InputDeviceListener {
    static { System.loadLibrary("nxnative"); }   // loaded through this class's loader, so the native method below resolves (NativeActivity then opens the same library)
    private static native void nativePadConnected(boolean connected);
    private static native long nativeFrames();   // frames the engine has presented (android_main.c), -1 before it draws
    private InputManager im;
    static volatile boolean alive;   // the game runs in this process (SetupActivity: the app icon goes back to it rather than to the setup screen)
    static boolean viaSetup;   // this process started the game from the setup screen (SetupActivity.startGame)

    @Override protected void onCreate(Bundle b) {
        autoSettings();   // before super.onCreate: NativeActivity starts the engine there
        // local play (hle_ldn.c) finds other phones by Wi-Fi broadcasts, which many phones drop without a multicast lock
        try { android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
              if (wm != null) { android.net.wifi.WifiManager.MulticastLock ml = wm.createMulticastLock("local play"); ml.setReferenceCounted(false); ml.acquire(); } } catch (Exception e) { }
        super.onCreate(b);
        // Android brings the task back after the process ended (recents, the launcher icon) straight into this activity, past the setup screen that translates the
        // shaders the game captured last time: hand over to it before there is a window (the engine starts with the window, android_main.c)
        //   (alive only after this: set before it, the setup screen took this finishing activity for a running game and sent the player back to it, round and round;
        //   the game named, so the launcher of two games does not come in between)
        if (!viaSetup && SetupActivity.pendingWork(GameSettings.dir(this))) { startActivity(new Intent(this, SetupActivity.class).putExtra("game", GameSettings.game(this)[0])); finish(); return; }
        alive = true;
        im = getSystemService(InputManager.class);
        if (im != null) im.registerInputDeviceListener(this, null);
        check();
        liveShaders();
        handheld();
        if (GameSettings.get(this, "DM_PERF_OVERLAY").equals("on")) perfOverlay(true);
    }
    // ---------- rumble (hle_hid.c → touch_ui.c dm_rumble): the game's motor strength 0..1, about every 100 ms while it lasts, on the engine's haptic thread ----------
    // the controller's own motor when the one in use has one, else the phone's (DM_RUMBLE: "" both, "pad" controllers only, "off" none); each call is a 150 ms pulse
    private volatile android.os.Vibrator padVib; private android.os.Vibrator phoneVib; private String rumbleMode; private long rumbleRead;
    public void rumble(float amp) {
        long now = android.os.SystemClock.uptimeMillis();
        if (rumbleMode == null || now - rumbleRead > 3000) { rumbleMode = GameSettings.get(this, "DM_RUMBLE"); rumbleRead = now; }
        if (rumbleMode.equals("off")) return;
        android.os.Vibrator v = padVib;
        if (v == null && !rumbleMode.equals("pad")) { if (phoneVib == null) phoneVib = getSystemService(android.os.Vibrator.class); v = phoneVib; }
        if (v == null || !v.hasVibrator()) return;
        if (amp < 0.03f) { v.cancel(); return; }
        if (!v.hasAmplitudeControl() && amp < 0.3f) return;   // (an on / off motor: only the stronger rumbles, or it buzzes all the time)
        int a = v.hasAmplitudeControl() ? Math.max(1, Math.min(255, Math.round(amp * 255))) : android.os.VibrationEffect.DEFAULT_AMPLITUDE;
        try { v.vibrate(android.os.VibrationEffect.createOneShot(150, a)); } catch (RuntimeException e) { }
    }
    @SuppressWarnings("deprecation")
    private static android.os.Vibrator vibratorOf(InputDevice d) {
        android.os.Vibrator v = android.os.Build.VERSION.SDK_INT >= 31 ? d.getVibratorManager().getDefaultVibrator() : d.getVibrator();
        return v != null && v.hasVibrator() ? v : null;
    }
    // ---------- handhelds with built-in controls (GameSettings.handheld): the controls are a connected controller from the start, so the touch controls stay hidden;
    // told once how to reach the menu without them ----------
    private void handheld() {
        String h = GameSettings.handheld(); if (h == null || !GameSettings.get(this, "DM_HANDHELD_TOLD").isEmpty()) return;
        GameSettings.set(this, "DM_HANDHELD_TOLD", "1");
        hint(T.t(this, "%s: its built-in controls are used. Hold − and + together for 1 s for the menu.", h));
    }
    // a one-line hint at the top of the screen for a few seconds: not a pop-up (it takes no touch and no focus), the game keeps running and keeps its input
    private void hint(String s) {
        android.view.View root = getWindow().getDecorView();
        root.postDelayed(() -> {
            android.widget.TextView t = new android.widget.TextView(this); t.setText(s); t.setTextColor(0xFFFFFFFF); t.setTextSize(13);
            android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable(); g.setColor(0xCC121419); g.setCornerRadius(40); t.setBackground(g);
            int p = Math.round(12 * getResources().getDisplayMetrics().density); t.setPadding(p * 2, p, p * 2, p);
            android.widget.PopupWindow w = new android.widget.PopupWindow(t, -2, -2, false); w.setTouchable(false);
            try { w.showAtLocation(root, android.view.Gravity.TOP | android.view.Gravity.CENTER_HORIZONTAL, 0, p * 2); } catch (RuntimeException e) { return; }
            root.postDelayed(() -> { try { w.dismiss(); } catch (RuntimeException e) { } }, 5000);
        }, 3000);
    }
    // ---------- the performance overlay (☰ → Performance overlay): fps, temperature, GPU, free memory, battery, once a second at the top left ----------
    private android.widget.PopupWindow perf; private android.os.Handler perfH; private long perfFrames = -1, perfT;
    void perfOverlay(boolean on) {
        runOnUiThread(() -> {
            if (!on) { if (perf != null) perf.dismiss(); perf = null; return; }
            if (perf != null) return;
            android.widget.TextView t = new android.widget.TextView(this); t.setTextColor(0xFFFFFFFF); t.setTextSize(11); t.setShadowLayer(3, 0, 0, 0xFF000000);
            t.setBackgroundColor(0x66000000); t.setPadding(12, 4, 12, 4);
            perf = new android.widget.PopupWindow(t, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, false);
            perf.setTouchable(false);
            android.view.View root = getWindow().getDecorView();
            root.post(() -> { if (perf != null) try { perf.showAtLocation(root, android.view.Gravity.TOP | android.view.Gravity.START, 16, 16); } catch (RuntimeException e) { perf = null; } });
            if (perfH == null) perfH = new android.os.Handler(android.os.Looper.getMainLooper());
            perfH.post(new Runnable() { public void run() { if (perf == null) return; t.setText(perfLine()); perfH.postDelayed(this, 1000); } });
        });
    }
    private String gpuName;
    private String perfLine() {
        long f = -1, now = android.os.SystemClock.uptimeMillis(); try { f = nativeFrames(); } catch (UnsatisfiedLinkError e) { }
        String fps = "–"; if (f >= 0 && perfFrames >= 0 && now > perfT) fps = String.valueOf(Math.round((f - perfFrames) * 1000.0 / (now - perfT))); perfFrames = f; perfT = now;
        Intent bat = registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        float temp = bat != null ? bat.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, 0) / 10f : 0;
        int level = bat != null ? bat.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1) * 100 / Math.max(1, bat.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100)) : -1;
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo(); getSystemService(android.app.ActivityManager.class).getMemoryInfo(mi);
        if (gpuName == null || gpuName.isEmpty()) { gpuName = ""; try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(new File(GameSettings.dir(this), "gpu.txt")))) { String l = r.readLine(); if (l != null) gpuName = l.trim(); } catch (java.io.IOException e) { } }
        android.os.PowerManager pm = getSystemService(android.os.PowerManager.class); String heat = "";
        if (pm != null) { int s = pm.getCurrentThermalStatus(); heat = s >= android.os.PowerManager.THERMAL_STATUS_SEVERE ? " " + T.t(this, "hot") : s >= android.os.PowerManager.THERMAL_STATUS_MODERATE ? " " + T.t(this, "warm") : ""; }
        return String.format(java.util.Locale.ROOT, "%s fps · %.0f °C%s · %s · %s %.1f GB · %d%%", fps, temp, heat, gpuName.isEmpty() ? "GPU" : gpuName, T.t(this, "free"), mi.availMem / 1e9, level);
    }
    // Shaders the game saves while it runs (a mod's new effect, nvn_vk.c mtl_shader_seen): translated now in the background into the folder the engine watches
    // (setup_game.py --captures --live, nvn_vk.c live_check), so they draw a few seconds later instead of after a restart. Kept out of the game's way on every phone, nothing
    // to set: the small cores only, the lowest priority, killed before the game when memory runs short (setup_game.py live_yield); one worker under 8 GB, two above;
    // none while the phone is hot, low on battery off the charger, or short of free memory (then the shader waits for the next start, as before)
    private volatile boolean liveOn = true;
    private void liveShaders() {
        File dir = GameSettings.dir(this), live = new File(dir, "live"), cap = new File(dir, "nvn_capture"); java.util.Set<String> tried = new java.util.HashSet<>();
        Thread t = new Thread(() -> {
            while (liveOn) {
                try { Thread.sleep(10000); } catch (InterruptedException e) { return; }
                java.util.List<String> todo = SetupActivity.pendingCaptures(dir); todo.removeAll(tried); if (todo.isEmpty()) continue;
                if (!liveRoom()) continue;
                tried.addAll(todo);   // (once a run: a failure is left to the setup screen's next start)
                try {
                    Process p = SetupActivity.startTool(this, java.util.Arrays.asList("--captures", cap.getPath(), "--live", live.getPath()), GameSettings.memoryGb(this) < 7.5 ? 1 : 2); if (p == null) return;
                    try (java.io.InputStream in = p.getInputStream()) { byte[] b = new byte[4096]; while (in.read(b) >= 0) { } }
                    if (p.waitFor() == 0) try (java.io.FileWriter w = new java.io.FileWriter(new File(dir, "captures_done"), true)) { for (String n : todo) w.write(n + "\n"); }
                } catch (Exception e) { }
            }
        }, "live shaders");
        t.setPriority(Thread.MIN_PRIORITY); t.setDaemon(true); t.start();
    }
    @Override protected void onResume() { super.onResume(); check(); }
    // am start ... --es menu <page>: the settings panel (testing over adb; 1 = the controller buttons)
    @Override protected void onNewIntent(Intent i) { super.onNewIntent(i); String m = i.getStringExtra("menu"); if (m != null) GameMenu.show(this, m.equals("1") ? 1 : 0); }
    @Override protected void onActivityResult(int req, int res, Intent data) { if (!GameMenu.onResult(this, req, res, data)) super.onActivityResult(req, res, data); }   // the in-game menu's file pickers
    private boolean liveRoom() {
        android.os.PowerManager pm = getSystemService(android.os.PowerManager.class);
        if (pm != null && pm.getCurrentThermalStatus() >= android.os.PowerManager.THERMAL_STATUS_MODERATE) return false;
        android.os.BatteryManager bm = getSystemService(android.os.BatteryManager.class);
        if (bm != null && !bm.isCharging() && bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY) < 20) return false;
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo(); getSystemService(android.app.ActivityManager.class).getMemoryInfo(mi);
        return !mi.lowMemory && mi.availMem - mi.threshold > (700L << 20);   // (room for a translator above Android's own low-memory line)
    }
    @Override protected void onDestroy() { liveOn = false; alive = false; if (im != null) im.unregisterInputDeviceListener(this); super.onDestroy(); }
    @Override public void onInputDeviceAdded(int id) { check(); }
    @Override public void onInputDeviceRemoved(int id) { check(); }
    @Override public void onInputDeviceChanged(int id) { check(); }

    // What this phone is (GameSettings.profile: CPU, GPU and memory tiers, the display's refresh rates), handed to the engine as the Auto defaults; the player's
    // own choices (env.txt) win over them (android_main.c device_profile)
    private void autoSettings() {
        try {
            String[] p = GameSettings.profile(this);
            android.system.Os.setenv("DM_AUTO_GRAPHICS", p[2], true); android.system.Os.setenv("DM_AUTO_TEX", p[3], true);
            android.system.Os.setenv("DM_AUTO_ANISO", p[4], true); android.system.Os.setenv("DM_AUTO_FPS", p[5], true); android.system.Os.setenv("DM_AUTO_WHAT", p[1], true);
            android.system.Os.setenv("DM_AUTO_SCALER", p[6], true); android.system.Os.setenv("DM_AUTO_SHADOWS", p[7], true);
            // Sword / Shield: no split submissions (nvn_vk.c sub_split): they need none (3-5 ms of work a frame on a Galaxy S25+) and with them Sword flashed white
            //   (v0.40, pulled); Scarlet / Violet keep them, they hold 30 fps with them. env.txt's own DM_VK_SPLIT wins
            if (!GameSettings.paldea(this) && GameSettings.get(this, "DM_VK_SPLIT").isEmpty()) android.system.Os.setenv("DM_VK_SPLIT", "0", true);
            StringBuilder hz = new StringBuilder(); for (float r : GameSettings.refreshRates(this)) hz.append(hz.length() > 0 ? "," : "").append(Math.round(r));
            android.system.Os.setenv("DM_DISPLAY_HZ", hz.toString(), true);
        } catch (Exception e) { }
        // The Chinese / Korean shared-font stand-in (hle_sdk16.c, #62: a game set to Chinese drew none of its text) into the game's font/, for setups made before it
        //   shipped; the APK carries it (build_apk.py), copied when missing or of another size
        try {
            File fd = new File(GameSettings.dir(this), "font"); File cjk = new File(fd, "cjk.ttf");
            android.content.res.AssetFileDescriptor afd = null; long want = -1;
            try { afd = getAssets().openFd("cjk_fallback.ttf"); want = afd.getLength(); } catch (java.io.IOException ignored) { } finally { if (afd != null) afd.close(); }
            if (fd.isDirectory() && (!cjk.isFile() || (want > 0 && cjk.length() != want))) {
                File tmp = new File(fd, "cjk.ttf.part");
                try (java.io.InputStream in = getAssets().open("cjk_fallback.ttf"); java.io.OutputStream out = new java.io.FileOutputStream(tmp)) {
                    byte[] b = new byte[1 << 16]; int n; while ((n = in.read(b)) > 0) out.write(b, 0, n); }
                if (!tmp.renameTo(cjk)) tmp.delete();
            }
        } catch (Exception e) { }
        // Why Android ended the previous run (Android 11+): a game that vanished with no crash in its log was killed from outside (memory, heat, the phone's own
        //   game manager: #57's log simply stops). Said at the top of the new log (android_main.c boot lines)
        if (android.os.Build.VERSION.SDK_INT >= 30) try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            java.util.List<android.app.ApplicationExitInfo> ex = am.getHistoricalProcessExitReasons(null, 0, 1);
            if (!ex.isEmpty()) { android.app.ApplicationExitInfo e = ex.get(0);
                String why = "reason " + e.getReason() + ", status " + e.getStatus() + ", importance " + e.getImportance() + ", " + (e.getPss() >> 10) + " MB" + (e.getDescription() != null ? ", " + e.getDescription() : "");
                android.system.Os.setenv("DM_LAST_EXIT", why.replace('\n', ' '), true); }
        } catch (Exception e) { }
    }
    private void check() {
        boolean pad = false; android.os.Vibrator pv = null;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id); if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) { pad = true; if (pv == null) pv = vibratorOf(d); }
        }
        padVib = pv;   // (a handheld's built-in controls usually have no motor of their own: its body's, the phone vibrator, rumbles instead)
        try { nativePadConnected(pad); } catch (UnsatisfiedLinkError e) { }
    }
}
