#!/usr/bin/env python3
import os
import runpy
import struct
import subprocess
import sys
import tempfile
import zlib

PACKER = os.path.abspath(os.path.join(os.path.dirname(__file__), "../android/tools/spv_pak.py"))


def read_pack(path):
    data = open(path, "rb").read()
    version, count = struct.unpack_from("<II", data, 4)
    stride = 44 if version in (3, 4) else 32
    entries = []
    for i in range(count):
        off = 28 + stride * i
        key = data[off:off + 8]
        spv_off, spv_len, vkr_off, vkr_len = struct.unpack_from("<QIQI", data, off + 8)
        row = [key, data[spv_off:spv_off + spv_len], data[vkr_off:vkr_off + vkr_len]]
        if stride == 44:
            gl_off, gl_len = struct.unpack_from("<QI", data, off + 32)
            row.append(data[gl_off:gl_off + gl_len] if gl_len else b"")
        entries.append(row)
    return version, entries


def read_blob(blob, compressed):
    if not compressed:
        return blob
    size = struct.unpack_from("<I", blob)[0]
    result = zlib.decompress(blob[4:])
    assert len(result) == size
    return result


def main():
    with tempfile.TemporaryDirectory() as temp:
        source = os.path.join(temp, "b-testchain")
        os.mkdir(source)
        key = "0102030405060708"
        files = {
            key + ".spv": b"base vertex module",
            key + ".vkr": b"stage vs\n",
            key + ".gl.spv": b"GL depth fallback module",
        }
        for name, value in files.items():
            with open(os.path.join(source, name), "wb") as stream:
                stream.write(value)
        v1 = os.path.join(temp, "shaders-v3.pak")
        subprocess.run([sys.executable, PACKER, source, v1], check=True)
        ver, entries = read_pack(v1)
        assert ver == 3 and len(entries) == 1
        assert entries[0] == [bytes.fromhex(key), files[key + ".spv"], files[key + ".vkr"], files[key + ".gl.spv"]]

        compressed = os.path.join(temp, "shaders-v4.pak")
        subprocess.run([sys.executable, PACKER, "--compress", v1, compressed], check=True)
        ver, entries = read_pack(compressed)
        assert ver == 4 and len(entries) == 1
        assert [read_blob(blob, True) for blob in entries[0][1:]] == [files[key + ".spv"], files[key + ".vkr"], files[key + ".gl.spv"]]

        other_key = "1112131415161718"
        with open(os.path.join(source, other_key + ".spv"), "wb") as stream:
            stream.write(b"captured vertex module")
        with open(os.path.join(source, other_key + ".vkr"), "wb") as stream:
            stream.write(b"stage vs\n")
        with open(os.path.join(source, other_key + ".gl.spv"), "wb") as stream:
            stream.write(b"captured GL depth fallback")
        captured_dir = os.path.join(temp, "b-captured")
        os.mkdir(captured_dir)
        for suffix in (".spv", ".vkr", ".gl.spv"):
            with open(os.path.join(captured_dir, other_key + suffix), "wb") as stream:
                stream.write(open(os.path.join(source, other_key + suffix), "rb").read())
        add_pack = os.path.join(temp, "captured.pak")
        subprocess.run([sys.executable, PACKER, captured_dir, add_pack], check=True)
        runpy.run_path(os.path.abspath(os.path.join(os.path.dirname(__file__), "../android/tools/setup_game.py")),
                       run_name="spv_pak_merge_test")["pak_merge"](compressed, add_pack)
        ver, entries = read_pack(compressed)
        assert ver == 4 and len(entries) == 2, (ver, len(entries))
        assert [read_blob(blob, True) for blob in entries[1][1:]] == [
            b"captured vertex module", b"stage vs\n", b"captured GL depth fallback"]

        base_dir = os.path.join(temp, "b-base")
        os.mkdir(base_dir)
        for suffix, value in ((".spv", b"legacy base vertex"), (".vkr", b"stage vs\n")):
            with open(os.path.join(base_dir, key + suffix), "wb") as stream:
                stream.write(value)
        base_pack = os.path.join(temp, "legacy-compressed.pak")
        subprocess.run([sys.executable, PACKER, base_dir, base_pack], check=True)
        subprocess.run([sys.executable, PACKER, "--compress", base_pack, base_pack + ".z"], check=True)
        os.replace(base_pack + ".z", base_pack)
        runpy.run_path(os.path.abspath(os.path.join(os.path.dirname(__file__), "../android/tools/setup_game.py")),
                       run_name="spv_pak_upgrade_test")["pak_merge"](base_pack, add_pack)
        ver, entries = read_pack(base_pack)
        assert ver == 4 and len(entries) == 2, (ver, len(entries))
        assert [read_blob(blob, True) for blob in entries[0][1:3]] == [b"legacy base vertex", b"stage vs\n"]
        assert entries[0][3] == b""
        assert [read_blob(blob, True) for blob in entries[1][1:]] == [
            b"captured vertex module", b"stage vs\n", b"captured GL depth fallback"]

        os.remove(os.path.join(source, key + ".gl.spv"))
        os.remove(os.path.join(source, other_key + ".gl.spv"))
        v1_legacy = os.path.join(temp, "shaders-v1.pak")
        subprocess.run([sys.executable, PACKER, source, v1_legacy], check=True)
        ver, entries = read_pack(v1_legacy)
        assert ver == 1 and entries[0] == [bytes.fromhex(key), files[key + ".spv"], files[key + ".vkr"]]


if __name__ == "__main__":
    main()
