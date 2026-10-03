#include "accel/model.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace accel {

const char* unit_name(Unit u) {
    switch (u) {
        case Unit::Ntt: return "ntt";
        case Unit::Mac: return "mac";
        case Unit::Auto: return "auto";
    }
    return "?";
}

double Hardware::rate(Unit u) const {
    // hardware.py: per_cycle * self.freq_ghz * 1e9, evaluated left to right
    double per_cycle = u == Unit::Ntt ? ntt_bfly_per_cycle : u == Unit::Mac ? mac_lanes : auto_words_per_cycle;
    return per_cycle * freq_ghz * 1e9;
}

static nlohmann::json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return nlohmann::json::parse(f);
}

Trace load_trace(const std::string& path) {
    auto doc = read_json(path);
    if (doc.value("format", "") != "fhe-sim-trace/1") throw std::runtime_error(path + ": not an fhe-sim trace");
    Trace t;
    t.log_n = doc["params"]["log_n"].get<int>();
    for (auto& [name, size] : doc["sizes"].items()) t.sizes[name] = size.get<std::int64_t>();
    for (auto& o : doc["ops"]) {
        HeOp op;
        op.id = o["id"];
        op.op = o["op"];
        op.stage = o["stage"];
        op.level = o["level"];
        op.inputs = o["inputs"].get<std::vector<std::string>>();
        op.output = o["output"];
        if (!o["key"].is_null()) op.key = std::make_pair(o["key"][0].get<std::string>(), o["key"][1].get<std::int64_t>());
        for (auto& p : o["pts"]) op.pts.emplace_back(p[0].get<std::string>(), p[1].get<std::int64_t>());
        for (auto& k : o["kernels"])
            op.kernels.push_back({k[0].get<std::string>(), k[1].get<std::int64_t>(), k[2].get<std::int64_t>()});
        t.ops.push_back(std::move(op));
    }
    return t;
}

Hardware load_hardware(const std::string& path) {
    auto j = read_json(path);
    Hardware h;
    h.freq_ghz = j.at("freq_ghz");
    h.ntt_bfly_per_cycle = j.at("ntt_bfly_per_cycle");
    h.mac_lanes = j.at("mac_lanes");
    h.auto_words_per_cycle = j.at("auto_words_per_cycle");
    h.sram_gbps = j.at("sram_gbps");
    h.hbm_gbps = j.at("hbm_gbps");
    h.hbm_chunk_bytes = j.at("hbm_chunk_bytes");
    h.window = j.at("window");
    h.clock = j.at("clock");
    h.spad_capacity = j.at("spad_capacity");
    return h;
}

Unit unit_of(const std::string& kind) {
    if (kind == "ntt" || kind == "intt") return Unit::Ntt;
    if (kind == "bconv" || kind == "mac") return Unit::Mac;
    if (kind == "auto") return Unit::Auto;
    throw std::runtime_error("unknown kernel kind " + kind);
}

Segment segment(const Hardware& hw, int log_n, const Kernel& k) {
    // hardware.py CostModel._seg and segments() (digital units only)
    Unit u = unit_of(k.kind);
    std::int64_t N = std::int64_t{1} << log_n;
    std::int64_t work_i = u == Unit::Ntt ? k.amount * (N / 2) * log_n : k.amount;
    double work = static_cast<double>(work_i);
    double s = hw.clock;
    double t_logic = work / (hw.rate(u) * s);
    double t_sram = static_cast<double>(k.words * 8) / (hw.sram_gbps * 1e9 * s);
    return {u, t_logic >= t_sram ? t_logic : t_sram, work};
}

// ── Scratchpad ───────────────────────────────────────────────────────────────
bool Scratchpad::hit(const std::string& name, std::int64_t size) {
    auto it = items_.find(name);
    if (it == items_.end() || it->second.size < size) return false;
    order_.splice(order_.end(), order_, it->second.pos);      // move to most recently used
    return true;
}

void Scratchpad::free(const std::string& name) {
    auto it = items_.find(name);
    if (it == items_.end()) return;
    used_ -= it->second.size;
    order_.erase(it->second.pos);
    items_.erase(it);
}

bool Scratchpad::alloc(const std::string& name, std::int64_t size, const std::string& cls, bool dirty,
                       const std::set<std::string>& pinned, std::vector<Evicted>& evicted) {
    free(name);
    if (size > cap_) return false;
    while (used_ + size > cap_) {
        auto v = order_.begin();
        while (v != order_.end() && pinned.count(*v)) ++v;
        if (v == order_.end()) return false;
        std::string victim = *v;
        Item it = items_.at(victim);
        free(victim);
        evicted.push_back({victim, it.size, it.cls, it.dirty});
    }
    order_.push_back(name);
    items_[name] = {std::prev(order_.end()), size, cls, dirty};
    used_ += size;
    return true;
}

// ── Planner ──────────────────────────────────────────────────────────────────
Planner::Planner(const Trace& t, const Hardware& hw) : t_(t), spad_(hw.spad_capacity) {
    for (const auto& o : t.ops) producer_[o.output] = o.id;
    for (const auto& o : t.ops)
        for (const auto& x : o.inputs) last_use_[x] = o.id;
}

OpPlan Planner::plan(const HeOp& o) {
    OpPlan pl;
    std::set<std::string> pinned(o.inputs.begin(), o.inputs.end());
    pinned.insert(o.output);
    if (o.key) pinned.insert(o.key->first);
    for (const auto& p : o.pts) pinned.insert(p.first);
    std::vector<Scratchpad::Evicted> ev;
    for (const auto& x : o.inputs) {
        std::int64_t sz = t_.sizes.at(x);
        if (!spad_.hit(x, sz)) {
            pl.ct_load.emplace_back(x, sz);
            spad_.alloc(x, sz, "ct", false, pinned, ev);
        }
    }
    if (o.key) {
        const auto& [kid, kb] = *o.key;
        if (!spad_.hit(kid, kb)) {
            pl.key_load += kb;
            spad_.alloc(kid, kb, "key", false, pinned, ev);
        }
    }
    for (const auto& [pid, pb] : o.pts) {
        if (!spad_.hit(pid, pb)) {
            pl.pt_load += pb;
            spad_.alloc(pid, pb, "pt", false, pinned, ev);
        }
    }
    std::int64_t out_size = t_.sizes.at(o.output);
    if (!last_use_.count(o.output))
        pl.out_write = out_size;                       // final result: straight to HBM
    else if (!spad_.alloc(o.output, out_size, "ct", true, pinned, ev))
        pl.out_write = out_size;
    for (const auto& e : ev) {
        auto lu = last_use_.find(e.name);
        if (e.dirty && lu != last_use_.end() && lu->second > o.id) pl.writebacks.emplace_back(e.name, e.size);
    }
    for (const auto& x : o.inputs)
        if (last_use_.at(x) == o.id) spad_.free(x);
    return pl;
}

}  // namespace accel
