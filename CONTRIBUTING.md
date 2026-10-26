# Contributing

- **Never commit anything from a game**: no game files, extracted data, rewritten modules (`*.patched.bin`), shaders or shader packs,
  keys, built packages (`.apk`) or signing keys. `.gitignore` covers the usual names; check `git status` before every commit.
- Work in English: code comments, log messages, documentation.
- Red log lines (something unimplemented or wrong) start with `[RED] `.
- Per-game values belong in the game profile (`dm/games/01008DB008C2C000/game.tsv`), not in code. Addresses inside the game's code are
  valid for one build only: gate them on the build ID (see the `touch_menu_build` key and `dm_main_build()`).
- Report device results with the phone model, GPU, Android version, driver, and `log.txt` from `Android/data/com.nxnative.pokemonshield/files`.
