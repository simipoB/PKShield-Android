# PKShield Android

**Pokémon Shield (Nintendo Switch, update 1.3.2) running natively on Android phones.** This is not emulation: the game's own ARM64 code runs
directly on the phone's processor, rewritten only where Android requires it, and its graphics (NVN) are rebuilt on Vulkan.

> [!WARNING]
> **Beta.** Tested on a Samsung Galaxy S25+ (Snapdragon 8 Elite, Adreno 830) with Qualcomm's driver and with Mesa Turnip. Other phones are
> expected to work but have not been tested yet: reports are welcome (see [CONTRIBUTING.md](CONTRIBUTING.md)).

> [!NOTE]
> **No game files, compiled game code, shaders or keys are included, and none will be provided.** You need your own legally obtained dump of
> Pokémon Shield (base game and update 1.3.2) and the `prod.keys` of your own console. Instructions on how to obtain them will not be provided.

## How it works

A Switch game is ARM64 code, and so is a phone. Instead of interpreting or recompiling that code, the engine loads the game's own modules and
runs them: a rewriter (`dm/tools/patch_nso.py`) changes the few instruction patterns Android's kernel and threads do not allow, and a host
runtime (`dm/host`) stands in for the Switch's system services. The game's GPU commands are replayed on Vulkan (`dm/gfx/nvn/nvn_vk.c`), with its
shaders translated to SPIR-V by Ryujinx's shader translator, on the phone, during setup.

## Install and play

1. Install `PokemonShield.apk` (from the releases, or build it yourself: [docs/BUILDING.md](docs/BUILDING.md)) and open it.
2. **Game files → Choose**: your `.nsp` / `.xci` files (base game, update 1.3.2, any DLC). **Keys → Choose**: your `prod.keys` (and
   `title.keys` if your dump needs it). The keys stay in the app's private storage: never uploaded, never part of the APK.
3. **Set up the game.** The phone unpacks the game, rewrites its code and translates its shaders. When it is done the app *is* the game: it
   opens straight into it every time, and your dump files are no longer needed. A Pokémon Shield shortcut with the game's own icon (from your
   dump) goes on the home screen.

Long-press the app's icon for **Settings**: graphics, touch controls, the graphics driver, exporting your save data as a ZIP, or setting the game up again.
A few shaders the game builds while it runs are saved the first time and added at the next start.

## Requirements

- Android 12 or newer, arm64, Vulkan 1.1 or newer with dynamic rendering (most phones from 2020 on; Vulkan 1.3 recommended), about 15 GB free.
- **Adreno** (Snapdragon): tested. Qualcomm's driver or a custom one (Mesa Turnip) chosen in the app.
- **Mali / PowerVR / Xclipse**: supported but untested. Textures these GPUs cannot sample (BC formats) are decoded as they load; on Mali
  the framebuffers are set up to keep its compression (AFBC).

## Playing

- **Touch**: a stick on the left, A B X Y and the camera stick on the right, L / ZL and R / ZR on top, − and + at the top centre, a D-pad.
  Controls fade when untouched; touch anywhere to bring them back. Two fingers tapped together = B.
- **Edit the layout** in game: hold − for 2 seconds, then drag to move, pinch to resize, double-tap to hide or show, hold + to reset,
  hold − again to save.
- **Controllers**: connecting a Bluetooth / USB controller hides the touch controls; a pair of Joy-Cons plays as one controller.
- **Settings**: graphics *High* (TV mode, 1080p), *Low* (handheld mode, 720p, for weaker phones) or *Auto* (High, switching to Low while the
  phone is hot); touch controls opacity and fade time.
- **Logs** for bug reports: `Android/data/com.nxnative.pokemonshield/files/log.txt` (and `log.prev.txt` for the run before).

## Known gaps

- No online features (Y-Comm, link trades, online raids).
- The water in the Wild Area renders darker than on the Switch.

## Building

[docs/BUILDING.md](docs/BUILDING.md): the APK from source on Linux or Windows (WSL) with one command.

## Credits

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** and **nxnative** (MIT): the native-execution engine, the NSO rewriter, the host
  runtime and the NVN layer this project is built from.
- **Ryujinx** (MIT): the shader translator (`dm/third_party/ryujinx`).
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reading NSP / XCI files on the phone.
- **[libadrenotools](https://github.com/bylaws/libadrenotools)** (BSD-2-Clause): custom Adreno drivers. **Mesa Turnip**: the driver it loads.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: the reference for Horizon's synchronization semantics. **yuzu**: Maxwell
  instruction encodings.
- **SPIRV-Cross** (Apache-2.0), **CPython** (PSF), **capstone** (BSD), **LZ4** (BSD), **libvpx** (BSD-3-Clause), **stb_vorbis** (MIT / public
  domain), **Vulkan Headers** (Apache-2.0 / MIT), **DejaVu Sans** (the button icons' letters), Android **native_app_glue** (Apache-2.0).
- **LASRecompAndroid** and **BOTWRecompAndroid**: ideas for Android presentation and CPU scheduling.

Pokémon and Pokémon Shield are trademarks of Nintendo, Creatures Inc. and GAME FREAK inc. This project is not affiliated with or endorsed by
Nintendo, The Pokémon Company, Creatures Inc. or GAME FREAK inc.
