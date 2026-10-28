package com.nxnative;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class DriverPackage {
    private static final long MAX_BYTES = 256L << 20;
    private static final int MAX_ENTRIES = 128;
    private static final Pattern LIBRARY_NAME =
            Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"]+)\"");

    private DriverPackage() { }

    static String install(InputStream source, File target) throws IOException {
        File parent = target.getCanonicalFile().getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
            throw new IOException("could not create driver storage");
        File staging = new File(parent, target.getName() + ".new-" + UUID.randomUUID());
        File backup = new File(parent, target.getName() + ".old-" + UUID.randomUUID());
        if (!staging.mkdir()) throw new IOException("could not prepare driver storage");
        boolean backedUp = false;
        try {
            unpack(source, staging);
            String library = libraryName(staging);
            File libraryFile = new File(staging, library).getCanonicalFile();
            String prefix = staging.getCanonicalPath() + File.separator;
            if (!libraryFile.getPath().startsWith(prefix) || !libraryFile.isFile())
                throw new IOException("driver package does not contain its declared Vulkan library");
            if (target.exists()) {
                if (!target.renameTo(backup)) throw new IOException("could not replace the previous driver");
                backedUp = true;
            }
            if (!staging.renameTo(target)) {
                if (backedUp) backup.renameTo(target);
                backedUp = false;
                throw new IOException("could not install the driver package");
            }
            if (backedUp) deleteTree(backup);
            return library;
        } finally {
            if (staging.exists()) deleteTree(staging);
            if (backedUp && backup.exists() && !target.exists()) backup.renameTo(target);
        }
    }

    private static void unpack(InputStream source, File root) throws IOException {
        String rootPath = root.getCanonicalPath() + File.separator;
        long total = 0;
        int entries = 0;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(source))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAX_ENTRIES) throw new IOException("driver package contains too many files");
                File output = new File(root, entry.getName()).getCanonicalFile();
                if (!output.getPath().startsWith(rootPath))
                    throw new IOException("driver package contains an invalid path");
                if (entry.isDirectory()) {
                    if (!output.isDirectory() && !output.mkdirs())
                        throw new IOException("could not create driver folder");
                } else {
                    File directory = output.getParentFile();
                    if (!directory.isDirectory() && !directory.mkdirs())
                        throw new IOException("could not create driver folder");
                    if (output.exists()) throw new IOException("driver package contains duplicate files");
                    try (OutputStream out = new FileOutputStream(output)) {
                        byte[] buffer = new byte[1 << 16];
                        int count;
                        while ((count = zip.read(buffer)) != -1) {
                            total += count;
                            if (total > MAX_BYTES) throw new IOException("driver package is too large");
                            out.write(buffer, 0, count);
                        }
                    }
                }
                zip.closeEntry();
            }
        }
    }

    private static String libraryName(File root) throws IOException {
        File meta = new File(root, "meta.json");
        if (!meta.isFile()) throw new IOException("driver package is missing meta.json");
        StringBuilder json = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(meta), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) json.append(line);
        }
        Matcher match = LIBRARY_NAME.matcher(json);
        if (!match.find()) throw new IOException("meta.json does not name a Vulkan library");
        String library = match.group(1);
        if (library.isEmpty() || library.indexOf('\\') >= 0 || library.startsWith("/")
                || library.contains("..") || !library.endsWith(".so"))
            throw new IOException("meta.json contains an invalid Vulkan library name");
        return library;
    }

    private static void deleteTree(File file) throws IOException {
        if (Files.isSymbolicLink(file.toPath())) {
            if (!file.delete()) throw new IOException("could not remove driver storage");
            return;
        }
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        if (file.exists() && !file.delete()) throw new IOException("could not remove driver storage");
    }
}
