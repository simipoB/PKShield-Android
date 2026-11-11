// The probe's screen: runs the pass replay (android.app.NativeActivity in its own process, so a crash in the graphics driver does not close this screen)
// once per variant, one after another, then shows what happened and shares it. No game data, no permissions, nothing sent anywhere unless the player shares it.
package com.nxnative.passprobe;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    static final int[] VARIANTS = { 0, 1, 2 };
    static final String[] NAMES = { "the game's passes, clears and copies", "+ the present (clear, blit)", "+ the touch overlay" };
    TextView text; Button run, share; Handler h = new Handler(Looper.getMainLooper());

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout l = new LinearLayout(this); l.setOrientation(LinearLayout.VERTICAL); int p = (int) (16 * getResources().getDisplayMetrics().density); l.setPadding(p, p, p, p);
        TextView t = new TextView(this); t.setText("Galar Native: Mali test 2\nNo game files needed. Tap Run test; it takes under a minute and may close by itself once or twice (that is the test). Then tap Share result and post it on GitHub."); t.setTextSize(16);
        run = new Button(this); run.setText("Run test"); run.setOnClickListener(v -> { for (int x : VARIANTS) new File(getFilesDir(), "run-" + x + ".txt").delete(); next(); });
        share = new Button(this); share.setText("Share result"); share.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_SEND); i.setType("text/plain"); i.putExtra(Intent.EXTRA_TEXT, report(true)); startActivity(Intent.createChooser(i, "Share the result")); });
        text = new TextView(this); text.setTypeface(Typeface.MONOSPACE); text.setTextSize(12); text.setTextIsSelectable(true);
        ScrollView sv = new ScrollView(this); sv.addView(text);
        l.addView(t); l.addView(run); l.addView(share); l.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(l);
    }
    @Override protected void onResume() {
        super.onResume(); text.setText(report(false));
        if (new File(getFilesDir(), "running").exists()) h.postDelayed(this::next, 800);   // a run just ended (or crashed): the next variant
    }
    // the first variant with no result yet, or the end of the test
    void next() {
        for (int x : VARIANTS) {
            File f = new File(getFilesDir(), "run-" + x + ".txt");
            if (!f.exists()) {
                try (FileWriter w = new FileWriter(new File(getFilesDir(), "variant"))) { w.write(Integer.toString(x)); } catch (IOException e) { }
                try (FileWriter w = new FileWriter(new File(getFilesDir(), "running"))) { w.write("1"); } catch (IOException e) { }
                Intent i = new Intent(); i.setClassName(this, "android.app.NativeActivity"); startActivity(i); return;
            }
        }
        new File(getFilesDir(), "running").delete(); text.setText(report(false));
    }
    static List<String> lines(File f) {
        List<String> r = new ArrayList<>(); if (!f.exists()) return r;
        try (BufferedReader br = new BufferedReader(new FileReader(f))) { String s; while ((s = br.readLine()) != null) r.add(s); } catch (IOException e) { }
        return r;
    }
    String report(boolean full) {
        StringBuilder sb = new StringBuilder();
        sb.append("Galar Native Mali test 2\nPhone: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.HARDWARE).append("), Android ").append(Build.VERSION.RELEASE).append('\n');
        boolean any = false;
        for (int k = 0; k < VARIANTS.length; k++) {
            List<String> L = lines(new File(getFilesDir(), "run-" + VARIANTS[k] + ".txt")); if (L.isEmpty()) continue; any = true;
            String gpu = "", last = "", done = "", fail = ""; int steps = 0;
            for (String s : L) { if (s.startsWith("GPU ")) gpu = s; if (s.startsWith("round") && s.contains(" step ")) { last = s; steps++; } if (s.startsWith("DONE")) done = s; if (s.startsWith("FAIL") || s.contains("failed")) fail = s; }
            if (k == 0 || sb.indexOf(gpu) < 0) sb.append(gpu).append('\n');
            sb.append("\nTest ").append(k + 1).append(" (").append(NAMES[k]).append("): ");
            if (!done.isEmpty()) sb.append("PASSED, ").append(steps).append(" steps\n");
            else if (!fail.isEmpty()) sb.append("STOPPED: ").append(fail).append('\n');
            else if (L.get(L.size() - 1).equals("END")) sb.append("ended early\n");
            else sb.append("CRASHED after ").append(steps).append(" steps. Last one:\n  ").append(last).append('\n');
            if (full && done.isEmpty()) { int from = Math.max(0, L.size() - 6); for (int i = from; i < L.size(); i++) sb.append("  > ").append(L.get(i)).append('\n'); }
        }
        if (!any) sb.append("\nNot run yet.");
        return sb.toString();
    }
}
