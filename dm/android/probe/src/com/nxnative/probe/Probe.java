// Shows the native probe's report (jni/probe.c) with Share and Copy buttons so it can be sent back.
package com.nxnative.probe;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

public class Probe extends Activity {
    static { System.loadLibrary("probe"); }
    static native String run();

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final String report = run();
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setFitsSystemWindows(true);
        LinearLayout bar = new LinearLayout(this);
        Button share = new Button(this); share.setText("Share report");
        share.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_SEND); i.setType("text/plain");
            i.putExtra(Intent.EXTRA_SUBJECT, "nxnative probe"); i.putExtra(Intent.EXTRA_TEXT, report);
            startActivity(Intent.createChooser(i, "Share report"));
        });
        Button copy = new Button(this); copy.setText("Copy");
        copy.setOnClickListener(v -> {
            ((ClipboardManager) getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("nxnative probe", report));
            Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show();
        });
        bar.addView(share); bar.addView(copy); root.addView(bar);
        TextView t = new TextView(this); t.setText(report); t.setTypeface(Typeface.MONOSPACE); t.setTextSize(11); t.setTextIsSelectable(true); t.setPadding(24, 12, 24, 24);
        ScrollView sv = new ScrollView(this); sv.addView(t); root.addView(sv);
        setContentView(root);
    }
}
