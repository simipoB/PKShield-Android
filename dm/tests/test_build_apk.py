#!/usr/bin/env python3
import os
import runpy
import tempfile

BUILDER = os.path.abspath(os.path.join(os.path.dirname(__file__), "../android/tools/build_apk.py"))
BUILD_APK = runpy.run_path(BUILDER, run_name="build_apk_test")
validate_extractor = BUILD_APK["validate_extractor"]
shader_chain = BUILD_APK["shader_chain"]
default_version = BUILD_APK["DEFAULT_VERSION"]
default_min_sdk = BUILD_APK["DEFAULT_MIN_SDK"]


def main():
    assert default_version == 27
    assert default_min_sdk == 30
    assert len(shader_chain()) == 12
    with tempfile.TemporaryDirectory() as temp:
        current = os.path.join(temp, "current.so")
        with open(current, "wb") as stream:
            stream.write(b"\x7fELFcurrent extractor")
        validate_extractor(current)

        stale = os.path.join(temp, "stale.so")
        with open(stale, "wb") as stream:
            stream.write("LibHac cache count was not found".encode("utf-16le"))
        try:
            validate_extractor(stale)
        except SystemExit as error:
            assert "stale libnx_extract.so" in str(error)
        else:
            raise AssertionError("APK builder accepted the known-broken extractor")


if __name__ == "__main__":
    main()
