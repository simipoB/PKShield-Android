# Handoff: Galar Native, Paldea Native and the Arceus port

State as of 2026-11-04, late evening. All testing was on one phone: a Galaxy S25+ (Snapdragon 8 Elite, Adreno 830). No Mali, Xclipse or second device was tested.

**The bar:** a locked 30 fps is the minimum, for every supported phone, in gameplay (menus don't count).

## Taking over: start here

1. **Get the code.** Clone `simipoB/Galar-Native-Android` and check out `violet`: the shared tree for both apps. Add the Paldea repo as a second remote:
   `git remote add paldea https://github.com/simipoB/Paldea-Native-Android.git` (private: ask the owner for access).
2. **Branch rule.** Work on `violet`. Copy each commit to Galar's `main` (`git cherry-pick`), and push `violet` to `paldea main`. Galar's `main` keeps its own `README.md`.
3. **Build.** See [BUILDING.md](BUILDING.md) and "Build and run" below. One engine per game pair: `01008DB008C2C000` for Galar, `01008F6008C5E000` for Paldea.
4. **Game data.** You need your own dumps and `prod.keys`; none are in the repo. Ask the owner for file paths: do not scan their Downloads.
5. **Read "Release status"** before publishing anything. Nothing goes out until it has passed on a phone and the owner says so.
6. **Measure before changing performance code:** `DM_VK_PROF=1`, `DM_VK_GPU_TIME=1`, and always read `/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq` and `cpu6` with every result. A phone charging over USB while warm caps every core at 1.7-2.2 GHz (normal 3.53 / 4.47) even at thermal status 0. Use adb over Wi-Fi (`adb tcpip 5555`, `adb connect <ip>:5555`) and unplug before measuring.
7. **Pick up the open work in the order of "Open problems".**
8. **Update this file** when you finish or rule something out. Write what you measured.

## Release status

Nothing was published tonight. Built, signed with the release key (`a5f49b44…`) and not debuggable:

| App | Version | Package | Notes |
|---|---|---|---|
| Galar Native | v0.41 (versionCode 41) | `com.nxnative.pokemonshield` | Auto 30 fps; split submissions off by default for Sword / Shield; setup and launcher fixes |
| Paldea Native | v0.42 (versionCode 42) | `com.nxnative.pokemonviolet` | First release of this app. 42 because the owner's phone has a versionCode 41 test build |

- The APKs were in the session scratchpad (`rel/`), which is temporary: rebuild with the command below without `--debuggable`.
- **The owner chose to fix Violet's black enrollment photo before releasing Paldea** (problem 1).
- Before publishing, install each release build over the phone's test build and boot all four games. Galar: delete old release pages except the last 2 (keep the tags). Galar v0.40 was published and pulled (white flashes in Sword); the next Galar release must be v0.41 or later.

## Where things are

| | |
|---|---|
| Galar Native (Sword, Shield) | `simipoB/Galar-Native-Android`, `main`. Package `com.nxnative.pokemonshield` |
| Paldea Native (Scarlet, Violet) | `simipoB/Paldea-Native-Android` (private), `main` = branch `violet` here. Package `com.nxnative.pokemonviolet` |
| Arceus port | `simipoB/androidswithy`, branch `arceus-windows`, checkout `C:\Users\daniel\androidswithy`. **Holds the owner's own uncommitted work**: see problem 4 |
| Game profiles | Violet `dm/games/01008F6008C5E000`, Scarlet `0100A3D008C5C000`, Shield `01008DB008C2C000`, Sword `0100ABF008968000`, Arceus `01001F5010DFA000` (androidswithy) |
| Handoff files | [docs/handoff/](handoff/): the uncommitted Arceus changes as patches, and the test scripts used tonight |

Rules for this project:

- No AI co-author lines in commits or PRs.
- Sign releases with `dm/android/probe/out/debug.keystore` from the `PKShield-Android` checkout (cert `a5f49b44…`). It exists only on the owner's PC.
- Do not install over, or change, the owner's `com.nxnative.shield` and `com.nxnative.arceus`. Test Arceus with the separate test app (problem 3).
- Do not scan the owner's Downloads. Ask for paths.
- **Check the foreground app before every adb input.** The owner uses the phone between tests (calls, Reddit, other games); input lands in whatever is on screen.
- The phone has about 4 GB free.

## Build and run

```
cd dm/android && python tools/win_build_lib.py <GAME_ID>
cp out/host-<GAME_ID>-vulkan/lib/libnxnative.so <libs folder>/
cd ../.. && STRICT_16K=1 python dm/android/tools/build_apk.py --game <GAME_ID> --version <n> \
  --libs <libs folder> --pylib <...>/aot/pyandroid/prefix/lib/python3.14 \
  --capstone-py <...>/aot/capsrc/capstone-5.0.7/bindings/python/capstone \
  --ks <keystore> [--debuggable] --out <file>.apk
```

- Use `C:/...` paths for Python and adb from Git Bash with `MSYS_NO_PATHCONV=1`.
- The NSP extractor for Android is built in WSL: `bash dm/android/extract_aot/build_extract_aot.sh`, output `dm/android/extract_aot/out/libnx_extract.so` (put it in the libs folder).
- Built-in code mods: `python dm/tools/builtin_mods.py <decompressed main> dm/host/builtin_mods.h name=<pchtxt>[,<extra build id>] ...` regenerates the table `loader.c` applies.
- Debug switches go in the game's `env.txt` (`Android/data/<package>/files/[<title id>/]env.txt`) and persist between runs. **Clear them after testing.**

## Done today (2026-11-04)

Galar and Paldea (both apps, `violet` and `main`):
- **Setup screen**: a first install opens a three-step setup (game files, keys, set up) in the launcher's style instead of the launcher. Picked files are checked (NSP/XCI only, NSZ refused, NSPs cut short refused, keys told apart by content, files of two different games refused), Clear button, unfinished copies reused, Back returns to the launcher.
- **Setup script** (`setup_game.py`): no longer deletes the other game's DLC; the `ready` file is removed before the game folder changes; free space checked first; plain explanations of unpacker failures; "Set up anyway" (`--allow-damaged`) offered for damaged dumps.
- **NSP extractor** (`dm/tools/nx_extract/Program.cs`): finds NSPs cut short from their PFS0 header; names the damaged file (base or update) when the RomFS header fails its hash; `LinkedListEmpty` counted as damage.
- **Scarlet / Violet built-in mods** (Fl4sh): Frame rate 60 applies Dynamic FPS (ASM), Shadows Sharp applies 2K Shadows; Auto picks 2K shadows on top phones only. Sources in `dm/games/0100A3D008C5C000/builtin_mods/`. Violet's main is the same code at every patched site.
- **60 fps**: a frame-rate guard (`android_main.c graphics_auto`) drops to 720p when the target is missed, back to 1080p after a calm minute. The frame step's cost now uses the game's measured work (it held 30 fps forever before: `hle_nvn.c vstep_tick`). Sword then held 60 fps (16.7 ms) on the S25+.
- **Galar Auto frame rate is 30** again (`GameSettings.AUTO_60 = false`): see problem 2. Sword and Shield no longer use split submissions by default (`GameActivity`: `DM_VK_SPLIT=0` unless `env.txt` sets it), because with them Sword flashed white.
- Diagnostics added: `DM_VK_LOG_ALIAS`, `DM_VK_LOG_COPY` (nvn_vk.c), `DM_NVN_REPORT_INTERVAL=<n>` (hle_nvn.c).

## Open problems

### 1. Violet's enrollment photo was black: fixed

Fixed on 2026-11-04 (confirmed on the owner's phone). The game takes the photo by copying a 1440x864 area of the screen into a **linear** texture (`NVN_TEXTURE_FLAGS_LINEAR`), reads those pixels on the CPU, compresses them (format `0x42`) and uploads them again for the form. The engine kept the copy in the GPU image only, so the CPU compressed zeros. `copy_t2t` now also writes a copy into a linear texture to the texture's memory (`DM_VK_NO_LINEAR_WB=1` turns that off). Found with `DM_VK_LOG_COPY=1`, which logs each distinct copy once with its frame.

### 2. Sword / Shield at 60 fps: the stadium crowd flickers

At 60 fps the crowd in Rose's stadium (the new-game intro) alternates between two pictures every couple of frames. The swsh-recomp project (`raulzrrs/swsh-recomp`, known issue G3) has the same bug, unsolved: "an animation counted in frames".

- The crowd is models `ob0041`-`ob0044` (`stadiumcrowda`-`d`), skinned (bone matrices in `vp_c4`), with a pixel shader that picks one of 16 rows of a texture sheet: `fma(c5[5].y, -0.0625, 0.9688)`. The CPU writes the row index.
- Recordings (`docs/handoff/crowdtest.sh`, `flick2.py`): no flicker at the game's own 30 fps (TV or handheld); flicker with the 60 fps patch whenever the step holds 2 or 3 slots (and in swsh-recomp at a real 60).
- `DM_NVN_REPORT_INTERVAL=2` (the game reads interval 2 back while the window presents at 1) cut the jumps from 79 to 12 frames in 160, not to 0.
- Sword's crowd code: the manager is built at `main+0x969e20` (references the four names); the per-model class constructor is `main+0x9abd10` (fields `+0x4c0`-`+0x508`; `+0x4d0` is a speed/alpha float passed to `0x59c550`, `+0x4d4 = 15`). The row counter was not found yet.
- Until fixed, Auto stays at 30 for Sword / Shield; 60 is a menu choice.

### 3. Arceus: shadowed areas are purple

In the field (the owner's save, the camp), every shadowed area is purple: under tents, crates, NPC shadows.

Ruled out on the phone: POTATO's Low Shadows (shadows change, purple stays), No Shadows (purple gone, because nothing is in shadow), the no-reflections mod, `DM_VK_NO_FMT_SWZ`, `DM_VK_NO_FMT_LIST`, `DM_VK_NO_BC`, and the colour-grading LUT (Galar's layered-3D fix ported: no change).

Found with a frame dump (`DM_NVN_TEXDUMP=2600:...`) and `DM_VK_TRACE_RT=3877`:
- The albedo target (ser 3873) has no purple; the lit HDR target (ser 3877, R11G11B10) has it. So the lighting pass adds it.
- Draws into the lit target: `ha_sky` (65ef28e5…), **`ha_light.bnsh` var72 (73f1192e…) and var77 (fa3842a1…)**, `ha_water`, particles.
- Var77 samples the shadow-map array `fp_t_tcb_28` (`texture(...).x` with a bias), and cube maps `fp_t_tcb_36` (with LOD: reflections) and `fp_t_tcb_34` (LOD 0: diffuse ambient). Shadowed pixels get only ambient, so a wrong ambient input explains purple there only.
- The engine's comment says Arceus's ambient-occlusion pass never runs on this host, and the profile key `tex_unwritten_one` (fragment key:binding pairs sampled as 1) exists for that, **but Arceus's `game.tsv` has never listed any**. A texture nothing writes is read as whatever memory it has. **Leading suspect.**

Next steps:
1. `DM_VK_DRAWLOG=<present>` on the test app logs every texture bound per draw for that frame. Find the textures bound to var72 / var77 that no pass writes (compare with the dumped render targets).
2. Try `DM_VK_TEXOVR=73f1192ee209ab42:<binding>=1,fa3842a1714385d7:<binding>=1` for those bindings. If the purple goes, add the pairs to `tex_unwritten_one` in the Arceus `game.tsv`.

**The Arceus test app** (`com.nxnative.arceut`): the owner's Arceus base APK with the new engine and its package name changed (same length, UTF-16 in the manifest), signed with the release key. It reads the game data from the owner's installed Arceus through `DM_APK_DIR=<that app's /data/app/... folder>` in its `env.txt`. The phone has too little space for a second 4 GB copy. Its save is a copy of the owner's (`save_import`).
- adb-made files in its folder are not readable by the app: `chmod 666 env.txt` (and `chmod -R a+rwX save_import`) after writing them. It is not debuggable, so `run-as` does not work.
- Build: `python dm/android/tools/apk_swap.py a.apk u.apk lib/arm64-v8a/libnxnative.so=<lib> AndroidManifest.xml=<renamed manifest>`, `zipalign -f -p 16384`, `apksigner sign`.
- `docs/handoff/arun.sh <tag> <env lines>` restarts an Arceus app with extra env lines, loads the save and screenshots.

### 4. Arceus: uncommitted work in `androidswithy`

That checkout already had the owner's uncommitted work (touch menu, gestures, `game.tsv` entries and more). Tonight's changes there are **not committed**, so as not to sweep that work in:
- `nvn_vk.c`: the layered 3D render targets and the shared image for a render target read through a second texture object, both from Galar / Paldea ([handoff/arceus-layered-3d-only.patch](handoff/arceus-layered-3d-only.patch) is exactly these).
- **Lower specs** (the POTATO mod, for low-end phones; off by default): a choice in `Settings.java` (Off / On / Lowest, `DM_LOWSPEC=1|2`) and `lowspec()` in `android_main.c`, which writes the three files from `lowspec_files.h` to `<files>/lowspec/romfs` and puts that first in `DM_ROMFS_OVERLAY`. Files: [handoff/arceus-lowspec-android_main.c](handoff/arceus-lowspec-android_main.c), [handoff/arceus-lowspec_files.h](handoff/arceus-lowspec_files.h), the source `.bin` files in [handoff/arceus-lowspec-bins](handoff/arceus-lowspec-bins). Not checked against the game's own files (no Arceus RomFS on the PC); the overlay itself was confirmed working on the phone.
- Ask the owner before committing in that repo.

### 5. Requested, not started

- **Arceus UI**: the owner wants Arceus's settings to match Scarlet / Violet's (the in-game panel `GameMenu.java`, the launcher and setup screens). Arceus has only a simple settings dialog (`Settings.java`, a NativeActivity app with no setup screen).
- **Fill-the-screen picture for Scarlet / Violet**: Fl4sh's 21:9 Ultrawide patch follows a formula. With `r = (screen width / height) / (16/9)`: HUD width `1920/r`, `1080`, offset `(1920 - 1920/r)/2`, FOV scale `r` (21:9 gives 1428.84, 1080, 245.58, 1.34375). A per-phone patch plus `DM_ASPECT=fill` would fill any screen.
- **Measure Scarlet / Violet at 60 fps**, indoors and outdoors, on a cool unplugged phone.

### 6. Other open items

- **Sword's 24-second freeze** loading after Rose's speech (game thread work 24 s, no frames). Not investigated.
- **Sword / Shield YEBIS shaders**: 46 shaders are in no game file (no `.tfxo` reader in `shader_split.py`); they are captured and merged on the next Play.
- **Violet outdoors** never measured: no Violet save past the opening.
- Unimplemented stubs `nvnDeviceGetWindowOriginMode` / `nvnDeviceGetDepthMode`; `no render target could be attached` once per run.
- Not tested: Mali / Xclipse / PanVK phones in this round (PanVK cannot run on the owner's Adreno phone), multiplayer, Bink video, long background play.
- Sword / Shield share no game files (about 13 GB twice).

## Phone state left behind

- Galar Native: debuggable test build (versionCode 30); both games' `env.txt` have `DM_FPS_TARGET=30`, Sword's also `DM_VK_SPLIT=0`. No saves (the test names "Player" are from automated runs).
- Paldea Native: debuggable test build (versionCode 42) with the alias diagnostic. Violet's `env.txt` still has `DM_NVN_TEXDUMP_NOW`, `DM_VK_LOG_VP` and `DM_VK_LOG_ALIAS` for problem 1; remove them when done.
- Arceus test app `com.nxnative.arceut` installed (see problem 3). The owner's `com.nxnative.arceus` is unchanged: its `env.txt` was restored from `/data/local/tmp/arceus_env_backup.txt` and the mod test folders deleted.
- adb is on Wi-Fi (`10.0.0.178:5555`) until the phone restarts.

## Things that wasted time

- **`g_rmu` is held for all of `push_stage`.** Adding locks in functions called from there deadlocks the replay (blue or black screen).
- **Charging hides performance.** The S25+ capped all cores at 1.7-2.2 GHz while charging warm, with thermal status 0. Results taken like that are meaningless.
- **A Python script named `dis.py`** shadows the standard library's `dis` (capstone imports it). Name scripts otherwise.
- **Patch scripts with `\n` in Git Bash heredocs**: the heredoc kept them, but in non-raw Python strings they became newlines in C source. Use raw strings or the file tool.
- A frame dump shows one frame. Confirm which screen it is from.
- In-game taps at A's position type into Sword's name keyboard when it is open.
- The phone's `sed` lacks `\|`; use `grep -v -E`.
- `screenrecord` keeps only frames that change (about 17-20 a second). Single-frame flashes can be missed; compare against both neighbours and against pairs.
