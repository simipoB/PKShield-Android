package com.nxnative;

import java.util.LinkedHashMap;
import java.util.Map;

public final class PadMapping {
    // Android KeyEvent standard keycode constants
    public static final int KEYCODE_BACK = 4;
    public static final int KEYCODE_DPAD_UP = 19;
    public static final int KEYCODE_DPAD_DOWN = 20;
    public static final int KEYCODE_DPAD_LEFT = 21;
    public static final int KEYCODE_DPAD_RIGHT = 22;
    public static final int KEYCODE_BUTTON_A = 96;
    public static final int KEYCODE_BUTTON_B = 97;
    public static final int KEYCODE_BUTTON_C = 98;
    public static final int KEYCODE_BUTTON_X = 99;
    public static final int KEYCODE_BUTTON_Y = 100;
    public static final int KEYCODE_BUTTON_Z = 101;
    public static final int KEYCODE_BUTTON_L1 = 102;
    public static final int KEYCODE_BUTTON_R1 = 103;
    public static final int KEYCODE_BUTTON_L2 = 104;
    public static final int KEYCODE_BUTTON_R2 = 105;
    public static final int KEYCODE_BUTTON_THUMBL = 106;
    public static final int KEYCODE_BUTTON_THUMBR = 107;
    public static final int KEYCODE_BUTTON_START = 108;
    public static final int KEYCODE_BUTTON_SELECT = 109;
    public static final int KEYCODE_BUTTON_MODE = 110;

    public static final class PadButton {
        public final String id;
        public final String labelEn;
        public final String labelEs;
        public final int defXbox;
        public final int defNintendo;

        public PadButton(String id, String labelEn, String labelEs, int defXbox, int defNintendo) {
            this.id = id;
            this.labelEn = labelEn;
            this.labelEs = labelEs;
            this.defXbox = defXbox;
            this.defNintendo = defNintendo;
        }

        public String label(boolean spanish) {
            return spanish ? labelEs : labelEn;
        }
    }

    public static final PadButton[] BUTTONS = {
        new PadButton("A", "Button A", "Botón A", KEYCODE_BUTTON_B, KEYCODE_BUTTON_A),
        new PadButton("B", "Button B", "Botón B", KEYCODE_BUTTON_A, KEYCODE_BUTTON_B),
        new PadButton("X", "Button X", "Botón X", KEYCODE_BUTTON_Y, KEYCODE_BUTTON_X),
        new PadButton("Y", "Button Y", "Botón Y", KEYCODE_BUTTON_X, KEYCODE_BUTTON_Y),
        new PadButton("L", "Button L", "Botón L", KEYCODE_BUTTON_L1, KEYCODE_BUTTON_L1),
        new PadButton("R", "Button R", "Botón R", KEYCODE_BUTTON_R1, KEYCODE_BUTTON_R1),
        new PadButton("ZL", "Button ZL (LT)", "Botón ZL (LT)", KEYCODE_BUTTON_L2, KEYCODE_BUTTON_L2),
        new PadButton("ZR", "Button ZR (RT)", "Botón ZR (RT)", KEYCODE_BUTTON_R2, KEYCODE_BUTTON_R2),
        new PadButton("PLUS", "Button + (Start)", "Botón + (Start)", KEYCODE_BUTTON_START, KEYCODE_BUTTON_START),
        new PadButton("MINUS", "Button - (Select)", "Botón - (Select)", KEYCODE_BUTTON_SELECT, KEYCODE_BUTTON_SELECT),
        new PadButton("UP", "D-Pad Up", "Cruceta Arriba", KEYCODE_DPAD_UP, KEYCODE_DPAD_UP),
        new PadButton("DOWN", "D-Pad Down", "Cruceta Abajo", KEYCODE_DPAD_DOWN, KEYCODE_DPAD_DOWN),
        new PadButton("LEFT", "D-Pad Left", "Cruceta Izquierda", KEYCODE_DPAD_LEFT, KEYCODE_DPAD_LEFT),
        new PadButton("RIGHT", "D-Pad Right", "Cruceta Derecha", KEYCODE_DPAD_RIGHT, KEYCODE_DPAD_RIGHT),
        new PadButton("LS", "Left Stick Click (L3)", "Pulsar Stick Izquierdo (L3)", KEYCODE_BUTTON_THUMBL, KEYCODE_BUTTON_THUMBL),
        new PadButton("RS", "Right Stick Click (R3)", "Pulsar Stick Derecho (R3)", KEYCODE_BUTTON_THUMBR, KEYCODE_BUTTON_THUMBR),
    };

    public static final int[] COMMON_KEYCODES = {
        KEYCODE_BUTTON_A, KEYCODE_BUTTON_B, KEYCODE_BUTTON_X, KEYCODE_BUTTON_Y,
        KEYCODE_BUTTON_L1, KEYCODE_BUTTON_R1, KEYCODE_BUTTON_L2, KEYCODE_BUTTON_R2,
        KEYCODE_BUTTON_THUMBL, KEYCODE_BUTTON_THUMBR,
        KEYCODE_BUTTON_START, KEYCODE_BUTTON_SELECT,
        KEYCODE_DPAD_UP, KEYCODE_DPAD_DOWN, KEYCODE_DPAD_LEFT, KEYCODE_DPAD_RIGHT,
        KEYCODE_BUTTON_C, KEYCODE_BUTTON_Z, KEYCODE_BUTTON_MODE, KEYCODE_BACK
    };

    public static String keyName(int kc) {
        switch (kc) {
            case KEYCODE_BUTTON_A: return "Button A (Bottom / ✕)";
            case KEYCODE_BUTTON_B: return "Button B (Right / ◯)";
            case KEYCODE_BUTTON_X: return "Button X (Left / □)";
            case KEYCODE_BUTTON_Y: return "Button Y (Top / △)";
            case KEYCODE_BUTTON_L1: return "L1 / LB";
            case KEYCODE_BUTTON_R1: return "R1 / RB";
            case KEYCODE_BUTTON_L2: return "L2 / LT";
            case KEYCODE_BUTTON_R2: return "R2 / RT";
            case KEYCODE_BUTTON_THUMBL: return "Thumb L (L3)";
            case KEYCODE_BUTTON_THUMBR: return "Thumb R (R3)";
            case KEYCODE_BUTTON_START: return "Start / + / Menu";
            case KEYCODE_BUTTON_SELECT: return "Select / - / Back";
            case KEYCODE_DPAD_UP: return "D-Pad Up";
            case KEYCODE_DPAD_DOWN: return "D-Pad Down";
            case KEYCODE_DPAD_LEFT: return "D-Pad Left";
            case KEYCODE_DPAD_RIGHT: return "D-Pad Right";
            case KEYCODE_BUTTON_C: return "Button C";
            case KEYCODE_BUTTON_Z: return "Button Z";
            case KEYCODE_BUTTON_MODE: return "Home / Guide / Mode";
            case KEYCODE_BACK: return "Back";
            default:
                return "KEY [" + kc + "]";
        }
    }

    public static Map<String, Integer> load(String layout, String customMapping) {
        Map<String, Integer> map = new LinkedHashMap<>();
        boolean nintendo = "nintendo".equalsIgnoreCase(layout);
        for (PadButton pb : BUTTONS) {
            map.put(pb.id, nintendo ? pb.defNintendo : pb.defXbox);
        }
        if (customMapping != null && !customMapping.trim().isEmpty()) {
            for (String tok : customMapping.split("[, ]+")) {
                String[] parts = tok.split("[:=]");
                if (parts.length == 2) {
                    try {
                        String id = parts[0].trim().toUpperCase(java.util.Locale.US);
                        int kc = Integer.parseInt(parts[1].trim());
                        map.put(id, kc);
                    } catch (NumberFormatException ignored) { }
                }
            }
        }
        return map;
    }

    public static String serialize(Map<String, Integer> map) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, Integer> e : map.entrySet()) {
            if (sb.length() > 0) sb.append(",");
            sb.append(e.getKey()).append(":").append(e.getValue());
        }
        return sb.toString();
    }
}

