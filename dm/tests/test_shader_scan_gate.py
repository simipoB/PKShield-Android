#!/usr/bin/env python3
# The shader scan on a phone that never has the gate's free memory (#137: a Moto G52 sat on "find the shaders" for 7 hours): the workers past the first waited for it
# forever. Now they run one at a time once worker 0 is done. Runs Android's path (forked workers, no process pools) with a gate no phone could meet. Linux only (fork)
import os, signal, sys, time

os.environ.setdefault('DM_GAME', '01008DB008C2C000')
sys.modules['multiprocessing.synchronize'] = None   # as on Android's Python: pmap forks its own workers
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../gfx/tools'))
import shader_split

os.environ['DM_GATE_MB'] = str(1 << 30)   # a terabyte free: never
os.environ['DM_GATE_FLOOR_MB'] = '300'

def stuck(*_): print('FAIL: the scan is still waiting after 60 s', flush=True); os._exit(1)
signal.signal(signal.SIGALRM, stuck); signal.alarm(60)
t = time.time()
got = shader_split.pmap(4, lambda x: x * x, list(range(40)), ({}, {}, {}))
signal.alarm(0)
assert got == [x * x for x in range(40)], got
print(f'4 workers, memory never enough: all 40 results in order in {time.time() - t:.1f} s')
