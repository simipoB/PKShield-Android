#!/bin/bash
# arun.sh <tag> <env lines...>: the user's Arceus env (backup) + these lines, start, load the save, screenshot to arc/<tag>.png
A="/c/Users/daniel/AppData/Local/Android/Sdk/platform-tools/adb.exe -s 10.0.0.178:5555"; F=/sdcard/Android/data/com.nxnative.arceus/files
SC=/c/Users/daniel/AppData/Local/Temp/claude/c--Users-daniel-PKShield-Android/26aa72e2-84b8-4dc8-a80a-4c8cf916ead8/scratchpad
tag=$1; shift
$A shell "am force-stop com.nxnative.arceus; cp /data/local/tmp/arceus_env_backup.txt $F/env.txt"
for e in "$@"; do $A shell "echo $e >> $F/env.txt"; done
$A shell "monkey -p com.nxnative.arceus -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
sleep 55; for i in 1 2 3; do $A shell input tap 2710 767; sleep 4; done; sleep 28
$A exec-out screencap -p > $SC/arc/$tag.png
python -c "
import cv2; i=cv2.imread(r'$(cygpath -m $SC)/arc/$tag.png'); c=i[760:860,1500:2400]
import numpy as np; b,g,r=[float(x) for x in c.reshape(-1,3).mean(0)]; print('$tag ground under tents rgb %.0f %.0f %.0f  (purple if g well below r,b)'%(r,g,b))"
