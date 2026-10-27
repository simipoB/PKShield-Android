#!/usr/bin/env python3
import os
import runpy
import tempfile

BUILDER = os.path.abspath(os.path.join(os.path.dirname(__file__), "../android/tools/build_apk.py"))
validate_extractor = runpy.run_path(BUILDER, run_name="build_apk_test")["validate_extractor"]


def main():
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
