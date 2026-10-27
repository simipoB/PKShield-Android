# dm/android: the Android side

| Folder / file | What it is |
| --- | --- |
| `app/jni/android_main.c` | The game's native activity: starts the host, input (touch, controllers, the phone's keyboard for names), settings from `env.txt`, logs, graphics Auto |
| `app/jni/touch_ui.c` | The touch controls and their in-game layout editor (hold − for 2 s) |
| `app/java/com/nxnative/SetupActivity.java` | The first screen: game files, keys, settings, setup; then straight into the game |
| `app/java/com/nxnative/GameActivity.java` | NativeActivity plus controller connection (a connected controller hides the touch controls) |
| `tools/setup_game.py` | The setup the phone runs: unpack (nx_extract), rewrite the code (patch_nso.py), translate and pack the shaders; `--captures` adds the shaders the game builds in memory |
| `tools/build_apk.py` | Packs the APK: native libraries, Java, CPython's standard library, the setup scripts |
| `extract_aot/`, `xlate_aot/` | The unpacker (LibHac) and the shader translator (Ryujinx) built for Android with .NET NativeAOT |
| `spirv_cross/`, `python/`, `pyshim/` | SPIRV-Cross for Android; the Python launcher; the LZ4 stand-in the setup scripts import |
| `compat/` | What the host needs from Apple platforms, on Android: AAudio sound output, MediaCodec AAC, reading game files in place |
| `host/movie_avc_android.c` | H.264 movies (the staff credits) on MediaCodec |
| `Makefile.host`, `tools/win_build_lib.py` | Build the engine library (Linux / macOS, or Windows) |

Setup builds shader locations, facts, and pairs from the selected game dump instead of assuming it contains every shader in the profile's reference map. This lets base-game-only setups use their actual package indices; profile entries for shaders captured later at runtime are retained.

## Graphics (`dm/gfx/nvn/nvn_vk.c`)

The game's NVN commands are recorded and replayed on Vulkan. Paths chosen at start from what the device reports:

- **The driver**: the phone's own, or a custom one (Mesa Turnip) opened through libadrenotools; every Vulkan function is loaded at run time
  (`vk_dispatch.h`).
- **Vulkan version**: 1.3, or 1.1 / 1.2 with dynamic rendering, timeline semaphores and extended dynamic state as extensions.
- **GPU memory**: the game's memory pools are shared with the GPU through host-memory import, `VK_KHR_external_memory_fd` export, or driver-memory
  mapping moves. If a driver advertises fd export but rejects an opaque-fd buffer, or its mappings cannot be moved onto the game pools, the app falls back
  to mirrored memory, copying changes in both directions around GPU submissions.
- **Framebuffer compression**: render targets list their view formats instead of being freely mutable, which keeps Adreno's UBWC on; on Mali
  every plain colour image does, and those with a single format are not mutable at all (AFBC). With `VK_EXT_image_compression_control` the log
  says whether the first render targets are compressed.
- **Textures**: BC1–BC7 sampled by the GPU where it can, else decoded on the CPU as they load (`dm/gfx/tex/bc_decode.c`).
- **Shaders**: SPIR-V made on the phone during setup (`shaders.pak`); pipelines recorded as the game makes them and built ahead at the next start.

## Graphics quality

`DM_GRAPHICS=low` starts the game in the console's handheld mode (720p, the game's own lighter settings); `auto` switches between TV and
handheld mode from the phone's thermal status, as a Switch does when it leaves or enters its dock.
