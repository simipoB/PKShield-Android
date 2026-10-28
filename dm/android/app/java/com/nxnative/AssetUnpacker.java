package com.nxnative;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.BufferedOutputStream;
import java.io.FileOutputStream;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class AssetUnpacker {
    private AssetUnpacker() { }

    static String revision(long versionCode, long lastUpdateTime, String shaderChain) {
        return versionCode + ":" + lastUpdateTime + ":" + shaderChain;
    }

    static File unpack(InputStream source, File target, String revision) throws IOException {
        if (isCurrent(target, revision)) return target;
        File parent = target.getAbsoluteFile().getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("could not create " + parent);
        File staging = new File(parent, target.getName() + ".unpack-" + UUID.randomUUID());
        File backup = new File(parent, target.getName() + ".old-" + UUID.randomUUID());
        if (!staging.mkdir()) throw new IOException("could not create " + staging);
        boolean backedUp = false;
        try {
            unzip(source, staging);
            try (FileWriter stamp = new FileWriter(new File(staging, ".unpacked"))) {
                stamp.write(revision);
                stamp.write("\n");
            }
            if (target.exists()) {
                if (!target.renameTo(backup)) throw new IOException("could not move old asset directory " + target);
                backedUp = true;
            }
            if (!staging.renameTo(target)) {
                if (backedUp && !backup.renameTo(target))
                    throw new IOException("could not install refreshed assets or restore " + backup);
                backedUp = false;
                throw new IOException("could not install refreshed assets at " + target);
            }
            if (backedUp) deleteTree(backup);
            return target;
        } finally {
            if (staging.exists()) deleteTree(staging);
            if (backedUp && backup.exists() && !target.exists()) backup.renameTo(target);
        }
    }

    private static boolean isCurrent(File target, String revision) {
        File stamp = new File(target, ".unpacked");
        try (BufferedReader reader = new BufferedReader(new FileReader(stamp))) {
            return revision.equals(reader.readLine());
        } catch (IOException e) {
            return false;
        }
    }

    private static void unzip(InputStream source, File root) throws IOException {
        String rootPath = root.getCanonicalPath() + File.separator;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(source))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                File output = new File(root, entry.getName());
                String path = output.getCanonicalPath();
                if (!path.startsWith(rootPath)) throw new IOException("asset contains an invalid path: " + entry.getName());
                if (entry.isDirectory()) {
                    if (!output.isDirectory() && !output.mkdirs()) throw new IOException("could not create " + output);
                } else {
                    File directory = output.getParentFile();
                    if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("could not create " + directory);
                    try (OutputStream stream = new BufferedOutputStream(new FileOutputStream(output))) {
                        byte[] buffer = new byte[65536];
                        int count;
                        while ((count = zip.read(buffer)) != -1) stream.write(buffer, 0, count);
                    }
                }
                zip.closeEntry();
            }
        }
    }

    private static void deleteTree(File file) throws IOException {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        if (file.exists() && !file.delete()) throw new IOException("could not remove " + file);
    }
}
