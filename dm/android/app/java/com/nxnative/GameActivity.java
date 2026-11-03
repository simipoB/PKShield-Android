// The game's activity: Android's NativeActivity (the engine, libnxnative.so) plus what only Java can see: which input devices are connected.
// A controller connected (Bluetooth / USB gamepad or joystick, e.g. a Joy-Con pair) hides the touch controls; disconnecting the last one shows them again.
// A touch on the screen still brings them back while a controller is connected (touch_ui.c).
package com.nxnative;

import android.app.NativeActivity;
import android.content.Intent;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.view.InputDevice;

public final class GameActivity extends NativeActivity implements InputManager.InputDeviceListener {
    static { System.loadLibrary("nxnative"); }   // loaded through this class's loader, so the native method below resolves (NativeActivity then opens the same library)
    private static native void nativePadConnected(boolean connected);
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
        if (!viaSetup && !SetupActivity.pendingCaptures(GameSettings.dir(this)).isEmpty()) { startActivity(new Intent(this, SetupActivity.class)); finish(); return; }
        im = getSystemService(InputManager.class);
        if (im != null) im.registerInputDeviceListener(this, null);
        check();
    }
    @Override protected void onResume() { super.onResume(); check(); }
    @Override protected void onActivityResult(int req, int res, Intent data) { if (!GameMenu.onResult(this, req, res, data)) super.onActivityResult(req, res, data); }   // the in-game menu's file pickers
    @Override protected void onDestroy() { if (im != null) im.unregisterInputDeviceListener(this); super.onDestroy(); }
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
            StringBuilder hz = new StringBuilder(); for (float r : GameSettings.refreshRates(this)) hz.append(hz.length() > 0 ? "," : "").append(Math.round(r));
            android.system.Os.setenv("DM_DISPLAY_HZ", hz.toString(), true);
        } catch (Exception e) { }
    }
    private void check() {
        boolean pad = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id); if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) pad = true;
        }
        try { nativePadConnected(pad); } catch (UnsatisfiedLinkError e) { }
    }
}
