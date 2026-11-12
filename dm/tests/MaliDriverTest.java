package com.nxnative;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class MaliDriverTest {
    private static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    public static void main(String[] args) throws Exception {
        String zip = "6e9129a3b9db5204c315df2ba5836fb88dea063686a0d11426de992e9e2409c5", lib = "e6e92930cc70f3fae6d7157c5de71598f27b4b1fdfce89504f6c8b6aeda12801";
        check("FristOneRR PanVK 3.7.0".equals(MaliDriver.known(zip, null)), "the release zip is known");
        check("FristOneRR PanVK 3.7.0".equals(MaliDriver.known("00", lib)), "the release library is known when the zip was re-made");
        check("FristOneRR PanVK 3.7.0".equals(MaliDriver.known(zip.toUpperCase(), null)), "digest case does not matter");
        check("PanVK faux123 v1.1.6".equals(MaliDriver.known("f5ce5a1bdb16bc59df836bc72dd34327bbd356b9113de027099ca8f9a49bafea", null)), "faux123's v1.1.6 zip is known");
        check("PanVK faux123 v1.1.6".equals(MaliDriver.known("00", "96419b8e73bdc77a00ccd4449c9874c218441e42e2ba51a93dc8896421178b49")), "faux123's v1.1.6 library is known");
        check(MaliDriver.isPanvk("/data/user/0/x/files/drivers/panvk_faux123_v1.1.6.adpkg/libvulkan_panfrost.so"), "faux123's library is PanVK");
        check(MaliDriver.known("00", "11") == null, "another build is not known");
        check(MaliDriver.known(null, null) == null && MaliDriver.known("", "") == null, "no digests are not known");
        for (String[] k : MaliDriver.KNOWN) check(k[0].matches("[0-9a-f]{64}") && k[1].matches("[0-9a-f]{64}"), "known digests are 64 lowercase hex digits");

        check(MaliDriver.isPanvk("/data/user/0/x/files/drivers/PanVK-FristOneRR-V.3.7.0/libvulkan_panfrost.so"), "PanVK library");
        check(!MaliDriver.isPanvk("/data/user/0/x/files/drivers/turnip/libvulkan_freedreno.so"), "Turnip is not PanVK");
        check(!MaliDriver.isPanvk("") && !MaliDriver.isPanvk(null), "no driver is not PanVK");
        check(MaliDriver.HEAP_MB[0].isEmpty(), "the first heap choice leaves the driver's default");

        File f = Files.createTempFile("mali-driver-test", ".so").toFile();
        try {
            Files.write(f.toPath(), "abc".getBytes(StandardCharsets.US_ASCII));
            check(MaliDriver.sha256(f).equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "file sha256");
        } finally { f.delete(); }
        System.out.println("MaliDriverTest: ok");
    }
}
