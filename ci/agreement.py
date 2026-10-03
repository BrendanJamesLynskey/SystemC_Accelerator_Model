"""Export fresh cases from the installed FHE_Accelerator_Sim and require op-by-op agreement (AT, oldest-first)."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from export_case import export  # noqa: E402

BIN = Path(__file__).resolve().parents[1] / "build" / "accel_sim"
ALL = "min_ks,seeded_keys,otf_plaintexts"
bad = 0
for kw in ({}, {"opts": ALL}, {"hw_name": "small"}, {"hw_name": "small", "opts": ALL}, {"n_boot": 2}):
    d = Path(tempfile.mkdtemp())
    e = export(d, **kw)
    subprocess.run([str(BIN), "--trace", str(d / "trace.json"), "--hw", str(d / "hw.json"), "--out", str(d / "r.json")],
                   check=True, capture_output=True)
    r = json.loads((d / "r.json").read_text())
    worst = max(abs(a - b) for a, b in zip(r["op_end"], e["op_end"], strict=True))
    ok = abs(r["horizon_s"] / e["horizon_s"] - 1) < 1e-12 and worst < 1e-12 and r["bytes"] == e["bytes"]
    bad += not ok
    print(f"{kw or 'baseline'}: horizon {e['horizon_s']:.9e} vs {r['horizon_s']:.9e}, worst op {worst:.1e} s: "
          f"{'ok' if ok else 'MISMATCH'}")
sys.exit(1 if bad else 0)
