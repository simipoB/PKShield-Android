# Galar Native

*(formerly PKShield)*

**Pokémon Sword and Pokémon Shield (Nintendo Switch, update 1.3.2) running natively on Android phones.** This is not emulation: the game's
own ARM64 code runs directly on the phone's processor, rewritten only where Android requires it, and its graphics (NVN) are rebuilt on Vulkan.

> [!WARNING]
> **Beta.** See [Compatibility](#compatibility) for how each kind of phone does. Reports with logs are what moves a phone up that list
> (see [CONTRIBUTING.md](CONTRIBUTING.md)).

> [!NOTE]
> **No game files, compiled game code, shaders or keys are included, and none will be provided.** You need your own legally obtained dump of
> the game (base game and update 1.3.2) and the `prod.keys` of your own console. Instructions on how to obtain them will not be provided.

## One app, both games

**Galar Native** (`GalarNative.apk`, package `com.nxnative.pokemonshield`: it updates the PKShield app in place, keeping
your setup and saves) runs both games with one engine: Sword (`0100ABF008968000`) is the same engine build as Shield (`01008DB008C2C000`).

- **Setup reads your files** and installs whichever game they are: choose a Shield dump and Shield is set up, a Sword dump and Sword is.
  There is one place for each game, so the app holds at most one Shield and one Sword, side by side; setting a game up again replaces it.
- Each game keeps its own setup, saves, settings and mods (Shield in the app's folder as before, Sword in its `0100ABF008968000/` folder).
- With both set up, the app's icon opens a chooser; each game's home screen shortcut starts that game directly.
- Sword shares Shield's shader tables (`tables_from` in `dm/games/0100ABF008968000/game.tsv`), so both make the same shader translation;
  setup maps the shaders of your own dump, Sword's own models included.

## How it works

A Switch game is ARM64 code, and so is a phone. Instead of interpreting or recompiling that code, the engine loads the game's own modules and
runs them: a rewriter (`dm/tools/patch_nso.py`) changes the few instruction patterns Android's kernel and threads do not allow, and a host
runtime (`dm/host`) stands in for the Switch's system services. The game's GPU commands are replayed on Vulkan (`dm/gfx/nvn/nvn_vk.c`), with its
shaders translated to SPIR-V by Ryujinx's shader translator, on the phone, during setup.

## Install and play

1. Install `GalarNative.apk` (from the releases, or build it yourself: [docs/BUILDING.md](docs/BUILDING.md)) and open it.
2. **Game files → Choose**: your `.nsp` / `.xci` files (base game, update 1.3.2, any DLC; a merged XCI works too). **Keys → Choose**: your
   `prod.keys` (and `title.keys` if your dump needs it). The keys stay in the app's private storage: never uploaded, never part of the APK.
3. **Set up the game.** The phone unpacks the game, rewrites its code and translates its shaders. When it is done the app *is* the game: it
   opens straight into it every time, and your dump files are no longer needed (delete them to get the space back). A shortcut with the
   game's own icon (from your dump) goes on the home screen.

### Official downloads

Official APKs come **only** from this repository's [Releases](https://github.com/simipoB/Galar-Native-Android/releases). They never include game
files: an APK that comes with the game already in it is not from this project. Every official release from v0.37 on is signed with this certificate:

```
SHA-256: a5f49b44b6181758d82504f3beb0851c92093f454c792b49688728130673a070
```

Check a download with `apksigner verify --print-certs GalarNative.apk`. A build signed with any other key says **"Unofficial build: not
supported"** on its setup screen, and its log records it (`DM_BUILD_SIGNER=unofficial …`).

A few shaders the game builds while it runs are saved the first time and added at the next start.
If a file picker does not expose a path the app can read directly, the selected game file is copied into the app's storage; this needs free space for both the copy and the setup output.

## Requirements

- Android 12 or newer, arm64, Vulkan 1.1 or newer with dynamic rendering (most phones from 2020 on; Vulkan 1.3 recommended).
- Free space: about 15 GB per game, plus your dump files while setup runs (setup reads them where they are; an XCI's empty padding can be
  trimmed first). 8 GB of memory or more is best; with less, Auto picks half-size textures.

## Compatibility

The game's code runs the same on every arm64 phone; what differs is the GPU and its driver. Every log names the build and the driver it ran,
so a report says exactly what happened.

| GPU (chips) | Driver | Status |
| --- | --- | --- |
| **Adreno 6xx / 7xx / 8xx** (Snapdragon 845 and newer) | Qualcomm's, or **Mesa Turnip** chosen in the app | Plays. Snapdragon 8 Elite (Adreno 830) is the reference phone. Old Qualcomm drivers can crash in places (#44): Turnip fixes that. |
| **Mali Valhall** G57 / G68 / G77 / G78 (Dimensity 700-1200, Helio G99, Exynos 1280 / 1380) | the phone's, or **Mesa PanVK** (FristOneRR) chosen in the app | Starts; the stock driver has rendering gaps (#32). PanVK is experimental: try it when the stock driver draws wrong. |
| **Mali Bifrost** G52 / G72 / G76 (Helio G85-G96, older Exynos) | the phone's, or PanVK | Weak GPUs: Low graphics and half textures. PanVK has a start-up fix for G52. |
| **Mali CSF** G610 / G615 / G710 / G720 / G925 Immortalis (Dimensity 7300 / 8000 / 9000 series) | the phone's only (PanVK does not support them yet) | Fixed in v0.40 for the white screen (#40, #47); waiting on reports. |
| **Xclipse** 540 / 940 (Exynos 1580 / 2400) | the phone's | Fixed in v0.40 for the crashes (#41, #45); waiting on reports. |
| **PowerVR** (Google Tensor G5 / G6) | the phone's | Untested. |

Textures a GPU cannot read natively (the Switch's BC formats on Mali, Xclipse and PowerVR) are decoded as they load; on Mali the
framebuffers keep its compression (AFBC). The driver can be changed any time from the app's icon (long-press → Settings).

## Playing

- **Touch**: a stick on the left, A B X Y and the camera stick on the right, L / ZL and R / ZR on top, − and + at the top centre, a D-pad,
  and ☰ at the top left for the settings. Controls fade when untouched; touch anywhere to bring them back. Two fingers tapped together = B.
- **Controllers**: connecting a Bluetooth / USB controller hides the touch controls; a pair of Joy-Cons plays as one controller.
  **Hold − for 3 seconds** to remap the controller's buttons.
- **Edit the touch layout**: ☰ → *Move the touch controls* (or hold the on-screen − for 2 seconds), then drag to move, pinch to resize,
  double-tap to hide or show, hold + to reset, hold − to save.
- **Logs** for bug reports (the app's log says which build and driver it ran): `Download/PokemonShield-log.txt` or `Download/PokemonSword-log.txt` (and `-log-previous.txt` for the run before).

## Settings (in game: ☰)

| Setting | Choices | Applies |
| --- | --- | --- |
| Picture | 16:9 as made, fill the screen (stretched), 20:9 for a widescreen mod | at once |
| Upscaling | Sharp (ours), SGSR (Qualcomm's), Smooth (a plain stretch) | at once |
| Graphics | **Auto**, High (TV mode, 1080p), Low (handheld mode, 720p), Adaptive (High, Low while the phone is hot) | next start |
| Textures | **Auto**, Full, Half (large textures at half size: less memory and GPU work) | next start |
| Frame rate (Sword & Shield) | Auto, 30 fps, 60 fps (experimental) | next start |
| Controller buttons | by position (as on a Switch pad) or by label; remap any button | at once |
| Touch controls | opacity, fade time, layout | at once |

**Auto** looks at the phone's chip and its memory: the chip sets the starting point (Snapdragon 8 / Dimensity 9000 / Exynos 2000 full,
Snapdragon 6 and lower-end MediaTek / Exynos lighter), then memory adjusts it for every phone: under 8 GB the textures go to half size
whatever the chip, under 6 GB graphics go to Low, and 12 GB or more keeps full textures unless the chip is a weak one. The menu shows what
Auto picked for your phone. Long-pressing the app's icon still offers the graphics driver (including Mesa PanVK packages for Mali G57 / G68 / G77 / G78 and Bifrost,
with their options) and setting a game up again.

## Mods and save data (in game: ☰)

- **Import a mod (.zip)**: the zip may hold `romfs/` and `exefs/` at its top, inside one folder, or in the Atmosphère layout
  (`atmosphere/contents/<title ID>/romfs`); loose `.pchtxt` files are taken as code patches. Mods go to
  `Android/data/<package>/files/mods/<mod name>/` (`romfs/` files replace the game's, `exefs/*.pchtxt` patch its code; patches must be made
  for update 1.3.2 of your game, their build ID is checked). **Mods** in the menu lists them and switches each on or off; mods apply from the
  next start. Several mods replacing the same file: the first by name wins.
- **Export save data** writes your save as a ZIP; **Import save data** takes one back (an exported or edited save) and loads it at the next
  start, keeping your current save as a backup. Save in the game before exporting.

## Known gaps

- No online features (Y-Comm, link trades, online raids).
- The water in the Wild Area renders darker than on the Switch.

## Building

[docs/BUILDING.md](docs/BUILDING.md): the APK from source on Linux or Windows (WSL) with one command. The engine is built for the Shield
profile (`win_build_lib.py 01008DB008C2C000` / `make GAME=01008DB008C2C000`), which runs both games; `build_apk.py` makes the Sword & Shield app.

## Credits

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** and **nxnative** (MIT): the native-execution engine, the NSO rewriter, the host
  runtime and the NVN layer this project is built from.
- **Ryujinx** (MIT): the shader translator (`dm/third_party/ryujinx`).
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reading NSP / XCI files on the phone.
- **[libadrenotools](https://github.com/bylaws/libadrenotools)** (BSD-2-Clause): custom Adreno drivers. **Mesa Turnip**: the driver it loads.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: the reference for Horizon's synchronization semantics. **yuzu**: Maxwell
  instruction encodings.
- **[Snapdragon Game Super Resolution](https://github.com/SnapdragonStudios/snapdragon-gsr)** (Qualcomm, BSD-3-Clause): the SGSR upscaler (`dm/third_party/sgsr`).
- **SPIRV-Cross** (Apache-2.0), **CPython** (PSF), **capstone** (BSD), **LZ4** (BSD), **libvpx** (BSD-3-Clause), **stb_vorbis** (MIT / public
  domain), **Vulkan Headers** (Apache-2.0 / MIT), **DejaVu Sans** (the button icons' letters), Android **native_app_glue** (Apache-2.0).
- **LASRecompAndroid** and **BOTWRecompAndroid**: ideas for Android presentation and CPU scheduling.

Pokémon, Pokémon Sword and Pokémon Shield are trademarks of Nintendo, Creatures Inc. and GAME FREAK inc. This project is not affiliated with
or endorsed by Nintendo, The Pokémon Company, Creatures Inc. or GAME FREAK inc.
