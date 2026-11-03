// The app's text in the player's language: the English text in the code is the key, assets/lang/<language>.tsv holds "English TAB translation" lines (\n for a line
// break, %s / %d as in String.format, in the same order). The language: ☰ → Language (DM_LANG), else the phone's; English when there is no table for it or a line is
// missing. dm/android/tools/lang_check.py checks that every T.t text has a line in every table
package com.nxnative;

import android.content.Context;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

final class T {
    private T() { }
    static final String[][] LANGS = { { "", "Auto (the phone's language)" }, { "en", "English" }, { "es", "Español" }, { "pt", "Português" }, { "fr", "Français" }, { "ja", "日本語" } };
    private static String loaded; private static Map<String, String> map = new HashMap<>();

    static String lang(Context c) {
        String l = GameSettings.get(c, "DM_LANG");
        if (l.isEmpty()) l = Locale.getDefault().getLanguage();
        for (String[] k : LANGS) if (!k[0].isEmpty() && k[0].equals(l)) return l;
        return "en";
    }
    static String t(Context c, String en, Object... args) {
        String s = en;
        synchronized (T.class) {
            String l = lang(c);
            if (!l.equals(loaded)) { map = load(c, l); loaded = l; }
            String v = map.get(en); if (v != null) s = v;
        }
        try { return args.length > 0 ? String.format(s, args) : s; } catch (java.util.IllegalFormatException e) { return args.length > 0 ? String.format(en, args) : en; }
    }
    private static Map<String, String> load(Context c, String l) {
        Map<String, String> m = new HashMap<>(); if (l.equals("en")) return m;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("lang/" + l + ".tsv"), StandardCharsets.UTF_8))) {
            String ln;
            while ((ln = r.readLine()) != null) {
                if (ln.isEmpty() || ln.startsWith("#")) continue;
                int tab = ln.indexOf('\t'); if (tab <= 0) continue;
                m.put(ln.substring(0, tab).replace("\\n", "\n"), ln.substring(tab + 1).replace("\\n", "\n"));
            }
        } catch (IOException e) { }
        return m;
    }
}
