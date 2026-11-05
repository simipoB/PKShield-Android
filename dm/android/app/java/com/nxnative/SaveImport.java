package com.nxnative;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class SaveImport {
    private static final long MAX_BYTES = 256L << 20;
    private static final int MAX_ENTRIES = 10000;

    private SaveImport() { }

    static int install(InputStream source, File destination) throws IOException { return install(source, destination, null); }
    // The files the last install put in place (for the message: a player sees at once whether it is the save they meant)
    static String lastFiles = "";
    // A save ZIP from anywhere: ours (files at the root), a folder zipped as a whole (Checkpoint backups, Windows' and macOS's "compress", a save copied out of
    //   another emulator's folders), or a bare save file (JKSV's main). The games open save:/main, so the folder that holds a file named main becomes the save's
    //   root (Reddit: "3 files were loaded" and the game started anew: they sat one folder down); without one, the layout stays as it came.
    //   macOS's __MACOSX/ and .DS_Store, Windows' Thumbs.db / desktop.ini are left out. name: the picked file's name (a bare save file keeps it)
    static int install(InputStream source, File destination, String name) throws IOException {
        java.io.BufferedInputStream in = new java.io.BufferedInputStream(source, 1 << 16); in.mark(8); byte[] mg = new byte[4]; int got = 0, r;
        while (got < 4 && (r = in.read(mg, got, 4 - got)) > 0) got += r;
        in.reset(); source = in;
        boolean isZip = got == 4 && mg[0] == 'P' && mg[1] == 'K' && (mg[2] == 3 || mg[2] == 5) && (mg[3] == 4 || mg[3] == 6);
        if (Files.isSymbolicLink(destination.toPath()))
            throw new IOException("save import destination is a symbolic link");
        File target = destination.getCanonicalFile();
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
            throw new IOException("could not create private save storage");
        File staging = new File(parent, target.getName() + ".pending");
        File backup = new File(parent, target.getName() + ".previous");
        deleteTree(staging);
        if (!staging.mkdir()) throw new IOException("could not prepare temporary save storage");

        int entries = 0;
        int files = 0;
        long total = 0;
        if (!isZip) {   // a bare save file
            String n = name == null ? "" : new File(name).getName().trim();
            if (n.isEmpty() || n.startsWith(".") || n.indexOf(':') >= 0 || n.toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) { deleteTree(staging); throw new IOException("the file is not a ZIP archive or a save file"); }
            try (OutputStream out = new FileOutputStream(new File(staging, n))) {
                byte[] buffer = new byte[1 << 16]; int count;
                while ((count = source.read(buffer)) != -1) { total += count; if (total > MAX_BYTES) throw new IOException("save file is larger than 256 MB"); out.write(buffer, 0, count); }
            } catch (IOException | RuntimeException e) { deleteTree(staging); throw e; }
            if (total == 0) { deleteTree(staging); throw new IOException("the save file is empty"); }
            files = 1;
        } else
        try (ZipInputStream zip = new ZipInputStream(source)) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAX_ENTRIES) throw new IOException("archive contains too many entries");
                String en = entry.getName(), base = en.replaceAll("/+$", ""); base = base.substring(base.lastIndexOf('/') + 1);
                if (en.startsWith("__MACOSX/") || base.equals(".DS_Store") || base.equalsIgnoreCase("Thumbs.db") || base.equalsIgnoreCase("desktop.ini")) { zip.closeEntry(); continue; }
                File output = entryFile(staging, entry);
                if (entry.isDirectory()) {
                    if (!output.isDirectory() && !output.mkdirs())
                        throw new IOException("could not create save folder " + entry.getName());
                    zip.closeEntry();
                    continue;
                }
                File directory = output.getParentFile();
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IOException("could not create save folder " + entry.getName());
                if (output.exists()) throw new IOException("archive contains duplicate path " + entry.getName());
                try (OutputStream out = new FileOutputStream(output)) {
                    byte[] buffer = new byte[1 << 16];
                    int count;
                    while ((count = zip.read(buffer)) != -1) {
                        total += count;
                        if (total > MAX_BYTES) throw new IOException("save archive expands beyond 256 MB");
                        out.write(buffer, 0, count);
                    }
                }
                files++;
                zip.closeEntry();
            }
            if (files == 0) throw new IOException("archive contains no save files");
        } catch (IOException | RuntimeException e) {
            deleteTree(staging);
            throw e;
        }
        File root = saveRoot(staging);
        if (!root.equals(staging)) {   // the save's own folder moved up to the root, the rest left out
            File lifted = new File(parent, target.getName() + ".lift");
            deleteTree(lifted);
            if (!root.renameTo(lifted)) { deleteTree(staging); throw new IOException("could not arrange the imported save"); }
            deleteTree(staging);
            if (!lifted.renameTo(staging)) { deleteTree(lifted); throw new IOException("could not arrange the imported save"); }
            files = count(staging);
        }
        { String[] names = staging.list(); java.util.Arrays.sort(names); lastFiles = String.join(", ", names); }

        deleteTree(backup);
        boolean hadPrevious = target.exists();
        if (hadPrevious && !target.renameTo(backup)) {
            deleteTree(staging);
            throw new IOException("could not preserve the pending save import");
        }
        if (!staging.renameTo(target)) {
            if (hadPrevious) backup.renameTo(target);
            deleteTree(staging);
            throw new IOException("could not install the imported save archive");
        }
        deleteTree(backup);
        return files;
    }

    // the folder that is the save: the shallowest one holding a file named main, else the archive's own root
    static File saveRoot(File top) {
        java.util.ArrayDeque<File> q = new java.util.ArrayDeque<>(); q.add(top);
        while (!q.isEmpty()) {
            File d = q.poll(); if (new File(d, "main").isFile()) return d;
            File[] ch = d.listFiles(); if (ch != null) { java.util.Arrays.sort(ch); for (File c : ch) if (c.isDirectory() && !Files.isSymbolicLink(c.toPath())) q.add(c); }
        }
        return top;   // (no main anywhere: the layout as it came, nested folders and all)
    }
    private static int count(File d) { File[] ch = d.listFiles(); int n = 0; if (ch != null) for (File c : ch) n += c.isDirectory() ? count(c) : 1; return n; }
    private static File entryFile(File root, ZipEntry entry) throws IOException {
        String name = entry.getName();
        if (name == null || name.isEmpty() || name.startsWith("/") || name.indexOf('\\') >= 0)
            throw new IOException("archive contains an invalid path");
        String[] parts = name.split("/", -1);
        int limit = entry.isDirectory() && parts[parts.length - 1].isEmpty() ? parts.length - 1 : parts.length;
        if (limit == 0) throw new IOException("archive contains an invalid path");
        File output = root;
        for (int i = 0; i < limit; i++) {
            String part = parts[i];
            if (part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf(':') >= 0)
                throw new IOException("archive contains an invalid path");
            output = new File(output, part);
        }
        File canonical = output.getCanonicalFile();
        String prefix = root.getCanonicalPath() + File.separator;
        if (!canonical.getPath().startsWith(prefix))
            throw new IOException("archive path escapes save storage");
        return canonical;
    }

    private static void deleteTree(File file) throws IOException {
        if (Files.isSymbolicLink(file.toPath())) {
            if (!file.delete()) throw new IOException("could not clean temporary save storage");
            return;
        }
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        if (file.exists() && !file.delete()) throw new IOException("could not clean temporary save storage");
    }
}
