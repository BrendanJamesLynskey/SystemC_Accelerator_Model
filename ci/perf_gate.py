"""Wall-clock gate: AT and LT (10 us quantum) on four bootstraps must not slow down by more than
--margin against ci/perf_baseline.json (median of five runs). --bless writes the baseline."""

import argparse
import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from export_case import export  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--margin", type=float, default=0.5)
ap.add_argument("--bless", action="store_true")
a = ap.parse_args()
d = Path(tempfile.mkdtemp())
export(d, n_boot=4)
now = {}
for name, args in (("AT", ["--style", "at"]), ("LT 10us", ["--style", "lt", "--quantum-ns", "10000"])):
    walls = []
    for _ in range(5):
        subprocess.run([str(ROOT / "build" / "accel_sim"), "--trace", str(d / "trace.json"), "--hw", str(d / "hw.json"),
                        "--out", str(d / "r.json"), *args], check=True, capture_output=True)
        walls.append(json.loads((d / "r.json").read_text())["wall_s"])
    now[name] = statistics.median(walls)
base_file = ROOT / "ci" / "perf_baseline.json"
if a.bless:
    base_file.write_text(json.dumps(now, indent=1) + "\n")
    print("blessed", now)
    sys.exit(0)
base = json.loads(base_file.read_text())
lines = ["# SystemC model performance gate", "", "| run | baseline s | now s | verdict |", "|---|---|---|---|"]
bad = 0
for k, b in base.items():
    ok = now[k] <= (1 + a.margin) * b
    bad += not ok
    lines.append(f"| {k} | {b:.4f} | {now[k]:.4f} | {'ok' if ok else 'SLOWER'} (margin {a.margin:.0%}) |")
Path("perf_report.md").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(1 if bad else 0)
