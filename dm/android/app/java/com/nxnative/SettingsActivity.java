package com.nxnative;

import android.os.Bundle;

public final class SettingsActivity extends SetupActivity {
    @Override protected void onCreate(Bundle state) {
        getIntent().putExtra("setup", true);
        super.onCreate(state);
    }
}
