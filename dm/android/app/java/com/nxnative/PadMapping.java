package com.nxnative;

import android.view.KeyEvent;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

// Controller button remapping, as the settings screen keeps it in env.txt: DM_PAD_MAPPING=<Switch button>:<Android key code>,... on top of DM_PAD_LAYOUT
// (position / labels). android_main.c pad_map_init reads both; a button named here leaves whatever key it had
final class PadMapping {
    // the Switch buttons, by the names android_main.c uses, with what the settings screen calls them
    static final String[][] BUTTONS = {
        { "A", "A" }, { "B", "B" }, { "X", "X" }, { "Y", "Y" }, { "L", "L" }, { "R", "R" }, { "ZL", "ZL" }, { "ZR", "ZR" },
        { "LS", "Left stick press" }, { "RS", "Right stick press" }, { "PLUS", "+" }, { "MINUS", "−" },
        { "UP", "D-pad up" }, { "DOWN", "D-pad down" }, { "LEFT", "D-pad left" }, { "RIGHT", "D-pad right" },
    };

    static Map<String, Integer> parse(String s) {
        Map<String, Integer> m = new LinkedHashMap<>();
        if (s == null) return m;
        for (String t : s.split(",")) {
            int c = t.indexOf(':'); if (c <= 0) continue;
            try { int kc = Integer.parseInt(t.substring(c + 1).trim()); if (kc > 0) m.put(t.substring(0, c).trim().toUpperCase(Locale.ROOT), kc); } catch (NumberFormatException e) { }
        }
        return m;
    }

    static String serialize(Map<String, Integer> m) {
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, Integer> e : m.entrySet()) { if (b.length() > 0) b.append(','); b.append(e.getKey()).append(':').append(e.getValue()); }
        return b.toString();
    }

    // a key code as the controller's button reads (KEYCODE_BUTTON_A → "Button A")
    static String keyName(int kc) {
        String n = KeyEvent.keyCodeToString(kc);
        if (n.startsWith("KEYCODE_")) n = n.substring(8);
        n = n.replace("BUTTON_", "Button ").replace("DPAD_", "D-pad ").replace('_', ' ');
        return n.isEmpty() ? "key " + kc : n.charAt(0) + n.substring(1).toLowerCase(Locale.ROOT).replace("button ", "Button ").replace("d-pad ", "D-pad ");
    }

    // keys a controller sends (not the phone's own buttons): what the remap dialog accepts
    static boolean isPadKey(KeyEvent e) {
        int kc = e.getKeyCode();
        return KeyEvent.isGamepadButton(kc) || (e.getSource() & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
            && (kc == KeyEvent.KEYCODE_DPAD_UP || kc == KeyEvent.KEYCODE_DPAD_DOWN || kc == KeyEvent.KEYCODE_DPAD_LEFT || kc == KeyEvent.KEYCODE_DPAD_RIGHT);
    }
}
