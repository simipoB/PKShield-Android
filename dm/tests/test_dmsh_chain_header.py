#!/usr/bin/env python3
import os
import pathlib
import re
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
CHAIN = REPO / "dm/gfx/tools/dmsh_chain.py"
HEADER = REPO / "dm/games/01008DB008C2C000/dmsh_chain.h"


class ShaderChainHeaderTests(unittest.TestCase):
    def test_profile_header_matches_current_translation_chain(self):
        env = dict(os.environ, DM_GAME="01008DB008C2C000")
        result = subprocess.run(
            [sys.executable, str(CHAIN)],
            env=env,
            capture_output=True,
            check=True,
            text=True,
        )
        match = re.search(r'^#define DMSH_CHAIN "([0-9a-f]{12})"$', HEADER.read_text(), re.M)
        self.assertIsNotNone(match, f"{HEADER} has no generated DMSH_CHAIN definition")
        self.assertEqual(match.group(1), result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
