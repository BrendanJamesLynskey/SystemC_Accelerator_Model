"""Export a case for the SystemC model: the trace, the hardware, and the SimPy model's answers.

    python tools/export_case.py OUTDIR [--params ark] [--hw ark] [--op hmult|hrot|bootstrap]
                                [--opts min_ks,seeded_keys,...] [--chunk-mib 4]

writes OUTDIR/trace.json (FHE_Accelerator_Sim's own format, from dump_trace),
OUTDIR/hw.json (the hardware parameters the timing depends on, plus the scratchpad
capacity and the clock, which depend on the parameter set and the TDP), and
OUTDIR/expected.json (horizon, unit busy times, bytes and per-op times from the
SimPy model in worst-case clocking mode, the mode the SystemC model implements).
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from fhe_sim import ACCELERATORS, PARAMS, BootOptions, SimConfig, bootstrap_trace, he_op_trace, simulate
from fhe_sim.params import MiB
from fhe_sim.workload import dump_trace


def export(out: Path, params: str = "ark", hw_name: str = "ark", op: str = "bootstrap", opts: str = "",
           chunk_mib: int = 4, n_boot: int = 1) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    p = PARAMS[params]
    hw = ACCELERATORS[hw_name].with_(power_mode="worst-case", hbm_chunk_mib=chunk_mib)
    if hw.optical is not None:
        raise SystemExit("the SystemC model covers digital designs only")
    o = BootOptions(n_boot=n_boot, **{k: True for k in opts.split(",") if k})
    trace = bootstrap_trace(p, o) if op == "bootstrap" else he_op_trace(p, op)
    dump_trace(trace, out / "trace.json")
    clock = hw.tdp_clock()
    hwj = {"freq_ghz": hw.freq_ghz, "ntt_bfly_per_cycle": hw.ntt_bfly_per_cycle, "mac_lanes": hw.mac_lanes,
           "auto_words_per_cycle": hw.auto_words_per_cycle, "sram_gbps": hw.sram_gbps, "hbm_gbps": hw.hbm_gbps,
           "hbm_chunk_bytes": hw.hbm_chunk_mib * MiB, "window": hw.window, "clock": clock,
           "spad_capacity": hw.sram_bytes - p.ks_working_set()}
    (out / "hw.json").write_text(json.dumps(hwj, indent=1))
    t0 = time.perf_counter()
    res = simulate(trace, SimConfig(hw))
    wall = time.perf_counter() - t0
    st = res.stats
    exp = {"horizon_s": res.horizon, "clock": res.clock, "simpy_wall_s": wall,
           "busy": {"ntt": st.busy["ntt"], "mac": st.busy["mac"], "auto": st.busy["auto"], "hbm": st.busy["hbm"]},
           "bytes": st.bytes, "op_start": res.op_start, "op_end": res.op_end,
           "case": {"params": params, "hw": hw_name, "op": op, "opts": opts, "chunk_mib": chunk_mib, "n_boot": n_boot}}
    (out / "expected.json").write_text(json.dumps(exp, indent=1))
    return exp


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--params", default="ark")
    ap.add_argument("--hw", default="ark")
    ap.add_argument("--op", default="bootstrap")
    ap.add_argument("--opts", default="")
    ap.add_argument("--chunk-mib", type=int, default=4)
    ap.add_argument("--n-boot", type=int, default=1)
    a = ap.parse_args()
    e = export(a.out, a.params, a.hw, a.op, a.opts, a.chunk_mib, a.n_boot)
    print(f"{a.out}: {len(e['op_end'])} ops, SimPy horizon {e['horizon_s'] * 1e3:.4f} ms")
