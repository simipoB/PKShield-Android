# Galar Native — engine analysis and GPU testing handout

For work on [simipoB/Galar-Native-Android](https://github.com/simipoB/Galar-Native-Android): studying how Pokémon Sword/Shield (1.3.2) uses the
GPU, and testing Mali, Xclipse and Adreno phones, including Samsung Remote Test Lab (RTL) devices driven from VS Code.

Dev setup assumed: Windows + WSL2 (Ubuntu), VS Code, the repo built with `scripts/build_apk_pc.sh` ([BUILDING.md](BUILDING.md)).

---

## 1. Ground rules

These keep the project legal and keep your accounts safe.

- **Your own dump, your own keys, your own machine.** Analysis works on files from your own console. Nothing from the game goes into the repo, issues,
  PRs, releases or chat logs.
- **No decompiled game code in the repo.** Ghidra pseudocode of the game is the game's code. In the repo, keep only *facts* (offsets, function
  roles, which NVN calls and texture formats the game uses) and *your own* code written from those facts. Keep raw Ghidra projects and exports outside the repo
  (e.g. `~/galar-re/`), never under `dm/`.
- **Never put the dump or `prod.keys` on a Remote Test Lab phone.** RTL devices are shared Samsung hardware that other people use after you. Use RTL only for
  things that need no game data (section 6).
- **Only use tools you're licensed to use.** Use Ghidra (free), or IDA/Hopper/JEB only with your own licence. REA does not grant licences for them.

---

## 2. Will decompiling the engine fix Mali and Xclipse?

Partly. Know which part before you spend weeks on it.

The game's ARM64 code is **the same on every phone**. What differs between Adreno, Mali and Xclipse is everything *below* the game:

| Layer | Where in the repo | GPU-specific? |
| --- | --- | --- |
| Game Freak engine (game's own code) | the player's `main` NSO | No — identical everywhere |
| NVN → Vulkan translation | `dm/gfx/nvn/nvn_vk.c`, `nvn_tex.c`, `nvn_spv.c` | **Yes** |
| Shader translation | `dm/gfx/tools/dmsh_xlate` (Ryujinx), SPIRV-Cross | **Yes** (driver shader compilers differ) |
| Texture formats | `dm/gfx/nvn/nvn_tex.c`, `dm/gfx/tex` | **Yes** (BC vs ASTC support, compact formats) |
| Vulkan driver | phone / Turnip / PanVK / RADV | **Yes** |

So most Mali/Xclipse fixes land in `dm/gfx`. Engine analysis helps by telling you **what the game asks for**:

- which NVN functions it fetches and calls, and how often;
- which texture formats, sizes and render-target layouts it creates;
- how it synchronises (sync objects, queue fences, buffer reuse). Xclipse crashes are often sync or memory-mapping issues; see the existing
  Xclipse special case at `dm/gfx/nvn/nvn_vk.c:192`;
- which shaders it binds per scene, so a broken effect on Mali can be traced to the one shader that breaks.

### Architecture goal

GPU quirks are currently decided by `vendorID` checks spread through `nvn_vk.c` (e.g. `0x144D` Samsung at line 192, `0x13B5` Arm formats at line
1012, `0x5143` Qualcomm). The goal is:

1. **One** NVN translation layer, resource system and shader pipeline for every GPU.
2. **One capability probe** at startup: features, extensions and format support, read from Vulkan rather than assumed from the vendor.
3. **One device-profile table** (vendor + driver version → workarounds), replacing scattered `if (vendorID == …)` checks.
4. A per-GPU **regression log** (section 7) so a fix for Mali can't silently break Adreno.

The S25+ (Adreno 830) stays the reference device, but no code path should depend on it.

---

## 3. Tools

| Tool | Use |
| --- | --- |
| **Ghidra** (free) | Decompiler and disassembler for the game's ARM64 code |
| **REA** (`npx rea-agents setup`, official: [morluto/rea](https://github.com/morluto/rea)) | Lets Claude Code drive Ghidra: find functions, trace calls, gather evidence |
| **This repo's tools** (`dm/tools/`) | Switch-specific helpers, listed below |
| **adb** (Android platform-tools, Windows side) | Phones, both local and RTL |

A local REA fork (`C:\Users\daniel\rea`) adds NSO/NRO inspection and NSO→ELF export for 32- and 64-bit modules. It is **not wired into the MCP
server or CLI yet**; use `dm/tools/nso2elf.py` until it is.

### Repo tools you'll use

| Command | What it gives you |
| --- | --- |
| `dm/tools/nx_extract` — `nx_extract <prod.keys> <base.nsp> <update.nsp\|-> <out dir>` | ExeFS (`main`, `main.npdm`, `subsdk*`) and RomFS from your own NSPs |
| `python3 dm/tools/nso2elf.py main main.elf` | ELF64 that Ghidra's stock loader opens, with imports, exports and relocations |
| `python3 dm/tools/nso.py main main.orig.bin` | Raw loaded image (`.orig.bin`) for the tools below |
| `python3 dm/tools/sdis.py main.orig.bin <symbol\|0xoffset> [count]` | Disassembly with symbol names; `--xref 0xaddr` finds who references an address |
| `python3 dm/tools/nvn_slots_got.py …` | The game's NVN fetch table: every NVN function it looks up and the slot it stores it in |
| `python3 dm/tools/nso_syms.py <dm/host> - <ExeFS>` | The game's SDK surface compared with the host's HLE hooks |
| `python3 dm/tools/nso_fnmap.py old.orig.bin new.orig.bin 0x…` | Carries offsets across game versions |
| `python3 dm/tools/insn_census.py …` | Counts instruction sites the rewriter must handle |

Keep every output under `~/galar-re/`, outside the repo.

---

## 4. Engine analysis workflow

```sh
# WSL2
mkdir -p ~/galar-re && cd ~/galar-re
nx_extract ~/keys/prod.keys ~/dumps/shield-base.nsp ~/dumps/shield-1.3.2.nsp ./shield
python3 ~/Galar-Native-Android/dm/tools/nso2elf.py shield/exefs/main shield-main.elf
python3 ~/Galar-Native-Android/dm/tools/nso.py     shield/exefs/main shield-main.orig.bin
```

(Adjust the paths to where `nx_extract` puts ExeFS in your output folder.)

1. **Import** `shield-main.elf` into a Ghidra project (`~/galar-re/ghidra/`). Let auto-analysis finish; it takes a while on a module this size.
2. **Connect REA** to that Ghidra project, then ask Claude Code questions such as:
   - "List the NVN functions this module calls most, grouped by subsystem (textures, command buffers, sync, shaders)."
   - "Trace what happens between `nvnQueueSubmitCommands` and `nvnQueuePresentTexture` in one frame."
   - "Which texture formats does the game pass to `nvnTextureBuilderSetFormat`, and where?"
3. **Cross-check** with `nvn_slots_got.py`. Its fetch table is the ground truth for which NVN entry points exist in this build.
4. **Map subsystems** and write down offsets and roles only:

   | Subsystem | What to record | Why it matters for GPUs |
   | --- | --- | --- |
   | Renderer frame loop | submit / present order, frames in flight | Frame pacing, Xclipse sync |
   | Texture creation | formats, sizes, mip counts, render targets | Mali/PowerVR format fallbacks, memory |
   | Shader binding | which program per pass | Isolating a broken effect |
   | Sync | sync objects, waits, buffer reuse | Hangs and crashes on stricter drivers |
   | Memory pools | pool sizes and flags | CPU-visible vs device-local mapping (Xclipse) |

5. **Turn findings into code** in `dm/gfx` as your own implementation. The pull request or commit message explains the *behaviour* (e.g. "the game reuses
   pool X after fence Y; Mali needs a barrier here"), not the game's code.

### Notes file

Keep `~/galar-re/NOTES.md` with lines like:

```
0x00ABC123  renderer: per-frame submit; waits fence, then reuses cmd memory  → nvn_vk.c sync path
0x00DEF456  tex: creates the 3 shadow-map RTs (D32F)                         → check D32F+sampling on Mali
```

---

## 5. Samsung Remote Test Lab from VS Code

RTL gives you real Galaxy phones in a browser. ADB access goes through Samsung's **Remote Debug Bridge (RDB)** tool on Windows. The exact menu
names change, so **follow the RDB instructions shown on the RTL device page**. The usual flow:

1. Sign in at [developer.samsung.com/remote-test-lab](https://developer.samsung.com/remote-test-lab) and reserve a device. Each reservation uses credits.
2. On the device page, open the **Remote Debug Bridge** option and download RDB for Windows. Run it and leave it running.
3. Click **Connect** on the RTL page. It shows a local port.
4. In VS Code, open a **PowerShell** terminal (Windows side, not WSL):

   ```powershell
   adb connect localhost:PORT      # PORT = the one RTL shows
   adb devices -l
   ```

   Accept the USB-debugging prompt on the remote screen if one appears.

### WSL2 and adb

Run RDB and `adb` on **Windows**. From WSL, call the Windows binary so both sides share one adb server:

```sh
alias adb='/mnt/c/Users/daniel/AppData/Local/Android/Sdk/platform-tools/adb.exe'   # adjust to your platform-tools path
```

### Several devices at once

Samsung doesn't publish a limit on simultaneous reservations. Try two first. adb handles several devices; name one with `-s`:

```powershell
adb devices -l
adb -s localhost:PORT1 shell getprop ro.product.model
adb -s localhost:PORT2 install -r GalarNative.apk
```

---

## 6. What to run on RTL (no game data)

### a) The device probe — `dm/android/probe`

It reports the GPU, driver, the Vulkan features and extensions the NVN replay uses, and texture-format support (BC, ASTC, depth). It also covers memory and
x18/TLS behaviour, with **no game files**. This is the main RTL test.

> `dm/android/probe/build.sh` currently points at macOS paths (a `darwin-x86_64` clang and a Homebrew JDK). On WSL2, set `ANDROID_SDK` and `JAVA_HOME`
> and change the clang path to `linux-x86_64` before running it.

```powershell
adb -s localhost:PORT install -r nxnative-probe.apk
adb -s localhost:PORT shell am start -n com.nxnative.probe/.MainActivity   # check the activity name in AndroidManifest.xml
```

Use **Share / Copy** in the app to save the report as `reports/<model>-<gpu>.txt` on your PC.

### b) Galar Native install and launch, up to the setup screen

This checks install, the 16 KB page alignment, native libraries loading and the driver picker. **Stop at the setup screen. Don't pick game files or keys.**

### c) Device facts

```powershell
adb -s localhost:PORT shell getprop ro.product.model
adb -s localhost:PORT shell getprop ro.soc.model
adb -s localhost:PORT shell getprop ro.hardware.vulkan
adb -s localhost:PORT shell getprop ro.build.version.release
adb -s localhost:PORT logcat -d > rtl-<model>.log
```

Full gameplay tests (rendering, shaders, frame rate) need the game, so run them on **phones you own** or ones testers own, using the in-app log
(`Download/PokemonShield-log.txt` / `PokemonSword-log.txt`).

---

## 7. GPU compatibility record

One row per device per build. Keep it in an issue or a `docs/compat/` table.

| Date | Build | Device | SoC / GPU | Driver (from probe) | Android | Probe OK | Launch OK | Notes / issue |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | v0.53 | Galaxy S25+ | 8 Elite / Adreno 830 | | | | | reference |
| | | Galaxy S26 FE (RTL) | | | 17 | | | |
| | | Galaxy Tab S12 Ultra (RTL) | | | 17 | | | |

Open issues to re-test against: #32 (Mali stock gaps), #40 / #47 (Mali white screen), #41 / #45 (Xclipse crashes), #44 (old Adreno driver).

### Per-build checklist

- [ ] Probe report saved for each new device or driver version
- [ ] Installs and reaches the setup screen (RTL)
- [ ] Owned devices: boots to title, enters Wild Area, battles, no white screen, 30 fps held (the project's minimum)
- [ ] Adreno reference unchanged (no regression)
- [ ] Logs attached to the matching issue

---

## 8. Quick reference

```text
Repo:        https://github.com/simipoB/Galar-Native-Android
Build:       bash scripts/build_apk_pc.sh            (WSL2)  → ~/psa-build/PokemonShield.apk
Analysis:    ~/galar-re/   (outside the repo — never commit)
NSO → ELF:   python3 dm/tools/nso2elf.py main main.elf
RTL:         RDB running → adb connect localhost:PORT → adb -s localhost:PORT …
Never:       game files, keys or decompiled code in the repo or on RTL phones
```
