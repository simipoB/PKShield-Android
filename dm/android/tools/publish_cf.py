#!/usr/bin/env python3
# Publishes an APK to the official download site (site/, a Cloudflare Worker): uploads it to the R2 bucket and puts it first in releases.json, which the
# site's version list and its download function read. The APK must be signed with the release key (BuildSigner.RELEASE_CERT): an APK from anywhere
# else is refused. Needs Node.js (npx wrangler) logged in to the Cloudflare account (npx wrangler login), and apksigner for the key check.
# Usage: publish_cf.py <apk> --version 0.58 [--notes <release page URL>] [--bucket galar-native-apks] [--apksigner <path>] [--dry-run]
import argparse, datetime, hashlib, json, os, re, shutil, subprocess, sys, tempfile

RELEASE_CERT = 'a5f49b44b6181758d82504f3beb0851c92093f454c792b49688728130673a070'   # (BuildSigner.java)


def npx():
    n = shutil.which('npx') or shutil.which('npx.cmd')
    if not n: sys.exit('publish_cf: npx not found (install Node.js)')
    return n


def wrangler(*args, check=True):
    r = subprocess.run([npx(), '--yes', 'wrangler', *args], capture_output=True, text=True)
    if check and r.returncode: sys.exit(f'publish_cf: wrangler {" ".join(args)} failed:\n{r.stdout}{r.stderr}')
    return r


def signer_ok(apk, apksigner):
    tool = apksigner or shutil.which('apksigner') or shutil.which('apksigner.bat')
    if not tool: sys.exit('publish_cf: apksigner not found: pass --apksigner (Android build-tools), the release key is checked before anything is uploaded')
    out = subprocess.run([tool, 'verify', '--print-certs', apk], capture_output=True, text=True)
    if out.returncode: sys.exit(f'publish_cf: the APK does not verify:\n{out.stdout}{out.stderr}')
    digests = re.findall(r'certificate SHA-256 digest: ([0-9a-f]{64})', out.stdout)
    return digests and all(d == RELEASE_CERT for d in digests)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('apk'); ap.add_argument('--version', required=True); ap.add_argument('--notes', default='')
    ap.add_argument('--bucket', default='galar-native-apks'); ap.add_argument('--apksigner'); ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    if not re.fullmatch(r'\d+(\.\d+)+', a.version): sys.exit('publish_cf: --version like 0.58')
    if not os.path.isfile(a.apk): sys.exit(f'publish_cf: {a.apk} not found')
    if not signer_ok(a.apk, a.apksigner): sys.exit('publish_cf: not signed with the release key: refused')
    data = open(a.apk, 'rb').read(); sha = hashlib.sha256(data).hexdigest(); name = f'GalarNative-v{a.version}.apk'
    notes = a.notes or f'https://github.com/simipoB/Galar-Native-Android/releases/tag/v{a.version}'
    entry = {'version': a.version, 'file': name, 'size': len(data), 'sha256': sha, 'date': datetime.date.today().isoformat(), 'notes': notes}

    with tempfile.TemporaryDirectory() as td:
        cur = os.path.join(td, 'releases.json'); lst = []
        if wrangler('r2', 'object', 'get', f'{a.bucket}/releases.json', '--file', cur, '--remote', check=False).returncode == 0 and os.path.isfile(cur):
            try: lst = json.load(open(cur, encoding='utf-8'))
            except ValueError: sys.exit('publish_cf: releases.json in the bucket is not valid JSON: fix it before publishing')
        lst = [entry] + [r for r in lst if r.get('version') != a.version]
        print(f'{name}: {len(data) / 1048576:.1f} MB, SHA-256 {sha}; {len(lst)} version(s) listed')
        if a.dry_run: print(json.dumps(lst, indent=1)); return
        wrangler('r2', 'object', 'put', f'{a.bucket}/{name}', '--file', a.apk, '--content-type', 'application/vnd.android.package-archive', '--remote')
        json.dump(lst, open(cur, 'w', encoding='utf-8'), indent=1)
        wrangler('r2', 'object', 'put', f'{a.bucket}/releases.json', '--file', cur, '--content-type', 'application/json', '--remote')
    print('published: the site lists it within a minute')


if __name__ == '__main__':
    main()
