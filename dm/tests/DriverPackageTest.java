package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class DriverPackageTest {
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
        File parent = Files.createTempDirectory("driver-package-test").toFile();
        try {
            File target = new File(parent, "panvk");
            String meta = "{\"libraryName\":\"libvulkan_panfrost.so\"}";
            String library = "not-a-real-library";
            String selected = DriverPackage.install(new ByteArrayInputStream(
                    archive("meta.json", meta, "libvulkan_panfrost.so", library)), target);
            if (!"libvulkan_panfrost.so".equals(selected))
                throw new AssertionError("wrong driver library selected");
            if (!new File(target, selected).isFile())
                throw new AssertionError("driver library was not installed");

            try {
                DriverPackage.install(new ByteArrayInputStream(archive(
                        "meta.json", "{\"libraryName\":\"../escape.so\"}",
                        "escape.so", library)), new File(parent, "bad"));
                throw new AssertionError("path traversal library was accepted");
            } catch (IOException expected) {
                if (new File(parent, "escape.so").exists())
                    throw new AssertionError("driver package escaped its directory");
            }
        } finally {
            delete(parent);
        }
        System.out.println("PanVK-style packages install atomically and reject unsafe library paths.");
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }
}
