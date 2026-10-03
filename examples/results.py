"""Every number quoted in the README and in deck SimEng 03.

    cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/systemc && cmake --build build -j2
    cmake -S . -B build-asan -DACCEL_SANITIZE=ON ... && cmake --build build-asan -j2    # optional
    python examples/results.py                      # writes examples/results.md

Cases are exported fresh from FHE_Accelerator_Sim (tools/export_case.py), so the
comparison is always against the current SimPy model. Wall-clock times are the median
of five runs on an otherwise idle machine.
"""

from __future__ import annotations

import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
from export_case import export  # noqa: E402

from fhe_sim import ACCELERATORS, PARAMS, BootOptions, SimConfig, bootstrap_trace, simulate  # noqa: E402

BIN = ROOT / "build" / "accel_sim"
TMP = Path(tempfile.mkdtemp(prefix="accel_results_"))
OUT: list[str] = []
REPS = 5


def h(title):
    OUT.append(f"\n## {title}\n")


def table(head, rows):
    OUT.append("| " + " | ".join(head) + " |")
    OUT.append("|" + "---|" * len(head))
    for r in rows:
        OUT.append("| " + " | ".join(str(x) for x in r) + " |")


def sim(case: Path, style="at", quantum_ns=0.0, tiebreak="oldest") -> dict:
    out = TMP / "r.json"
    subprocess.run([str(BIN), "--trace", str(case / "trace.json"), "--hw", str(case / "hw.json"), "--style", style,
                    "--quantum-ns", str(quantum_ns), "--tiebreak", tiebreak, "--out", str(out)],
                   check=True, capture_output=True)
    return json.loads(out.read_text())


def compare(r: dict, e: dict) -> dict:
    d = [abs(a - b) for a, b in zip(r["op_end"], e["op_end"], strict=True)]
    return {"rel": r["horizon_s"] / e["horizon_s"] - 1, "max_op": max(d), "n_ops": sum(x > 1e-12 for x in d),
            "bytes": r["bytes"] == e["bytes"],
            "busy": max(abs(r["busy"][u] - e["busy"][u]) / e["horizon_s"] for u in ("ntt", "mac", "auto", "hbm"))}


ALL = "min_ks,seeded_keys,otf_plaintexts"
CASES = [("one HMult (ark)", dict(op="hmult")), ("one HRot (ark)", dict(op="hrot")),
         ("bootstrap, ARK-class", {}), ("bootstrap + Min-KS/seeded/OTF, ARK-class", dict(opts=ALL)),
         ("bootstrap, small digital", dict(hw_name="small")),
         ("bootstrap + Min-KS/seeded/OTF, small digital", dict(hw_name="small", opts=ALL)),
         ("bootstrap, ARK-class, 1 MiB chunks", dict(chunk_mib=1)),
         ("4 bootstraps, ARK-class", dict(n_boot=4))]

# 1 ── agreement ────────────────────────────────────────────────────────
h("1. The AT model against the SimPy model on the same traces")
OUT.append("SimPy: FHE_Accelerator_Sim in worst-case clocking mode. SystemC: the approximately-timed model with the "
           "explicit tie-break (oldest operation first among simultaneous requests), time resolution 1 fs.\n")
rows = []
dirs = {}
for k, (name, kw) in enumerate(CASES):
    d = TMP / f"case{k}"
    e = export(d, **kw)
    dirs[name] = (d, e)
    c = compare(sim(d), e)
    rows.append([name, len(e["op_end"]), f"{1e3 * e['horizon_s']:.4f} ms", f"{c['rel']:+.1e}",
                 f"{c['max_op'] * 1e15:.0f} fs" if c["max_op"] < 1e-9 else f"{c['max_op'] * 1e6:.1f} us",
                 f"{c['n_ops']}", "yes" if c["bytes"] else "NO", f"{c['busy']:.0e}"])
table(["case", "HE ops", "SimPy horizon", "SystemC horizon, relative difference", "largest op-end difference",
       "ops differing by more than 1 ps", "HBM bytes identical", "unit busy-time difference / horizon"], rows)
OUT.append("\nCosts, scratchpad decisions and byte counts are identical by construction (the same formulas in the "
           "same floating-point order), so the only freedom left is the order of simultaneous requests. With 1 MiB "
           "chunks there are four times as many HBM transactions and more exact ties, and in some of them SimPy's "
           "order (its event-scheduling order) is not oldest-operation-first; those operations finish earlier or "
           "later while the horizon, busy times and bytes still agree.")

# 2 ── tie-breaks ─────────────────────────────────────────────────────
h("2. Tie-breaking: explicit against the kernel's process order")
OUT.append("`--tiebreak kernel` grants a free resource to whichever process asks first in the SystemC kernel's "
           "execution order, which IEEE 1666 leaves to the implementation (here Accellera SystemC "
           f"{os.environ.get('SYSTEMC_VERSION', '3.0.2')}). `--tiebreak oldest` waits until the instant has settled "
           "and grants the oldest operation's request.\n")
rows = []
for name in ("bootstrap, ARK-class", "bootstrap, small digital", "bootstrap + Min-KS/seeded/OTF, small digital",
             "4 bootstraps, ARK-class"):
    d, e = dirs[name]
    k, o = compare(sim(d, tiebreak="kernel"), e), compare(sim(d), e)
    rows.append([name, f"{k['n_ops']} of {len(e['op_end'])}", f"{k['rel']:+.1e}", f"{o['n_ops']}", f"{o['rel']:+.1e}"])
table(["case", "kernel order: ops that differ from SimPy", "horizon difference", "explicit: ops that differ",
       "horizon difference"], rows)

# 3 ── LT against AT ────────────────────────────────────────────────────
h("3. Loosely timed (temporal decoupling) against approximately timed: speed and accuracy")
d, e = dirs["4 bootstraps, ARK-class"]
trace_obj = bootstrap_trace(PARAMS["ark"], BootOptions(n_boot=4))
hw_obj = ACCELERATORS["ark"].with_(power_mode="worst-case")
simpy = []
for _ in range(REPS):
    t0 = time.perf_counter()
    simulate(trace_obj, SimConfig(hw_obj))
    simpy.append(time.perf_counter() - t0)
OUT.append(f"Workload: 4 bootstraps on the ARK-class design ({len(e['op_end'])} HE ops). SimPy: "
           f"{1e3 * statistics.median(simpy):.0f} ms of wall-clock time.\n")
rows = []
for style, q in [("AT", 0.0), ("LT", 0.0), ("LT", 100.0), ("LT", 1e3), ("LT", 1e4), ("LT", 1e5), ("LT", 1e6)]:
    walls, r = [], None
    for _ in range(REPS):
        r = sim(d, style.lower(), q)
        walls.append(r["wall_s"])
    c = compare(r, e)
    qlabel = "-" if style == "AT" else ("0" if q == 0 else f"{q / 1e3:g} us")
    rows.append([style, qlabel, f"{1e3 * statistics.median(walls):.1f} ms", f"{r['delta_cycles']:,}",
                 f"{1e3 * r['horizon_s']:.4f} ms", f"{c['rel']:+.2e}", f"{c['n_ops']}"])
table(["style", "global quantum", "wall-clock (median of 5)", "delta cycles", "horizon",
       "horizon vs SimPy", "ops that differ from SimPy"], rows)
OUT.append("\nLT books units and the HBM channel in the order processes call them; with a quantum, a process "
           "books resources up to a quantum ahead of simulated time, before others that would have asked earlier.")

# 4 ── tests ────────────────────────────────────────────────────────────
h("4. Test suites")
for label, b in (("Release", ROOT / "build" / "accel_tests"), ("ASan + UBSan", ROOT / "build-asan" / "accel_tests")):
    if not b.exists():
        OUT.append(f"* {label}: not built.")
        continue
    p = subprocess.run([str(b)], capture_output=True, text=True)
    m = re.search(r"\[==========\] (\d+) tests? from .* ran", p.stdout)
    passed = re.search(r"\[  PASSED  \] (\d+) tests?", p.stdout)
    OUT.append(f"* GoogleTest, {label}: {m[1] if m else '?'} tests, {passed[1] if passed else 0} passed"
               f"{'' if p.returncode == 0 else ' (FAILURES)'}.")
    assert p.returncode == 0, p.stdout[-3000:]

ver = subprocess.run([str(BIN), "--help"], capture_output=True, text=True).stderr
OUT.insert(0, f"# Results (generated by examples/results.py)\n\nRecorded on {platform.machine()} Linux, "
              f"{platform.processor() or 'x86_64'}; Accellera SystemC 3.0.2, g++ "
              f"{subprocess.run(['g++', '-dumpfullversion'], capture_output=True, text=True).stdout.strip()}. "
              "Every number in the README and in deck SimEng 03 comes from here.")
(HERE / "results.md").write_text("\n".join(OUT) + "\n")
print("\n".join(OUT))
