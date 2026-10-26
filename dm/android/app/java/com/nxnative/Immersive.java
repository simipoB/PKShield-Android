// Full screen without the system bars: the navigation bar (recents, home, back) sat beside the on-screen A / B / R buttons, so a thumb on them could open recents
// and swipe the game away. The bars come back for a moment with a swipe from the edge. Called from the native front end (app/jni/android_main.c) each time the
// game window gains focus, since dialogs (the name entry) and returning from another app bring the bars back.
package com.nxnative;

import android.app.Activity;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

public final class Immersive {
    public static void apply(final Activity a) {
        a.runOnUiThread(new Runnable() {
            public void run() {
                Window w = a.getWindow();
                w.setDecorFitsSystemWindows(false);
                WindowInsetsController c = w.getInsetsController();
                if (c == null) return;
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(WindowInsets.Type.systemBars());
            }
        });
    }
}
