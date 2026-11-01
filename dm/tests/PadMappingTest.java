package com.nxnative;

import java.util.Map;

public final class PadMappingTest {
    public static void main(String[] args) {
        // Test 1: Default Xbox Layout
        Map<String, Integer> defMap = PadMapping.load("default", "");
        if (defMap.get("A") != PadMapping.KEYCODE_BUTTON_B) throw new AssertionError("A should be BUTTON_B in default layout");
        if (defMap.get("B") != PadMapping.KEYCODE_BUTTON_A) throw new AssertionError("B should be BUTTON_A in default layout");
        if (defMap.get("X") != PadMapping.KEYCODE_BUTTON_Y) throw new AssertionError("X should be BUTTON_Y in default layout");
        if (defMap.get("Y") != PadMapping.KEYCODE_BUTTON_X) throw new AssertionError("Y should be BUTTON_X in default layout");
        if (defMap.get("ZL") != PadMapping.KEYCODE_BUTTON_L2) throw new AssertionError("ZL should be BUTTON_L2");
        if (defMap.get("ZR") != PadMapping.KEYCODE_BUTTON_R2) throw new AssertionError("ZR should be BUTTON_R2");
        if (defMap.get("PLUS") != PadMapping.KEYCODE_BUTTON_START) throw new AssertionError("PLUS should be BUTTON_START");
        if (defMap.get("MINUS") != PadMapping.KEYCODE_BUTTON_SELECT) throw new AssertionError("MINUS should be BUTTON_SELECT");
        if (defMap.get("UP") != PadMapping.KEYCODE_DPAD_UP) throw new AssertionError("UP should be DPAD_UP");

        // Test 2: Nintendo Layout
        Map<String, Integer> ninMap = PadMapping.load("nintendo", "");
        if (ninMap.get("A") != PadMapping.KEYCODE_BUTTON_A) throw new AssertionError("A should be BUTTON_A in nintendo layout");
        if (ninMap.get("B") != PadMapping.KEYCODE_BUTTON_B) throw new AssertionError("B should be BUTTON_B in nintendo layout");
        if (ninMap.get("X") != PadMapping.KEYCODE_BUTTON_X) throw new AssertionError("X should be BUTTON_X in nintendo layout");
        if (ninMap.get("Y") != PadMapping.KEYCODE_BUTTON_Y) throw new AssertionError("Y should be BUTTON_Y in nintendo layout");
        if (ninMap.get("L") != PadMapping.KEYCODE_BUTTON_L1) throw new AssertionError("L should be BUTTON_L1 in nintendo layout");

        // Test 3: Custom mapping string
        Map<String, Integer> customMap = PadMapping.load("default", "A:96,B:97,ZL:102,L:104");
        if (customMap.get("A") != 96) throw new AssertionError("Custom A failed");
        if (customMap.get("B") != 97) throw new AssertionError("Custom B failed");
        if (customMap.get("ZL") != 102) throw new AssertionError("Custom ZL failed");
        if (customMap.get("L") != 104) throw new AssertionError("Custom L failed");
        // Others retain default
        if (customMap.get("ZR") != PadMapping.KEYCODE_BUTTON_R2) throw new AssertionError("Custom unmentioned should retain default");

        // Test 4: Serialization round-trip
        String serialized = PadMapping.serialize(customMap);
        Map<String, Integer> reloaded = PadMapping.load("default", serialized);
        if (!reloaded.equals(customMap)) throw new AssertionError("Serialization round-trip mismatch");

        // Test 5: Key names
        if (!PadMapping.keyName(PadMapping.KEYCODE_BUTTON_A).contains("Button A")) throw new AssertionError("keyName A invalid");
        if (!PadMapping.keyName(PadMapping.KEYCODE_BUTTON_L1).contains("L1 / LB")) throw new AssertionError("keyName L1 invalid");

        System.out.println("PadMapping unit tests passed successfully!");
    }
}

