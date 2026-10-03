// The parts of the accelerator model that do not need a simulation kernel:
// the FHE trace (FHE_Accelerator_Sim's "fhe-sim-trace/1" JSON), the hardware
// configuration, the cost model, and the scratchpad planner.
//
// Each is a line-for-line port of FHE_Accelerator_Sim (hardware.py: CostModel._seg
// and segments; sim.py: Scratchpad and Simulation.plan), with the same floating-point
// operation order, so that the SystemC model and the SimPy model cost every kernel and
// transfer identically and differ only in how they schedule them.
#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace accel {

enum class Unit { Ntt = 0, Mac = 1, Auto = 2 };
inline constexpr int kUnits = 3;
const char* unit_name(Unit u);

struct Kernel {
    std::string kind;      // ntt | intt | bconv | mac | auto
    std::int64_t amount;   // limbs (ntt/intt), multiply-adds or words
    std::int64_t words;    // on-chip words read and written
};

struct HeOp {
    int id = 0;
    std::string op, stage;
    int level = 0;
    std::vector<std::string> inputs;
    std::string output;
    std::optional<std::pair<std::string, std::int64_t>> key;    // (key id, bytes)
    std::vector<std::pair<std::string, std::int64_t>> pts;      // (plaintext id, bytes)
    std::vector<Kernel> kernels;
};

struct Trace {
    int log_n = 0;
    std::vector<HeOp> ops;
    std::unordered_map<std::string, std::int64_t> sizes;
};

// Hardware parameters the timing depends on (a digital design: no optical engine).
struct Hardware {
    double freq_ghz = 1.0;
    double ntt_bfly_per_cycle = 4096, mac_lanes = 8192, auto_words_per_cycle = 4096;
    double sram_gbps = 20000.0, hbm_gbps = 1000.0;
    std::int64_t hbm_chunk_bytes = 4LL << 20;
    int window = 4;
    double clock = 1.0;                 // clock fraction (worst-case TDP clocking)
    std::int64_t spad_capacity = 0;     // scratchpad bytes left after the key-switch reserve

    double rate(Unit u) const;
};

Trace load_trace(const std::string& path);
Hardware load_hardware(const std::string& path);

// One kernel's occupancy of one unit, as hardware.py's CostModel computes it.
struct Segment {
    Unit unit;
    double time;
    double work;
};
Unit unit_of(const std::string& kind);
Segment segment(const Hardware& hw, int log_n, const Kernel& k);

// ── the scratchpad planner (sim.py: Scratchpad and Simulation.plan) ──────────
struct OpPlan {
    std::int64_t key_load = 0, pt_load = 0, out_write = 0;
    std::vector<std::pair<std::string, std::int64_t>> ct_load, writebacks;
};

class Scratchpad {
public:
    explicit Scratchpad(std::int64_t capacity) : cap_(capacity) {}
    bool hit(const std::string& name, std::int64_t size);
    void free(const std::string& name);
    struct Evicted { std::string name; std::int64_t size; std::string cls; bool dirty; };
    bool alloc(const std::string& name, std::int64_t size, const std::string& cls, bool dirty,
               const std::set<std::string>& pinned, std::vector<Evicted>& evicted);
    std::int64_t used() const { return used_; }

private:
    struct Item { std::list<std::string>::iterator pos; std::int64_t size; std::string cls; bool dirty; };
    std::int64_t cap_, used_ = 0;
    std::list<std::string> order_;                  // least recently used first
    std::unordered_map<std::string, Item> items_;
};

class Planner {
public:
    Planner(const Trace& t, const Hardware& hw);
    OpPlan plan(const HeOp& o);                     // call in program order
    const std::unordered_map<std::string, int>& producer() const { return producer_; }

private:
    const Trace& t_;
    Scratchpad spad_;
    std::unordered_map<std::string, int> producer_, last_use_;
};

}  // namespace accel
