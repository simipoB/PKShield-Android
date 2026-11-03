// Whether this APK is an official Galar Native release (formerly PKShield): signed with the release key (v0.37 on; README "Official downloads").
// A build signed with any other key (a fork, a repackaged APK) still runs, but the setup screen says it is unofficial and env.txt records it, so the game's log
// (android_main.c env_file: "boot: env DM_BUILD_SIGNER=...") tells a support request from an unofficial build at its first lines.
package com.nxnative;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import java.security.MessageDigest;
import java.util.Locale;

final class BuildSigner {
    // SHA-256 of the release certificate (CN=nxnative debug, dm/android/probe/out/debug.keystore): the digest `apksigner verify --print-certs` shows
    static final String RELEASE_CERT = "a5f49b44b6181758d82504f3beb0851c92093f454c792b49688728130673a070";

    static String sha256(byte[] der) {
        try {
            byte[] d = MessageDigest.getInstance("SHA-256").digest(der); StringBuilder s = new StringBuilder(64);
            for (byte x : d) s.append(String.format(Locale.ROOT, "%02x", x & 0xff));
            return s.toString();
        } catch (Exception e) { return ""; }
    }
    // official only when every signer is the release key (an APK with an extra signer is not one of ours either)
    static boolean official(String[] digests) {
        if (digests == null || digests.length == 0) return false;
        for (String d : digests) if (!RELEASE_CERT.equalsIgnoreCase(d)) return false;
        return true;
    }
    // the digests of the installed APK's signers (empty when the package manager does not answer)
    static String[] digests(Context c) {
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            Signature[] sg = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
            if (sg == null) return new String[0];
            String[] out = new String[sg.length]; for (int i = 0; i < sg.length; i++) out[i] = sha256(sg[i].toByteArray());
            return out;
        } catch (Exception e) { return new String[0]; }
    }
    static boolean official(Context c) { return official(digests(c)); }
    // the env.txt value: "official", or "unofficial <first signer's digest>" so the log names the key that signed it
    static String label(String[] digests) {
        if (official(digests)) return "official";
        return "unofficial " + (digests != null && digests.length > 0 ? digests[0] : "unknown");
    }
}
