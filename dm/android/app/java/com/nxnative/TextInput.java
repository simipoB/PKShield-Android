// The game's software keyboard (the name entry): an Android dialog with a text box and the phone's own keyboard. Called from the native front end
// (app/jni/android_main.c front_text_input), which waits on state(): 0 open, 1 OK (text() holds the text), 2 cancelled.
package com.nxnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.text.InputFilter;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.TextView;
import android.widget.EditText;

public final class TextInput {
    private static volatile int state;
    private static volatile String text;

    public static void show(final Activity a, final String title, final String sub, final String guide, final String initial, final int max, final boolean numeric) {
        state = 0;
        text = null;
        a.runOnUiThread(new Runnable() {
            public void run() {
                final EditText e = new EditText(a);
                e.setSingleLine(true);
                if (numeric) e.setInputType(InputType.TYPE_CLASS_NUMBER);   // the game asks for digits (a Link Code, a quantity): the number pad
                e.setText(initial);
                e.setSelection(e.getText().length());
                if (guide != null && guide.length() > 0) e.setHint(guide);
                if (max > 0) e.setFilters(new InputFilter[] { new InputFilter.LengthFilter(max) });
                // a compact keyboard (landscape would cover the dialog with a full-screen editor), and its Done key confirms like the dialog's OK
                e.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
                AlertDialog.Builder b = new AlertDialog.Builder(a).setTitle(title != null && title.length() > 0 ? title : "Enter text").setView(e).setCancelable(true);
                if (sub != null && sub.length() > 0) b.setMessage(sub);
                b.setPositiveButton("OK", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) { text = e.getText().toString(); state = text.length() > 0 ? 1 : 2; }
                });
                b.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) { state = 2; }
                });
                b.setOnCancelListener(new DialogInterface.OnCancelListener() {
                    public void onCancel(DialogInterface d) { if (state == 0) state = 2; }
                });
                final AlertDialog d = b.create();
                e.setOnEditorActionListener(new TextView.OnEditorActionListener() {
                    public boolean onEditorAction(TextView v, int id, KeyEvent ev) {
                        if (id == EditorInfo.IME_ACTION_DONE || (ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER && ev.getAction() == KeyEvent.ACTION_DOWN)) {
                            text = e.getText().toString(); state = text.length() > 0 ? 1 : 2; d.dismiss(); return true;
                        }
                        return false;
                    }
                });
                d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
                d.show();
                e.requestFocus();
            }
        });
    }
    public static int state() { return state; }
    public static String text() { return text; }
}
