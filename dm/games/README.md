# Game profiles

Each game's constants and shader tables live in `dm/games/<title ID>/`, named after the base game's title ID. This project carries one:
**Pokémon Shield**, `01008DB008C2C000` (update 1.3.2). Tools pick it with `DM_GAME=01008DB008C2C000`; `game.py` reads it and writes the
engine's `game_profile.h`.

| File | What it holds |
| --- | --- |
| `game.tsv` | The profile's keys: the game's modules and builds, its rendering options, the Android operation mode |
| `shader_map.tsv` | Every shader the game uses, by key (a hash of its code), and where the setup finds it in the game's files |
| `shader_facts.tsv`, `shader_facts_manual.tsv`, `shader_pairs.tsv` | What the shader translator needs to know about each shader |
| `pso_manifest.tsv`, `c1_full.tsv`, `bf_want.tsv` | Pipeline and constant-buffer facts the renderer uses |
| `dmsh_chain.h` | The shader chain version the engine expects (`dm/gfx/tools/dmsh_chain.py --header`) |

None of these contain game data: they are hashes, file names and facts measured from the game, from which the setup on the phone finds and
translates the player's own copy.
