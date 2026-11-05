package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class SaveImportTest {
    private static byte[] archive(String name, String content) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            zip.putNextEntry(new ZipEntry(name));
            zip.write(content.getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
        }
        return bytes.toByteArray();
    }

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("save-import-test").toFile();
        try {
            File target = new File(root, "save_import");
            if (!target.mkdir()) throw new AssertionError("could not create fixture");
            Files.write(new File(target, "previous.sav").toPath(), new byte[] {1});

            int count = SaveImport.install(new ByteArrayInputStream(archive("profile/data.sav", "edited")), target);
            if (count != 1) throw new AssertionError("wrong imported file count: " + count);
            String saved = new String(Files.readAllBytes(new File(target, "profile/data.sav").toPath()), StandardCharsets.UTF_8);
            if (!"edited".equals(saved)) throw new AssertionError("nested save file was not imported");
            if (new File(target, "previous.sav").exists()) throw new AssertionError("old pending import was not replaced");

            try {
                SaveImport.install(new ByteArrayInputStream(archive("../escape.sav", "bad")), target);
                throw new AssertionError("path traversal archive was accepted");
            } catch (IOException expected) {
                if (!new File(target, "profile/data.sav").isFile())
                    throw new AssertionError("failed import damaged the staged save");
                if (new File(root, "escape.sav").exists())
                    throw new AssertionError("archive wrote outside save storage");
            }
            // a save zipped inside its folder, with macOS's junk: the folder holding main becomes the root, the junk is left out
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
                for (String n : new String[] { "Pokemon Shield/2026-11-05/main", "Pokemon Shield/2026-11-05/backup", "__MACOSX/Pokemon Shield/._main", "Pokemon Shield/.DS_Store" }) {
                    zip.putNextEntry(new ZipEntry(n)); zip.write(n.getBytes(StandardCharsets.UTF_8)); zip.closeEntry(); }
            }
            count = SaveImport.install(new ByteArrayInputStream(bytes.toByteArray()), target);
            if (count != 2 || !new File(target, "main").isFile() || !new File(target, "backup").isFile() || new File(target, "Pokemon Shield").exists())
                throw new AssertionError("a wrapped save was not lifted to the root: " + count + " " + String.join(",", target.list()));
            if (!SaveImport.lastFiles.equals("backup, main")) throw new AssertionError("wrong file list: " + SaveImport.lastFiles);
            // a bare save file (JKSV's main), not a ZIP
            count = SaveImport.install(new ByteArrayInputStream("raw save".getBytes(StandardCharsets.UTF_8)), target, "main");
            if (count != 1 || !"raw save".equals(new String(Files.readAllBytes(new File(target, "main").toPath()), StandardCharsets.UTF_8)))
                throw new AssertionError("a bare save file was not imported");
            try { SaveImport.install(new ByteArrayInputStream("x".getBytes(StandardCharsets.UTF_8)), target, "broken.zip"); throw new AssertionError("a broken .zip was taken as a save file"); }
            catch (IOException expected) { }
        } finally {
            delete(root);
        }
        System.out.println("Save ZIP import preserves nested paths, lifts a save out of its folder, takes a bare save file and rejects traversal without replacing staged data.");
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }
}
