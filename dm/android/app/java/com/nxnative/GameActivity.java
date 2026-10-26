// The game's activity: Android's NativeActivity (the engine, libnxnative.so) plus what only Java can see: which input devices are connected.
// A controller connected (Bluetooth / USB gamepad or joystick, e.g. a Joy-Con pair) hides the touch controls; disconnecting the last one shows them again.
// A touch on the screen still brings them back while a controller is connected (touch_ui.c).
package com.nxnative;

import android.app.NativeActivity;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.view.InputDevice;

public final class GameActivity extends NativeActivity implements InputManager.InputDeviceListener {
    static { System.loadLibrary("nxnative"); }   // loaded through this class's loader, so the native method below resolves (NativeActivity then opens the same library)
    private static native void nativePadConnected(boolean connected);
    private InputManager im;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        im = getSystemService(InputManager.class);
        if (im != null) im.registerInputDeviceListener(this, null);
        check();
    }
    @Override protected void onResume() { super.onResume(); check(); }
    @Override protected void onDestroy() { if (im != null) im.unregisterInputDeviceListener(this); super.onDestroy(); }
    @Override public void onInputDeviceAdded(int id) { check(); }
    @Override public void onInputDeviceRemoved(int id) { check(); }
    @Override public void onInputDeviceChanged(int id) { check(); }

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
