#!/usr/bin/env python3
import os, sys, shutil, subprocess, zipfile, glob

# Permitir a zipfile manejar hasta el límite real de especificación ZIP32 (4 GB unsigned)
zipfile.ZIP64_LIMIT = (1 << 32) - 1

GAME_DIR = "/mnt/DATASSD/switchgames/game_processed"
OUT_DIR = "/mnt/DATASSD/switchgames/splits_out"
PACKAGE = "com.nxnative.pokemonshield"
VERSION_CODE = "12"

# 1.8 GB por split: garantiza que ningún split APK supere los 2 GB requeridos por
# zipalign y PackageManager de Android (ZIP estándar de 32 bits, sin ZIP64).
MAX_CHUNK = 1800 * 1024 * 1024

def find_tool(name):
    p = shutil.which(name)
    if p: return p
    candidates = [
        os.path.expanduser("~/psa-build/android-sdk"),
        os.path.expanduser("~/android-sdk"),
        os.path.expanduser("~/Android/Sdk"),
        "/opt/android-sdk"
    ]
    for c in candidates:
        for bt in sorted(glob.glob(os.path.join(c, "build-tools/*")), reverse=True):
            tp = os.path.join(bt, name)
            if os.path.isfile(tp) and os.access(tp, os.X_OK):
                return tp
    return None

def find_jar():
    candidates = [
        os.path.expanduser("~/psa-build/android-sdk"),
        os.path.expanduser("~/android-sdk"),
        os.path.expanduser("~/Android/Sdk"),
        "/opt/android-sdk"
    ]
    for c in candidates:
        for p in sorted(glob.glob(os.path.join(c, "platforms/android-*/android.jar")), reverse=True):
            if os.path.isfile(p):
                return p
    return None

def find_keystore():
    candidates = [
        os.path.expanduser("~/psa-build/debug.keystore"),
        os.path.expanduser("~/.android/debug.keystore"),
        "debug.keystore"
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    ks = os.path.expanduser("~/psa-build/debug.keystore")
    os.makedirs(os.path.dirname(ks), exist_ok=True)
    print(f"[*] Creando debug.keystore en {ks}...")
    subprocess.run([
        "keytool", "-genkeypair", "-keystore", ks,
        "-storepass", "android", "-keypass", "android",
        "-alias", "debug", "-keyalg", "RSA", "-keysize", "2048",
        "-validity", "10000", "-dname", "CN=nxnative debug"
    ], check=True)
    return ks

def ensure_tools():
    aapt2 = find_tool("aapt2")
    zipalign = find_tool("zipalign")
    apksigner = find_tool("apksigner")
    jar = find_jar()

    if not (aapt2 and zipalign and apksigner and jar):
        sdk = os.path.expanduser("~/psa-build/android-sdk")
        print("[!] No se encontraron las herramientas de Android SDK (aapt2, zipalign, apksigner, android.jar).")
        print(f"[*] Descargando e instalando automáticamente en {sdk}...")
        os.makedirs(sdk, exist_ok=True)
        cmdline_zip = "/tmp/cmdline.zip"
        url = "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
        
        subprocess.run(["curl", "-fsSLo", cmdline_zip, url], check=True)
        subprocess.run(["unzip", "-qo", cmdline_zip, "-d", sdk], check=True)
        if os.path.exists(cmdline_zip):
            os.remove(cmdline_zip)
            
        sdkmanager = os.path.join(sdk, "cmdline-tools/bin/sdkmanager")
        if not os.path.exists(sdkmanager):
            sdkmanager = os.path.join(sdk, "cmdline-tools/latest/bin/sdkmanager")
            
        p = subprocess.Popen([sdkmanager, f"--sdk_root={sdk}", "--licenses"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
        p.communicate(input=b"y\ny\ny\ny\ny\ny\ny\ny\ny\ny\n")
        
        print("[*] Instalando build-tools 35.0.0 y platform android-35...")
        subprocess.run([sdkmanager, f"--sdk_root={sdk}", "build-tools;35.0.0", "platforms;android-35"], check=True)
        
        aapt2 = find_tool("aapt2")
        zipalign = find_tool("zipalign")
        apksigner = find_tool("apksigner")
        jar = find_jar()

    return aapt2, zipalign, apksigner, jar

def check_and_compress_shaders():
    pak = os.path.join(GAME_DIR, "shaders.pak")
    if os.path.isfile(pak):
        sz = os.path.getsize(pak)
        # Si shaders.pak sin comprimir supera 1.2 GB, comprimirlo al formato v4 nativo del motor
        if sz > 1200 * 1024 * 1024:
            print(f"[*] shaders.pak sin comprimir mide {sz / (1024**3):.2f} GB.")
            print("[*] Comprimiendo shaders.pak (formato nativo soportado por nvn_vk.c)...")
            here = os.path.dirname(os.path.abspath(__file__))
            spv_pak = os.path.join(here, "dm/android/tools/spv_pak.py")
            tmp_pak = pak + ".compressed"
            subprocess.run([sys.executable, spv_pak, "--compress", pak, tmp_pak], check=True)
            os.replace(tmp_pak, pak)
            new_sz = os.path.getsize(pak)
            print(f"[✓] shaders.pak comprimido exitosamente: {new_sz / (1024**3):.2f} GB")

def align_apk(zipalign, in_apk, out_apk):
    if os.path.exists(out_apk):
        os.remove(out_apk)
    # Intento 1: build-tools moderno con alineación a 16 KB (-f -P 16 4)
    r = subprocess.run([zipalign, "-f", "-P", "16", "4", in_apk, out_apk], capture_output=True, text=True)
    if r.returncode == 0:
        return
    # Intento 2: formato tradicional con página (-f -p 16384)
    r2 = subprocess.run([zipalign, "-f", "-p", "16384", in_apk, out_apk], capture_output=True, text=True)
    if r2.returncode == 0:
        return
    # Intento 3: estándar 4-byte (-f 4)
    subprocess.run([zipalign, "-f", "4", in_apk, out_apk], check=True)

def main():
    if not os.path.isdir(GAME_DIR):
        sys.exit(f"[RED] Directorio de juego no existe: {GAME_DIR}")
        
    aapt2, zipalign, apksigner, jar = ensure_tools()
    keystore = find_keystore()
    
    print(f"[*] aapt2:     {aapt2}")
    print(f"[*] zipalign:  {zipalign}")
    print(f"[*] apksigner: {apksigner}")
    print(f"[*] JAR:       {jar}")
    print(f"[*] Keystore:  {keystore}")
    
    # 0. Asegurar que shaders.pak no exceda el límite individual
    check_and_compress_shaders()
    
    os.makedirs(OUT_DIR, exist_ok=True)
    for old_work in glob.glob(os.path.join(OUT_DIR, "work_*")):
        shutil.rmtree(old_work, ignore_errors=True)
    
    # 1. Agrupar archivos en trozos manteniendo cada split <= 1.8 GB
    print("[*] Escaneando archivos procesados...")
    splits = []
    current_files, current_size = [], 0
    total_size = 0
    for root, _, files in os.walk(GAME_DIR):
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, GAME_DIR).replace("\\", "/")
            sz = os.path.getsize(full)
            if sz > MAX_CHUNK:
                print(f"[!] AVISO: Archivo individual {rel} mide {sz / (1024**3):.2f} GB (mayor a {MAX_CHUNK / (1024**3):.2f} GB)")
            if current_size + sz > MAX_CHUNK and current_files:
                splits.append(current_files)
                current_files, current_size = [], 0
            current_files.append((full, "assets/game/" + rel))
            current_size += sz
            total_size += sz
    if current_files:
        splits.append(current_files)
        
    print(f"[*] Total datos: {total_size / (1024**3):.2f} GB divididos en {len(splits)} split(s) (< 1.8 GB c/u)")
    
    # 2. Generar cada Split APK
    for idx, chunk in enumerate(splits):
        split_name = f"game_{idx}"
        work = os.path.join(OUT_DIR, f"work_{split_name}")
        os.makedirs(work, exist_ok=True)
        
        print(f"\n[+] Creando split {idx + 1}/{len(splits)} ({split_name})...")
        manifest = f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{PACKAGE}" versionCode="{VERSION_CODE}" split="{split_name}">
    <application android:hasCode="false" />
</manifest>'''
        m_path = os.path.join(work, "AndroidManifest.xml")
        with open(m_path, "w", encoding="utf-8") as f:
            f.write(manifest)
            
        raw_apk = os.path.join(work, "raw.apk")
        if os.path.exists(raw_apk):
            os.remove(raw_apk)
            
        subprocess.run([aapt2, "link", "-o", raw_apk, "--manifest", m_path, "-I", jar], check=True)
        
        # Añadir archivos STORED sin compresión
        print("    -> Empaquetando archivos STORED (ZIP 32-bit)...")
        with zipfile.ZipFile(raw_apk, "a", allowZip64=False) as z:
            for full, arc in chunk:
                z.write(full, arcname=arc, compress_type=zipfile.ZIP_STORED)
                
        # Alinear a 16 KB (obligatorio para apkfs y kernels modernos)
        aligned_apk = os.path.join(OUT_DIR, f"split_{split_name}.apk")
        print("    -> Alineando a 16 KB (zipalign)...")
        align_apk(zipalign, raw_apk, aligned_apk)
                
        # Firmar con el keystore
        print("    -> Firmando APK (apksigner)...")
        sign_env = dict(os.environ, TMPDIR=OUT_DIR)
        subprocess.run([
            apksigner, "sign", "--ks", keystore,
            "--ks-pass", "pass:android", "--key-pass", "pass:android",
            aligned_apk
        ], env=sign_env, check=True)
        
        shutil.rmtree(work)
        size_gb = os.path.getsize(aligned_apk) / (1024**3)
        print(f"[✓] Generado exitosamente: {aligned_apk} ({size_gb:.2f} GB)")

    print(f"\n[DONE] Todos los splits generados exitosamente en: {OUT_DIR}")

if __name__ == "__main__":
    main()
