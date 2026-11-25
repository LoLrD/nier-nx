import bisect
import re
import subprocess
import sys
from collections import Counter

run, first, last, thread_prefix = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
nm = subprocess.run(["nm", "-n", "-C", f"{run}/nier.elf"], capture_output=True, text=True).stdout.splitlines()
addrs, names = [], []
for line in nm:
    parts = line.split(" ", 2)
    if len(parts) == 3 and parts[1] in "tTwW":
        addrs.append(int(parts[0], 16))
        names.append(parts[2])

def sym(x):
    i = bisect.bisect_right(addrs, x) - 1
    return names[i] if i >= 0 else "?"

blocks = open(f"{run}/rex_profile.log", errors="replace").read().split("\n==== ")[1:]
hits = Counter()
running_total = 0.0
for index in range(first, last + 1):
    for th in re.split(r"\n-- thread ", blocks[index])[1:]:
        name = re.match(r'\d+ "([^"]*)"', th).group(1)
        if not name.startswith(thread_prefix):
            continue
        m = re.search(r"(\d+) samples, (\d+)% waiting", th)
        if not m:
            continue
        running = int(m.group(1)) * (100 - int(m.group(2))) / 100
        running_total += running
        for pct, addr in re.findall(r"([\d.]+)%  lr image\+0x([0-9a-f]+)", th):
            hits[re.sub(r"\(.*", "", sym(int(addr, 16)))[:110]] += float(pct) * running / 100
covered = sum(hits.values())
print(f"running samples {running_total:.0f}, covered by lr list {covered:.0f}")
for name, value in hits.most_common(int(sys.argv[5]) if len(sys.argv) > 5 else 40):
    print(f"{100 * value / covered:5.1f}%  {name}")
