"""Nightly: LT global quantum against wall-clock time and horizon error, to sweep.csv."""

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from export_case import export  # noqa: E402

d = Path(tempfile.mkdtemp())
e = export(d, n_boot=4)
with open("sweep.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["quantum_ns", "wall_s", "horizon_s", "relative_error"])
    for q in (0, 10, 100, 1e3, 3e3, 1e4, 3e4, 1e5, 3e5, 1e6):
        subprocess.run([str(ROOT / "build" / "accel_sim"), "--trace", str(d / "trace.json"), "--hw", str(d / "hw.json"),
                        "--style", "lt", "--quantum-ns", str(q), "--out", str(d / "r.json")], check=True,
                       capture_output=True)
        r = json.loads((d / "r.json").read_text())
        w.writerow([q, f"{r['wall_s']:.4f}", f"{r['horizon_s']:.9e}", f"{r['horizon_s'] / e['horizon_s'] - 1:.3e}"])
print(open("sweep.csv").read())
