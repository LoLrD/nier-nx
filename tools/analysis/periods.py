import re
import sys

D = sys.argv[1]
prof = open(f"{D}/rex_profile.log", errors="replace").read().split("\n==== ")[1:]
renders = [l for l in open(f"{D}/nier.log", errors="replace").read().splitlines() if "[native] render:" in l]
for i, block in enumerate(prof):
    fps = float(re.search(r"game ([\d.]+) fps", block).group(1))
    cpu = {m.group(1).strip(): float(m.group(2)) for m in re.finditer(r'-- thread \d+ "([^"]*)": CPU ([\d.]+)%', block)}
    ring = next((v for k, v in cpu.items() if k.startswith("GPU ring")), 0)
    rec = next((v for k, v in cpu.items() if k.startswith("GPU record")), 0)
    xs = sorted((v for k, v in cpu.items() if k.startswith("XThread")), reverse=True)[:3]
    line = renders[i - 1] if 0 < i <= len(renders) else ""
    frames = fps * 10 or 1
    g = re.search(r"gpu ms draws (\d+), resolves (\d+), transfers (\d+), uploads (\d+), present (\d+)", line)
    gpu = sum(map(int, g.groups())) / frames if g else 0
    d = re.search(r"render: (\d+)/", line)
    p = re.search(r"pipelines compiled (\d+) in (\d+) ms \(max (\d+) ms\)", line)
    t = re.search(r"loads new (\d+) \((\d+) MB\), changed (\d+)", line)
    ms = re.search(r"ms draws (\d+) \(mirror (\d+), textures (\d+)\), resolves (\d+), present (\d+), gpu wait (\d+)", line)
    print(f"{i:2} fps {fps:5.1f} ring {ring:5.1f}% rec {rec:5.1f}% game {' '.join(f'{x:4.1f}' for x in xs)} | gpu {gpu:5.1f} ms/f | draws/f {int(d.group(1)) / frames if d else 0:5.0f} | pipe {p.groups() if p else '-'} | tex new {t.groups() if t else '-'} | ring ms {ms.groups() if ms else '-'}")
