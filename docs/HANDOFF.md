# Handoff: Galar Native and Paldea Native

State as of 2026-11-03. All testing below was on one phone: a Galaxy S25+ (Adreno 830, Android with 4 KB pages). No Mali, Xclipse or second device was tested in this round.

**The bar:** a locked 30 fps is the minimum, for every supported phone. It is not met on Scarlet and Violet yet.

## Where things are

| | |
|---|---|
| Galar Native (Sword, Shield) | `simipoB/Galar-Native-Android`, branch `main`. Package `com.nxnative.pokemonshield` |
| Paldea Native (Scarlet, Violet) | `simipoB/Paldea-Native-Android` (private), branch `main`, the same code as branch `violet` of the Galar repo. Package `com.nxnative.pokemonviolet` |
| Shared code | Both apps are the same tree. Develop on `violet`, then merge to Galar's `main` keeping Galar's own `README.md` |
| Game profiles | Violet `dm/games/01008F6008C5E000`, Scarlet `0100A3D008C5C000`, Shield `01008DB008C2C000`, Sword `0100ABF008968000` |

Rules for this project:

- No AI co-author lines in commits or PRs.
- Sign releases with `dm/android/probe/out/debug.keystore` from the `PKShield-Android` checkout (cert `a5f49b44…`).
- Publish no release until it has passed on a phone. The user decides when.
- Do not install over, or change, the user's own `com.nxnative.shield` and `com.nxnative.arceus`. The first keeps its game data inside its APK splits, so its nearly empty files folder is normal. It was last updated Nov 2 at 22:30, probably by an earlier step of this work; it still runs and its save is intact.
- Do not scan the user's Downloads. Ask for paths.
- Check what is in the foreground before driving the phone. Input sent over adb goes to whatever is on screen, and it has landed in other apps before.

## Build and run

Windows, engine plus APK:

```
cd dm/android && python tools/win_build_lib.py <GAME_ID>
cp out/host-<GAME_ID>-vulkan/lib/libnxnative.so <libs folder>/
cd ../.. && STRICT_16K=1 python dm/android/tools/build_apk.py --game <GAME_ID> --version <n> \
  --libs <libs folder> --pylib <...>/aot/pyandroid/prefix/lib/python3.14 \
  --capstone-py <...>/aot/capsrc/capstone-5.0.7/bindings/python/capstone \
  --ks <keystore> --debuggable --out <file>.apk
```

- Use `C:/...` paths for Python and adb from Git Bash. `/c/...` fails when `MSYS_NO_PATHCONV=1` is set.
- Use `--game 01008F6008C5E000` for Paldea and `--game 01008DB008C2C000` for Galar. Each game pair builds its own `libnxnative.so`.
- All helper libraries in WSL's `~/psa-build/libs` are now 16 KB aligned. The full rebuild is `scripts/build_apk_pc.sh`.
  - It needs `cmake` and `ninja` in WSL. They were installed with `pip install --user`, so put `~/.local/bin` on PATH.
  - Delete `libadrenotools/build` when changing linker flags. CMake otherwise reuses the old link.
- The phone currently has **debuggable test builds** of both apps. Android 15+ shows its 16 KB notice only for debuggable apps. A release must drop `--debuggable`.

Game data on the phone goes in the first game's files folder, and the second game goes in a subfolder named after its ID. `ready` must hold the shader chain: `bebeadf4ac3b` for Galar, `b72f9b70ebb2` for Paldea. Prepared data from this round is in the session scratchpad (`violet/`, `scarlet/`, `swphone/`, `shphone/`, `swsetup/`), which is temporary. If it is gone, rebuild from the dumps; the app's own setup is the supported route.

## What works

| Game | Seen on the S25+ |
|---|---|
| Violet | Language, look and name screens, enrollment form, and the 3D classroom and library scenes |
| Scarlet | Boots; 3D scenes of its opening (Clavell in the white room, the library) |
| Sword | New-game intro and the trainer confirm screen with the 3D trainer, 30 fps, no missing shaders |
| Shield | Boots to the language screen, 30 fps, no missing shaders |

None of the four new-app copies has been played past these screens. That means no overworld and no battles.

Done in this round:

- 3D render targets render all their slices (`att_view`, layered `pass_begin`). This was the cause of all-black 3D on Scarlet and Violet: their 32x32x32 colour-grading table is filled in one instanced draw using `gl_Layer`.
- The upscaler's oversized triangle is scissored to the picture box. This fixed the off-centre picture and the diagonal cut corner.
- Shared font as the TTF itself, and an offline Bluetooth LE layer (`hle_sdk16.c`). Both were boot crashes on Violet.
- One game launcher per app (`SetupActivity.launcher`). Its Play button goes through `pendingWork`, so captured shaders are merged first.
- Background play, on by default: settings panel, System, Leaving the game (`DM_BACKGROUND=off`).
- Main and presenting threads pinned to the cores with the highest current clock limit, rechecked every 5 s (`pin_update`, `DM_CPU_PIN=off`).

## Open problems

### 1. Locked 30 fps on Scarlet and Violet (the main one)

Violet's busy scenes measured 37-41 ms a frame with pinning, about 48 ms before it, with about 484 draws a frame. Each run was on a phone at a different temperature, so treat the before/after as indicative only.

- Game thread frame: game logic plus the NVN-to-Vulkan replay, run one after the other. Replay CPU was 21 ms before pinning, 13-14 ms after.
- `DM_VK_PROF=1` breakdown per frame, measured before pinning: pass + pipeline lookup 4-4.5 ms, dynamic state 0.3, descriptors 1.1-1.3, flush + draw 2.1-2.3, target changes 2.3-2.7, state ops 1.4-1.7, queue submit 0.1. That is about 13 of the 19-21 ms measured; **the other 6-8 ms is unaccounted for**. Candidates are the timers' own overhead, and Vulkan driver time outside the timed ops.
- GPU (`DM_VK_GPU_TIME=1`): 21-30 ms at 1920x1080. There is little margin, so the replay thread alone may not hold 30 fps in busy scenes.
- Handheld mode (`DM_OPMODE=0`) did not help: GPU stayed about 21 ms and frames were slower. That run was one noisy comparison. The render targets were not confirmed to shrink, so check whether the game really changes resolution before relying on this.
- **Measurements are affected by heat.** While fast-charging at a 41-42 °C battery, the phone capped cores at 1.55-1.69 GHz (normal 3.53 and 4.47). Read `scaling_max_freq` with every measurement, and test on a cool phone on USB only.
- CPU profiling with `simpleperf` is blocked on this phone without root (perf events not supported). So is `debuggerd`.

**Next step: `DM_NVN_ASYNC=1`,** the replay on its own thread (`hle_nvn.c`: `rq_push`, `rq_main`, `rq_drain`). It is implemented and off by default.

- One run reached the look screen with no hang and no errors, at frame time p50 33-41 ms, p95 43-45 ms.
- **The A/B comparison against it being off was never finished**, because the run was interrupted. So there is no evidence yet that it helps.
- It has not been tested past that screen. Risks to test: objects changed while commands that name them are queued (only texture, program and pool finalize/initialize drain the queue), window teardown, and app suspend.
- A wait over 5 s logs itself as `[RED] ... has waited 5 s on the replay thread`.

Other ideas not tried: fewer render-pass changes (about 117 target sets a frame; "kept the open pass" is always 0), and a safe pipeline shortcut. A simple last-key cache in `pso_get` was tried and reverted: a GPU device loss happened on the run that had it, and pipelines can be replaced and destroyed later (`DF_PIPELINE`). The cause was not proven beyond that one run.

### 2. Violet's enrollment photo is black

The photo box on the enrollment form is black. Nobody has compared this with a Switch, so confirm it is really wrong first.

Ruled out:
- Missing shaders: no `SPIR-V missing` lines and no captures on Violet or Scarlet.
- Viewport offset: viewports are all full-target. The "character shifted up" reading of a frame dump was wrong; it was the camera framing.
- A second texture object over the render target's memory: a fallback for this never fired, and was removed.
- `tex_flush_upload` / `tex_dirty_upload`: tried, no effect on the black 3D, removed.

Leads: two 256x256 render targets (VkFormat 78) were black in a frame dump and no draw used a 256x256 viewport, so they may be cleared and never drawn. The dumped frame may not have been the photo's frame. The screen appears briefly and is awkward to reach.

Tool: `DM_NVN_TEXDUMP_NOW=<trigger file>:<path with %d>.png` dumps the next frame after the trigger file appears, and logs each pass, draw, texture and buffer as `[trace]`. `DM_NVN_TEXDUMP_ALPHA=1` keeps real alpha. The dump cannot show integer formats (they read as zero), and the alpha option showed the UI layer's alpha is a see-through mask, not a fault.

### 3. Sword and Shield shaders are only complete after they have been captured

46 shaders are in no game file. The splitter has no `.tfxo` reader, so they are not in a fresh pack; the Mac note attributes shaders like these to YEBIS, stored in a zlib stream in `main`. Today they are saved when the game first uses them and merged on the next Play.

- Shield's pack has 38,546 of 38,547 and Sword's 38,539. Each new player sees those effects missing until they have been captured.
- A proper fix is porting the Mac's `tfxo_streams` / `tfxo_shaders` into `dm/gfx/tools/shader_split.py`, so setup finds them all. Verify the zlib stream location on these builds first.
- Separately, Sword's setup used a shader map with Shield's rows at build time. Sword and Shield hold the same 38,547 keys in that map, but they were compared only through these maps.

### 4. Remaining red log lines

- `nvnDeviceGetWindowOriginMode` and `nvnDeviceGetDepthMode` are unimplemented stubs, seen on Sword and Shield. The Mac project implements them. Easy.
- `no render target could be attached`, once per run on Sword and Shield, cause unknown. Also present in the user's working Shield app.
- `storage buffer ... is not 64-aligned: bound as a snapshot` on Violet. A snapshot reads guest memory at replay time and misses GPU writes earlier in the submission. `DM_VK_SSBO_GPU_COPY=1` copies on the GPU instead, but a device loss happened on a run using it, cause not proven. It did not fix any visible problem.

### 5. Not tested at all

- Real Bink video (`demo/movie/*.bk2`) after the texture tracking settings were removed. The Mac note says the movie module's imports are supplied by the host. If videos are black, look there first.
- Sound by ear on Scarlet and Violet. The log shows callbacks running, and `audren: ... 0 of them not silent` on one output. The game may simply use the other one; verify.
- Multiplayer: same-Wi-Fi (`hle_ldn.c`) and room server (`hle_relay.c`, `dm/server`) have never run between two devices.
- Mali, Xclipse, PowerVR, PanVK, 4 GB phones, in this round.
- 60 fps option, and pacing on 60/90/120 Hz screens.
- App icon returning to a running game (`GameActivity.alive`) is built but untested.
- Background play was checked on Scarlet only, for sound and frame counters over about 25 s. There is no foreground service, so Android may stop the game after longer.
- Pinning was measured on Violet only, and it has not been judged on phones with other core layouts. Sword and Shield ran with it without problems on these screens.
- Mods, saves import/export and the settings panel on the new builds.

### 6. Not done

- Sword and Shield sharing identical game files. Only shaders are reused today (`setup_game.py` donor). The phone holds two copies of about 13 GB and had about 4.7 GB free.
- Translations for the new strings: "Background", "Leaving the game", "Keep playing", "Pause", "Set up or manage a game", "Set up". Add them to `dm/android/tools/lang_src.py` and run `lang_check.py`.
- The user asked for a "rebuild" button "by the side, or both in the middle". What it should do was not settled. A "Rebuild shaders" action on each game's setup screen was proposed; ask.
- From the older list: water rendering darker than on the Switch, Galar 1.0 release (delete old releases except the last 2), a compatibility spreadsheet.

## Things that wasted time

- **`g_rmu` is held for all of `push_stage`.** The comments say "caller holds g_rmu". Adding locks in functions called from there deadlocks the replay before the first frame, as a blue or black screen.
- **Compare shader sets by key, not by a column that happens to match.** A check on the package column wrongly said two packs were the same.
- A frame dump shows one frame. Confirm which screen it is from before drawing conclusions.
- After launching the launcher, wait for the rotation to landscape (about 7 s) before tapping. An early tap lands on the phone's home screen.
- In-game taps at A's position type into the name keyboard when it is open. The test saves on the phone have junk names; they are not the user's saves.
- The phone's `sed` does not support `\|`. Use `grep -v -E` to edit `env.txt`.
- Debug switches in `env.txt` persist between runs. Clear them after testing. They were all cleared when this was written.
