#!/usr/bin/env python3
import os
import runpy
import subprocess
import sys
import tempfile

BUILDER = os.path.abspath(os.path.join(os.path.dirname(__file__), "../android/tools/build_apk.py"))
BUILD = runpy.run_path(BUILDER, run_name="build_apk_test")
validate_extractor, shader_chain = BUILD["validate_extractor"], BUILD["shader_chain"]
CHAIN_TOOL = os.path.abspath(os.path.join(os.path.dirname(__file__), "../gfx/tools/dmsh_chain.py"))


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

    # the launcher compares the packaged chain with the one setup writes to the ready file: if they differ, every start would ask for setup again
    computed = subprocess.run([sys.executable, CHAIN_TOOL], capture_output=True, text=True, check=True,
                              env=dict(os.environ, DM_GAME="01008DB008C2C000")).stdout.strip()
    assert shader_chain() == computed, f"dmsh_chain.h holds {shader_chain()}, setup makes {computed}: run dmsh_chain.py --header"


if __name__ == "__main__":
    main()
