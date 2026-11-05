#!/bin/bash
# crowdtest.sh <tag> <env lines...>: restart Sword with these env lines (on top of the base), go through the name entry to the stadium, record the crowd, count flicker
export MSYS_NO_PATHCONV=1
A="/c/Users/daniel/AppData/Local/Android/Sdk/platform-tools/adb.exe -s 10.0.0.178:5555"; R=/sdcard/Android/data/com.nxnative.pokemonshield/files/0100ABF008968000
SC=$(cygpath -m /c/Users/daniel/AppData/Local/Temp/claude/c--Users-daniel-PKShield-Android/26aa72e2-84b8-4dc8-a80a-4c8cf916ead8/scratchpad)
tag=$1; shift
$A shell "am force-stop com.nxnative.pokemonshield; printf 'DM_VK_BC_TRANSCODE=off\nDM_SCALER=sgsr\nDM_BUILD_SIGNER=official\nDM_VK_SPLIT=0\n' > $R/env.txt"
for e in "$@"; do $A shell "echo $e >> $R/env.txt"; done
$A shell "am start -n com.nxnative.pokemonshield/com.nxnative.SetupActivity --es game 0100ABF008968000" >/dev/null
sleep 42; for i in $(seq 1 4); do $A shell "input tap 2738 1011"; sleep 2; done
for i in $(seq 1 15); do $A shell "dumpsys window | grep -q 'mCurrentFocus.*PopupWindow\|InputMethod' " ; $A exec-out screencap -p > $SC/flick/t.png; python -c "
import cv2,sys; i=cv2.imread('$SC/flick/t.png'); h,w=i.shape[:2]; sys.exit(0 if i[int(h*.8):,:].mean()<60 and i[:int(h*.3)].mean()>150 else 1)" && break; $A shell "input tap 2738 1011"; sleep 2; done
for i in 1 2 3 4 5 6; do $A shell input keyevent 67; done; $A shell input text Player; sleep 1
$A shell "input keyevent 66"; sleep 2; $A shell "input keyevent 66"; sleep 2
for i in $(seq 1 10); do $A shell "input tap 2738 1011"; sleep 2; done
$A shell "screenrecord --time-limit 8 --bit-rate 40000000 /data/local/tmp/ct.mp4"; $A pull /data/local/tmp/ct.mp4 $SC/flick/ct_$tag.mp4 >/dev/null
$A exec-out screencap -p > $SC/flick/ct_$tag.png
python - "$SC/flick/ct_$tag.mp4" <<'PY'
import cv2, numpy as np, sys
c=cv2.VideoCapture(sys.argv[1]); fr=[]
while True:
    ok,f=c.read()
    if not ok: break
    h,w=f.shape[:2]; fr.append(f[int(.30*h):int(.50*h),int(.10*w):int(.40*w)].astype(np.int16))
d=[float(np.mean(np.abs(fr[i]-fr[i-1]))) for i in range(1,len(fr))]
print(len(fr),'frames; big crowd jumps',sum(1 for x in d if x>25))
PY
$A shell "grep -E 'frame step: held|frame time p50' $R/log.txt | tail -2" | cut -c1-140
