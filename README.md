# SystemC_Accelerator_Model

A **C++17 SystemC TLM-2.0 model of an FHE accelerator tile**: an issuer with a window of
HE operations in flight, a DMA engine moving keys, plaintexts and ciphertexts between
HBM and the scratchpad, and NTT, MAC and automorphism units. It is driven by
[FHE_Accelerator_Sim](https://github.com/BrendanJamesLynskey/FHE_Accelerator_Sim)'s
own trace format, written in two TLM coding styles (approximately timed and loosely
timed), and checked against the SimPy model on the same traces.

It is the companion code for deck 03 (C++ performance models with SystemC TLM-2.0) of
the [Simulation Engineering Toolkit](https://github.com/BrendanJamesLynskey/SimEng_Hub_Toolkit)
series.

* **The same answers as the SimPy model, op by op.** The cost model and scratchpad
  planner are line-for-line ports, with the same floating-point order. With an explicit
  tie-break, the approximately-timed (AT) model reproduces SimPy's end time for every one
  of the 203–812 HE operations in seven of eight cases, to 7 fs (time resolution is 1 fs).
  In the eighth (1 MiB HBM chunks), 19 operations are ordered differently at exact ties;
  the horizon still agrees to 10⁻¹³.
* **Ties are part of the model.** IEEE 1666 does not specify the order in which processes
  runnable at the same instant execute. Leaving "first come" to the kernel reorders
  6–24 operations per run. An arbiter that waits for the instant to settle and grants the
  oldest operation's request makes the result independent of the kernel.
* **LT against AT, measured.** Four bootstraps: AT takes 330 ms, LT about 45 ms. With a global
  quantum of 1 µs or less LT gives the same horizon; at 10 µs it is 0.8% late, at 1 ms
  3.6% late, because processes book units and HBM ahead of simulated time.
* **GoogleTest, ASan and UBSan clean.** 16 tests, Release and sanitizer builds, in
  GitHub Actions and Jenkins.

All numbers below come from [`examples/results.md`](examples/results.md). The hardware
coefficients are FHE_Accelerator_Sim's, and they are illustrative.

---

## Quick start

```bash
# SystemC 3.x from source, no root needed (about 5 minutes)
curl -sL https://github.com/accellera-official/systemc/archive/refs/tags/3.0.2.tar.gz | tar xz
cmake -S systemc-3.0.2 -B sc-build -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/systemc
cmake --build sc-build -j2 && cmake --install sc-build
export LD_LIBRARY_PATH=$HOME/.local/opt/systemc/lib

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/.local/opt/systemc
cmake --build build -j2
./build/accel_tests                                    # GoogleTest
./build/accel_sim --trace tests/data/boot_ark/trace.json --hw tests/data/boot_ark/hw.json            # AT
./build/accel_sim --trace tests/data/boot_ark/trace.json --hw tests/data/boot_ark/hw.json \
                  --style lt --quantum-ns 10000                                                     # LT

cmake -S . -B build-asan -DACCEL_SANITIZE=ON -DCMAKE_PREFIX_PATH=$HOME/.local/opt/systemc && cmake --build build-asan -j2
pip install "fhe-sim @ git+https://github.com/BrendanJamesLynskey/FHE_Accelerator_Sim"
python tools/export_case.py mycase --hw small --opts min_ks,seeded_keys     # any trace + SimPy's answers
python examples/results.py                                                  # regenerates examples/results.md
```

## Structure

```
Tile (initiator socket) ──TLM-2.0──> Hbm (target socket)
  issuer: window of W HE ops                AT: nb_transport, BEGIN_REQ/END_REQ/BEGIN_RESP,
  per op: prefetch keys/plaintexts,             one server thread per request, Arbiter-granted
          write back spills, load inputs,   LT: b_transport, "busy until" booking, returns
          kernels on NTT / MAC / AUTO units      the finish time as the annotated delay
Arbiter: capacity-one resources, explicit tie-break
```

| File | What |
|------|------|
| [`include/accel/model.hpp`](include/accel/model.hpp), [`src/model.cpp`](src/model.cpp) | Trace and hardware loading, the cost model, the LRU scratchpad planner: no SystemC |
| [`include/accel/tile.hpp`](include/accel/tile.hpp), [`src/tile.cpp`](src/tile.cpp) | The SystemC modules: `Tile`, `Hbm`, `Arbiter`, `Gate`; AT and LT |
| [`src/main.cpp`](src/main.cpp) | `accel_sim`: run a trace, print JSON |
| [`tools/export_case.py`](tools/export_case.py) | Export a trace, the hardware, and the SimPy model's answers |
| [`tests/`](tests) | GoogleTest: planner bytes exact, unit busy times, AT agreement, LT invariants |

The SystemC model covers digital designs (no optical engine) in worst-case clocking mode
(FHE_Accelerator_Sim's fixed TDP clock), not the dynamic power manager.

## Selected results

| case | HE ops | SimPy horizon | SystemC, relative difference | ops differing by > 1 ps |
|---|---|---|---|---|
| bootstrap, ARK-class | 203 | 13.9354 ms | +6.4e-15 | 0 |
| bootstrap + Min-KS/seeded/OTF, small digital | 233 | 26.6480 ms | +8.9e-16 | 0 |
| bootstrap, ARK-class, 1 MiB chunks | 203 | 13.9734 ms | +9.8e-14 | 19 |
| 4 bootstraps, ARK-class | 812 | 55.7134 ms | +1.2e-13 | 0 |

| style | global quantum | wall-clock | horizon vs SimPy |
|---|---|---|---|
| AT | - | 330 ms | +1.3e-13 |
| LT | 1 µs | 45 ms | +1.3e-13 |
| LT | 10 µs | 45 ms | +8.0e-03 |
| LT | 1 ms | 43 ms | +3.6e-02 |

SimPy takes 204 ms on the same four bootstraps. Here AT is slower than SimPy: it pays
for a thread per HBM request and for the arbiter's settling deltas, and the workload is
coarse (a few thousand transactions). LT is about 4.5× faster than SimPy (wall-clock times are medians of five runs and vary by a few per cent between recordings).

## Why not the same order as SimPy everywhere?

Costs, scratchpad decisions and bytes are identical by construction, so any difference
is in the order of simultaneous requests. "Oldest operation first" matches SimPy's order
in all but one of the cases above. SimPy itself orders by when each event was scheduled.
An attempt to emulate that ordering in SystemC made agreement worse and was dropped; see
the notes in deck 03.

## CI

* **GitHub Actions** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): SystemC built
  from source (cached), Release and ASan+UBSan builds, GoogleTest, and an agreement job that
  exports fresh cases from the current FHE_Accelerator_Sim.
* **Jenkins** ([`Jenkinsfile`](Jenkinsfile)): build, GoogleTest with JUnit XML, sanitizers,
  agreement ([`ci/agreement.py`](ci/agreement.py)), a wall-clock gate against
  [`ci/perf_baseline.json`](ci/perf_baseline.json), `results.md`, and a nightly LT quantum sweep.

## Related

* [FHE_Accelerator_Sim](https://github.com/BrendanJamesLynskey/FHE_Accelerator_Sim): the SimPy model and the trace format.
* [Interview_CPP](https://github.com/BrendanJamesLynskey/Interview_CPP): C++ interview questions.

## Licence

MIT. SystemC is Apache-2.0 (Accellera); nlohmann/json and GoogleTest are fetched at build time under their own licences.
