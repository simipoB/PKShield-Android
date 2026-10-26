# Windows stand-in for `make -f Makefile.host GAME=<id> lib` (no make there): the NDK at ~/android-ndk/android-ndk-r27c (NDK r27c = 27.2.12479018). Run from dm/android: python tools/win_build_lib.py <GAME>
import glob, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

GAME = sys.argv[1]
NDK = os.path.expanduser("~/android-ndk/android-ndk-r27c")
TC = NDK + "/toolchains/llvm/prebuilt/windows-x86_64/bin"
CC = [TC + "/clang.exe", "--target=aarch64-linux-android33"]
HOST, GFXD = "../host", "../gfx"
OUT = f"out/host-{GAME}-vulkan"
GAME_DIR = f"../games/{GAME}"
GLUE = NDK + "/sources/android/native_app_glue"
CFLAGS = ("-O2 -g -fPIC -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format -fno-omit-frame-pointer "
          "-include compat/darwin.h -Icompat").split() + [f"-I{d}" for d in (HOST, GFXD + "/nvn", GFXD + "/nvn/include", "../third_party/vulkan-headers/include",
          OUT + "/gen", GAME_DIR, "../third_party/libvpx", "../third_party/stb_vorbis")]

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r

_HDR = None
def newer(src, dst):   # also when any header changed (no dependency files here): dmsh_chain.h, game_profile.h and the rest
    global _HDR
    if _HDR is None: _HDR = max((os.path.getmtime(h) for d in (HOST, GFXD, GAME_DIR, OUT + '/gen', '.') for h in glob.glob(d + '/**/*.h', recursive=True)), default=0)
    return not os.path.exists(dst) or max(os.path.getmtime(src), _HDR) > os.path.getmtime(dst)

os.makedirs(OUT + "/gen", exist_ok=True)
gh = OUT + "/gen/game_profile.h"
if not os.path.exists(gh):
    run([sys.executable, "../games/game.py", "header", GAME, gh])

g = lambda p: sorted(glob.glob(p))
src = [s for s in g(HOST + "/*.c") if os.path.basename(s) not in ("hle_movie_avc.c", "main.c")]
src += [s for s in g(GFXD + "/*.c") + g(GFXD + "/nvn/*.c") + g(GFXD + "/tex/*.c")]
src = [s.replace("\\", "/") for s in src]
local = ["compat/compat.c", "compat/apkfs.c", "compat/aaudio_unit.c", "compat/audio_converter.c", "host/movie_avc_android.c"]

jobs = []  # (cmd, src, obj)
for s in src:
    o = OUT + "/obj/" + s[3:-2] + ".o"; jobs.append((CC + CFLAGS + ["-c", s, "-o", o], s, o))
for s in local:
    o = OUT + "/obj/android/" + s[:-2] + ".o"; jobs.append((CC + CFLAGS + ["-c", s, "-o", o], s, o))
jobs.append((CC + CFLAGS + ["-Dmain=dm_main", "-c", HOST + "/main.c", "-o", OUT + "/objl/main.o"], HOST + "/main.c", OUT + "/objl/main.o"))
jobs.append((CC + CFLAGS + ["-I" + GLUE, "-c", "app/jni/android_main.c", "-o", OUT + "/objl/android_main.o"], "app/jni/android_main.c", OUT + "/objl/android_main.o"))
jobs.append((CC + CFLAGS + ["-I" + GFXD + "/nvn", "-c", "app/jni/touch_ui.c", "-o", OUT + "/objl/touch_ui.o"], "app/jni/touch_ui.c", OUT + "/objl/touch_ui.o"))
jobs.append((CC + CFLAGS + ["-c", "app/jni/touch_menu.c", "-o", OUT + "/objl/touch_menu.o"], "app/jni/touch_menu.c", OUT + "/objl/touch_menu.o"))
jobs.append((CC + ["-O2", "-fPIC", "-I" + GLUE, "-c", GLUE + "/android_native_app_glue.c", "-o", OUT + "/objl/glue.o"], GLUE + "/android_native_app_glue.c", OUT + "/objl/glue.o"))
for a in (HOST + "/entry.S", GFXD + "/nvn/nvn_entry.S", GFXD + "/nvn/nvn_thunks.S"):
    e = OUT + "/asm/" + os.path.basename(a); os.makedirs(OUT + "/asm", exist_ok=True)
    run([sys.executable, "elf_asm.py", a, e])
    jobs.append((CC + ["-c", e, "-o", e[:-2] + ".o"], e, e[:-2] + ".o"))
VPX = "../third_party/libvpx"
vpx = [os.path.join(dp, f).replace("\\", "/") for dp, _, fs in os.walk(VPX) for f in fs if f.endswith(".c")]
vpx_obj = []
for s in sorted(vpx):
    o = OUT + "/vpx/" + s[len(VPX) + 1:-2] + ".o"; vpx_obj.append(o)
    jobs.append((CC + ["-O2", "-fPIC", "-I" + VPX, "-I" + VPX + "/config", "-c", s, "-o", o], s, o))

def build(j):
    cmd, s, o = j
    if not newer(s, o):
        return
    os.makedirs(os.path.dirname(o), exist_ok=True)
    run(cmd)

with ThreadPoolExecutor(os.cpu_count()) as ex:
    list(ex.map(build, jobs))
print(f"compiled {len(jobs)} units")

lib = OUT + "/libvpx.a"
if os.path.exists(lib): os.remove(lib)
run([TC + "/llvm-ar.exe", "rcs", lib] + vpx_obj)
objs = [o for _, _, o in jobs if o not in vpx_obj]
os.makedirs(OUT + "/lib", exist_ok=True)
rsp = OUT + "/link.rsp"
open(rsp, "w").write("\n".join(objs))
run(CC + ["-shared", "-o", OUT + "/lib/libnxnative.so", "@" + rsp, lib, "-lm", "-ldl", "-llog", "-lz", "-laaudio", "-lmediandk", "-lvulkan", "-landroid",
          "-Wl,-Bsymbolic", "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384", "-u", "ANativeActivity_onCreate"])
print("built", OUT + "/lib/libnxnative.so")
