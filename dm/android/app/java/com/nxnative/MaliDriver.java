// Mesa PanVK driver packages for Mali (kbase) GPUs, chosen on the setup screen like a Turnip package: which builds are known, and the driver's own switches.
// Known builds are FristOneRR PanVK releases (github.com/FristOneRR/FristOneRR-Panvk-Driver, MIT): the SHA-256 of the release zip and of the library in it, as the
// project publishes them. FristOneRR supports Mali-G57 / G68 / G77 / G78 (Valhall, Job Manager) and Bifrost; Mali-G610 / G615 / G710 / G720 (CSF) are not supported yet.
package com.nxnative;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;

final class MaliDriver {
    // { zip SHA-256, library SHA-256, name }
    static final String[][] KNOWN = {
        { "6e9129a3b9db5204c315df2ba5836fb88dea063686a0d11426de992e9e2409c5", "e6e92930cc70f3fae6d7157c5de71598f27b4b1fdfce89504f6c8b6aeda12801", "FristOneRR PanVK 3.7.0" },
    };
    // the driver's switches the setup screen offers (the ones FristOneRR documents for players; its other variables are for development and can break rendering)
    static final String LOG = "FRR_LOG", HEAP = "PANVK_HEAP_MB", STRIDE = "PANVK_ATOM_STRIDE", STENCIL = "PANVK_STENCIL_LATE";
    static final String[] HEAP_MB = { "", "1024", "2048", "3072", "4096" };   // "" = the driver's own choice (a quarter of the RAM, 1024 to 3072)

    static boolean isPanvk(String library) { return library != null && new File(library).getName().toLowerCase(Locale.ROOT).contains("panfrost"); }

    // the known build a package is, by its zip or (a package re-zipped by hand) its library; null when neither digest is known
    static String known(String zipSha, String libSha) {
        for (String[] k : KNOWN) if (k[0].equalsIgnoreCase(zipSha == null ? "" : zipSha) || k[1].equalsIgnoreCase(libSha == null ? "" : libSha)) return k[2];
        return null;
    }

    static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256"); byte[] b = new byte[1 << 16]; int n;
            while ((n = in.read(b)) > 0) md.update(b, 0, n);
            return hex(md.digest());
        } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
    }
    static String hex(byte[] d) { StringBuilder s = new StringBuilder(d.length * 2); for (byte x : d) s.append(String.format(Locale.ROOT, "%02x", x & 0xff)); return s.toString(); }
}
