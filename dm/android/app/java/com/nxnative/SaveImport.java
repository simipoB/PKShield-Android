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

    static int install(InputStream source, File destination) throws IOException {
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
        try (ZipInputStream zip = new ZipInputStream(source)) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAX_ENTRIES) throw new IOException("archive contains too many entries");
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
