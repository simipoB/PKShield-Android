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
    static boolean viaSetup;   // this process started the game from the setup screen (SetupActivity.startGame)

    @Override protected void onCreate(Bundle b) {
        autoSettings();   // before super.onCreate: NativeActivity starts the engine there
        // local play (hle_ldn.c) finds other phones by Wi-Fi broadcasts, which many phones drop without a multicast lock
        try { android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
              if (wm != null) { android.net.wifi.WifiManager.MulticastLock ml = wm.createMulticastLock("local play"); ml.setReferenceCounted(false); ml.acquire(); } } catch (Exception e) { }
        super.onCreate(b);
        // Android brings the task back after the process ended (recents, the launcher icon) straight into this activity, past the setup screen that translates the
        // shaders the game captured last time: hand over to it before there is a window (the engine starts with the window, android_main.c)
        if (!viaSetup && SetupActivity.pendingWork(GameSettings.dir(this))) { startActivity(new Intent(this, SetupActivity.class)); finish(); return; }
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
        android.widget.Toast.makeText(this, T.t(this, "%s: its built-in controls are used. Hold − and + together for 1 s for the menu.", h), android.widget.Toast.LENGTH_LONG).show();
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
    @Override protected void onActivityResult(int req, int res, Intent data) { if (!GameMenu.onResult(this, req, res, data)) super.onActivityResult(req, res, data); }   // the in-game menu's file pickers
    private boolean liveRoom() {
        android.os.PowerManager pm = getSystemService(android.os.PowerManager.class);
        if (pm != null && pm.getCurrentThermalStatus() >= android.os.PowerManager.THERMAL_STATUS_MODERATE) return false;
        android.os.BatteryManager bm = getSystemService(android.os.BatteryManager.class);
        if (bm != null && !bm.isCharging() && bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY) < 20) return false;
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo(); getSystemService(android.app.ActivityManager.class).getMemoryInfo(mi);
        return !mi.lowMemory && mi.availMem - mi.threshold > (700L << 20);   // (room for a translator above Android's own low-memory line)
    }
    @Override protected void onDestroy() { liveOn = false; if (im != null) im.unregisterInputDeviceListener(this); super.onDestroy(); }
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
            android.system.Os.setenv("DM_AUTO_SCALER", p[6], true);
            StringBuilder hz = new StringBuilder(); for (float r : GameSettings.refreshRates(this)) hz.append(hz.length() > 0 ? "," : "").append(Math.round(r));
            android.system.Os.setenv("DM_DISPLAY_HZ", hz.toString(), true);
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
