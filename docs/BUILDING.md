# Building PKShield Android

The app (`PokemonShield.apk`) holds no game data: it sets the game up on the phone from the player's own `.nsp` / `.xci` files and
`prod.keys`. Building it needs a PC with **Linux**, or **Windows with WSL** (Ubuntu from the Microsoft Store). No game files or keys are
involved in the build.

## One command

```sh
sudo apt install git curl python3-venv build-essential cmake ninja-build
git clone <this repository> && cd PKShield-Android
bash scripts/build_apk_pc.sh            # → ~/psa-build/PokemonShield.apk
TURNIP=1 bash scripts/build_apk_pc.sh   # the same, plus custom Vulkan drivers (Mesa Turnip / PanVK) on the settings screen
```

On its first run the script fetches what it needs into `~/psa-build` (set `WORK=` for another folder): the Android NDK r27c, the .NET 10 SDK, the
Android build-tools, a JDK when the PC has none, CPython for Android, capstone, LZ4, SPIRV-Cross and, with `TURNIP=1`, libadrenotools. Then it
builds every part from source:

| Part | From | In the app as |
| --- | --- | --- |
| The engine | `dm/host`, `dm/gfx`, `dm/android` (`dm/android/Makefile.host`) | `libnxnative.so` |
| The NSP / XCI unpacker | `dm/tools/nx_extract` (LibHac), .NET NativeAOT (`dm/android/extract_aot`) | `libnx_extract.so` |
| The shader translator | `dm/gfx/tools/dmsh_xlate` (Ryujinx), .NET NativeAOT (`dm/android/xlate_aot`) | `libdmsh_xlate.so` |
| SPIRV-Cross | `dm/android/spirv_cross/build_spirv_cross.sh` | `libspirv_cross.so` |
| Python for the setup scripts | CPython 3.14 for Android, `dm/android/python/pylaunch.c` | `libpython3.14.so`, `libpython_exe.so`, `libssl_python.so`, `libcrypto_python.so`, `python.zip` |
| The setup scripts and the game profile | `dm/android/tools`, `dm/gfx/tools`, `dm/games/01008DB008C2C000` | `dm.zip` |
| Custom drivers (optional) | libadrenotools | `libadrenotools.so` and its hooks |

The APK is signed with a debug key made on first use (`~/psa-build/debug.keystore`). Keep it: an update installs over an earlier build only when
both are signed with the same key.

## Installing and setting the game up

1. Copy `PokemonShield.apk` to the phone and install it (allow installing from unknown sources when asked).
2. Open it. **Game files → Choose**: the base game, update 1.3.2 and any DLC. **Keys → Choose**: `prod.keys` (and `title.keys` if needed).
   Pick the files from the phone's own storage; the app asks for all-files access so it can read the dump where it is (nothing large is copied).
3. **Set up the game**. Progress shows step by step; the log is in `Android/data/com.nxnative.pokemonshield/files/setup_work/setup.log`.
4. From then on the app opens straight into the game. Long-press its icon for **Settings** (graphics, touch controls, name, graphics driver,
   setting the game up again). Uninstalling asks whether to keep the game data.

## Settings by hand

`Android/data/com.nxnative.pokemonshield/files/env.txt` holds `KEY=VALUE` lines the game reads at start (the settings screen writes them):

| Key | Values |
| --- | --- |
| `DM_GRAPHICS` | `high` (TV mode, 1080p), `low` (handheld mode, 720p), `auto` (high, low while the phone is hot) |
| `DM_TOUCH_OPACITY` | 20 to 100 (percent) |
| `DM_TOUCH_FADE` | seconds untouched before the controls fade; 0 = never |
| `DM_VK_DRIVER` | path of a custom Vulkan driver library (set by the settings screen) |
| `DM_VK_NO_BC=1` | decode BC textures on the CPU even where the GPU has them (to try the Mali / PowerVR path) |
| `DM_VK_API=11` / `12` | take the Vulkan 1.1 / 1.2 path on a newer driver (testing) |

## Reporting a problem

Attach `Android/data/com.nxnative.pokemonshield/files/log.txt` (and `log.prev.txt`), the phone model, its GPU and Android version. The
`Vulkan device` line near the top of the log shows the GPU, the driver, and which texture and memory paths were chosen.

If setup fails during extraction, attach `Android/data/com.nxnative.pokemonshield/files/setup_work/setup.log` instead. An error such as
`nx_extract terminated by SIGABRT (-6)` means the extractor process was aborted before Vulkan started; the selected dump paths, their sizes,
and free space are recorded immediately before extraction.

## Rebuilding only the engine

After a change to `dm/host`, `dm/gfx` or `dm/android/app/jni`:

```sh
cd dm/android && make -f Makefile.host GAME=01008DB008C2C000 NDK=~/psa-build/android-ndk-r27c lib
```

On Windows without WSL, `python dm/android/tools/win_build_lib.py 01008DB008C2C000` does the same with the Windows NDK. Then run
`scripts/build_apk_pc.sh` again (finished parts are reused) or `dm/android/tools/build_apk.py` directly.

Changes to the shader translation chain (`dm/gfx/tools/dmsh_*`, the translator, the profile's shader tables) change its version: run
`python dm/gfx/tools/dmsh_chain.py --header` with `DM_GAME=01008DB008C2C000` and rebuild the engine, so the two agree.
