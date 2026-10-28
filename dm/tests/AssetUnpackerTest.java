package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class AssetUnpackerTest {
    private static byte[] archive(String... entries) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (int i = 0; i < entries.length; i += 2) {
                zip.putNextEntry(new ZipEntry(entries[i]));
                zip.write(entries[i + 1].getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    public static void main(String[] args) throws Exception {
        File parent = Files.createTempDirectory("asset-unpacker-test").toFile();
        File target = new File(parent, "dm");
        try {
            String installed = AssetUnpacker.revision(25, 1000, "e1cfe274ffa6");
            String updatedSameCode = AssetUnpacker.revision(25, 1001, "e1cfe274ffa6");
            if (installed.equals(updatedSameCode))
                throw new AssertionError("an APK reinstall with the same version code did not invalidate the asset cache");

            AssetUnpacker.unpack(new ByteArrayInputStream(archive("setup.py", "old", "removed.py", "stale")), target, installed);
            if (!"old".equals(read(new File(target, "setup.py")))) throw new AssertionError("initial asset was not unpacked");

            AssetUnpacker.unpack(new ByteArrayInputStream(archive("invalid.zip", "unused")), target, installed);
            if (new File(target, "invalid.zip").exists()) throw new AssertionError("matching cache revision was unnecessarily unpacked");

            AssetUnpacker.unpack(new ByteArrayInputStream(archive("setup.py", "new")), target, updatedSameCode);
            if (!"new".equals(read(new File(target, "setup.py")))) throw new AssertionError("updated asset was not installed");
            if (new File(target, "removed.py").exists()) throw new AssertionError("stale asset survived refresh");

            try {
                AssetUnpacker.unpack(new ByteArrayInputStream(archive("../escape.txt", "bad")), target, "version:26");
                throw new AssertionError("asset path traversal was accepted");
            } catch (IOException expected) {
                if (!"new".equals(read(new File(target, "setup.py"))))
                    throw new AssertionError("failed refresh damaged the installed asset set");
                if (new File(parent, "escape.txt").exists()) throw new AssertionError("asset escaped its directory");
            }
        } finally {
            delete(target);
            delete(parent);
        }
        System.out.println("Asset cache refreshes by revision, removes stale files, and preserves installed files on failure.");
    }

    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }
}
