#!/usr/bin/env python3
# Usage: timer.py <runs> <binary> <script>
# Runs <binary> <script> N times and reports min / mean-of-5-best / median / max.
import subprocess, time, sys
if len(sys.argv) != 4:
    sys.exit('usage: timer.py <runs> <binary> <script>')
runs = int(sys.argv[1])
bin = sys.argv[2]
script = sys.argv[3]
ts = []
for _ in range(runs):
    t0 = time.perf_counter()
    subprocess.run([bin, script], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ts.append(time.perf_counter() - t0)
ts.sort()
best5 = ts[:5]
print(f"min={min(ts)*1000:.1f}ms  mean5={sum(best5)/5*1000:.1f}ms  median={ts[len(ts)//2]*1000:.1f}ms  max={max(ts)*1000:.1f}ms")
