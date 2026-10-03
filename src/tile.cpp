#include "accel/tile.hpp"

#include <algorithm>
#include <chrono>

using namespace sc_core;

namespace accel {

static sc_time seconds(double s) { return sc_time(s, SC_SEC); }

// ── Arbiter ──────────────────────────────────────────────────────────────────
Arbiter::Arbiter(sc_module_name name, int resources, TieBreak policy)
    : sc_module(name), res_(resources), policy_(policy) {
    if (policy_ == TieBreak::Oldest) SC_THREAD(run);
}

void Arbiter::request(int res, int op) {
    Res& r = res_[res];
    ++grants_;
    sc_event granted;
    if (policy_ == TieBreak::Kernel) {
        if (!r.busy && r.pending.empty()) {         // free: the first process to ask takes it
            r.busy = true;
            return;
        }
        r.pending[{SC_ZERO_TIME, 0, seq_++}] = &granted;   // otherwise queue in arrival order
    } else {
        r.pending[{sc_time_stamp(), op, seq_++}] = &granted;
        kick_.notify(SC_ZERO_TIME);
    }
    wait(granted);
}

void Arbiter::release(int res) {
    Res& r = res_[res];
    if (policy_ == TieBreak::Kernel) {
        if (r.pending.empty()) {
            r.busy = false;
        } else {                                    // hand over to the next in line
            auto next = r.pending.begin();
            next->second->notify(SC_ZERO_TIME);
            r.pending.erase(next);
        }
        return;
    }
    r.busy = false;
    kick_.notify(SC_ZERO_TIME);
}

void Arbiter::run() {
    for (;;) {
        wait(kick_);
        // let every process that can still act at this instant do so, then decide
        while (sc_pending_activity_at_current_time()) wait(SC_ZERO_TIME);
        for (auto& r : res_) {
            if (r.busy || r.pending.empty()) continue;
            auto first = r.pending.begin();
            r.busy = true;
            first->second->notify();
            r.pending.erase(first);
        }
    }
}

// ── Gate ─────────────────────────────────────────────────────────────────────
void Gate::wait() {
    if (open_) return;
    sc_event resumed;
    waiters_.push_back(&resumed);
    sc_core::wait(resumed);
}

void Gate::open() {
    open_ = true;
    for (sc_event* e : waiters_) e->notify();
    waiters_.clear();
}

// ── Hbm ──────────────────────────────────────────────────────────────────────
Hbm::Hbm(sc_module_name name, double gbps, Arbiter* arbiter)
    : sc_module(name), socket("socket"), bw_(gbps * 1e9), arb_(arbiter) {
    socket.register_b_transport(this, &Hbm::b_transport);
    socket.register_nb_transport_fw(this, &Hbm::nb_transport_fw);
}

void Hbm::b_transport(tlm::tlm_generic_payload& trans, sc_time& delay) {
    // LT: book the channel from the initiator's local time (now + delay), in call order
    double dt = static_cast<double>(trans.get_data_length()) / bw_;
    sc_time start = std::max(busy_until_, sc_time_stamp() + delay);
    busy_until_ = start + seconds(dt);
    delay = busy_until_ - sc_time_stamp();
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    ++transactions_;
}

tlm::tlm_sync_enum Hbm::nb_transport_fw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time&) {
    if (phase == tlm::BEGIN_REQ) {
        // accept at once (END_REQ); a thread per request waits for the channel
        sc_spawn([this, &trans]() { serve(&trans); });
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    if (phase == tlm::END_RESP) return tlm::TLM_COMPLETED;
    SC_REPORT_ERROR("Hbm", "unexpected phase");
    return tlm::TLM_COMPLETED;
}

void Hbm::serve(tlm::tlm_generic_payload* trans) {
    Requester* who = nullptr;
    trans->get_extension(who);
    arb_->request(kHbmResource, who ? who->op : 0);
    wait(seconds(static_cast<double>(trans->get_data_length()) / bw_));
    arb_->release(kHbmResource);
    trans->set_response_status(tlm::TLM_OK_RESPONSE);
    ++transactions_;
    tlm::tlm_phase phase = tlm::BEGIN_RESP;
    sc_time delay = SC_ZERO_TIME;
    socket->nb_transport_bw(*trans, phase, delay);       // the initiator completes it (END_RESP implied)
}

// ── Tile ─────────────────────────────────────────────────────────────────────
Tile::Tile(sc_module_name name, const Trace& trace, const Hardware& hw, Style style, Arbiter* arbiter)
    : sc_module(name), socket("socket"), t_(trace), hw_(hw), style_(style), planner_(trace, hw), arb_(arbiter),
      dummy_(static_cast<std::size_t>(hw.hbm_chunk_bytes)) {
    socket.register_nb_transport_bw(this, &Tile::nb_transport_bw);
    std::size_t n = trace.ops.size();
    for (std::size_t i = 0; i < n; ++i) done_.push_back(std::make_unique<Gate>());
    st_.op_start.assign(n, 0.0);
    st_.op_end.assign(n, 0.0);
    SC_THREAD(issuer);
}

double Tile::now(Qk& qk) const {
    sc_time t = sc_time_stamp();
    if (style_ == Style::LT) t += qk.get_local_time();
    return t.to_seconds();
}

void Tile::note_end(double t) { st_.horizon = std::max(st_.horizon, t); }

tlm::tlm_sync_enum Tile::nb_transport_bw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time&) {
    if (phase != tlm::BEGIN_RESP) SC_REPORT_ERROR("Tile", "unexpected phase");
    pending_.at(&trans)->notify();
    return tlm::TLM_COMPLETED;
}

void Tile::issuer() {
    for (const auto& o : t_.ops) {
        while (inflight_ >= hw_.window) wait(slot_ev_);
        OpPlan pl = planner_.plan(o);
        ++inflight_;
        sc_spawn([this, &o, pl]() { run_op(o, pl); });
    }
}

void Tile::wait_done(int id, Qk& qk) {
    if (!done_[id]->is_open()) {
        qk.sync();                      // LT: catch up with simulated time before blocking
        done_[id]->wait();
    }
}

void Tile::xfer(std::int64_t nbytes, const std::string& cls, int op, Qk& qk) {
    std::int64_t left = nbytes;
    const double bw = hw_.hbm_gbps * 1e9;
    while (left > 0) {
        std::int64_t sz = left < hw_.hbm_chunk_bytes ? left : hw_.hbm_chunk_bytes;
        tlm::tlm_generic_payload trans;
        trans.set_command(cls == "ct_write" ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        trans.set_address(0);
        trans.set_data_ptr(dummy_.data());
        trans.set_data_length(static_cast<unsigned>(sz));
        trans.set_streaming_width(static_cast<unsigned>(sz));
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        Requester who;
        who.op = op;
        trans.set_extension(&who);
        if (style_ == Style::AT) {
            sc_event done;
            pending_[&trans] = &done;
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = SC_ZERO_TIME;
            socket->nb_transport_fw(trans, phase, delay);
            wait(done);
            pending_.erase(&trans);
        } else {
            sc_time delay = qk.get_local_time();
            socket->b_transport(trans, delay);
            qk.set(delay);
            if (qk.need_sync()) qk.sync();
        }
        trans.clear_extension(&who);
        st_.hbm_busy += static_cast<double>(sz) / bw;
        note_end(now(qk));
        left -= sz;
    }
    st_.bytes[cls] += nbytes;
}

void Tile::run_segment(const Segment& seg, int op, Qk& qk) {
    int u = static_cast<int>(seg.unit);
    if (style_ == Style::AT) {
        arb_->request(u, op);
        wait(seconds(seg.time));
        arb_->release(u);
    } else {
        sc_time start = std::max(unit_busy_until_[u], sc_time_stamp() + qk.get_local_time());
        unit_busy_until_[u] = start + seconds(seg.time);
        qk.set(unit_busy_until_[u] - sc_time_stamp());
        if (qk.need_sync()) qk.sync();
    }
    st_.busy[u] += seg.time;
    note_end(now(qk));
}

void Tile::prefetch(OpPlan pl, int op, std::shared_ptr<Gate> finished) {
    Qk qk;
    qk.reset();
    if (pl.key_load) xfer(pl.key_load, "key", op, qk);
    if (pl.pt_load) xfer(pl.pt_load, "pt", op, qk);
    qk.sync();                          // finish at the process's local time
    finished->open();
}

void Tile::writeback(std::string name, std::int64_t size, int op, std::shared_ptr<Gate> finished) {
    Qk qk;
    qk.reset();
    auto p = planner_.producer().find(name);
    if (p != planner_.producer().end()) wait_done(p->second, qk);
    xfer(size, "ct_write", op, qk);
    qk.sync();
    finished->open();
}

void Tile::run_op(const HeOp& o, OpPlan pl) {
    Qk qk;
    qk.reset();
    std::shared_ptr<Gate> pf;
    if (pl.key_load || pl.pt_load) {
        pf = std::make_shared<Gate>();
        int id = o.id;
        sc_spawn([this, pl, id, pf]() { prefetch(pl, id, pf); });
    }
    for (const auto& [name, size] : pl.writebacks) {
        std::string n = name;
        std::int64_t s = size;
        auto g = std::make_shared<Gate>();
        wb_[n] = g;
        int id = o.id;
        sc_spawn([this, n, s, id, g]() { writeback(n, s, id, g); });
    }
    for (const auto& x : o.inputs) {
        auto p = planner_.producer().find(x);
        if (p != planner_.producer().end()) wait_done(p->second, qk);
    }
    for (const auto& [x, size] : pl.ct_load) {
        auto w = wb_.find(x);
        if (w != wb_.end() && !w->second->is_open()) {
            qk.sync();
            w->second->wait();
        }
        xfer(size, "ct_read", o.id, qk);
    }
    if (pf && !pf->is_open()) {
        qk.sync();
        pf->wait();
    }
    st_.op_start[o.id] = now(qk);
    for (const auto& k : o.kernels) run_segment(segment(hw_, t_.log_n, k), o.id, qk);
    if (pl.out_write) xfer(pl.out_write, "ct_write", o.id, qk);
    st_.op_end[o.id] = now(qk);
    note_end(st_.op_end[o.id]);
    qk.sync();
    done_[o.id]->open();
    --inflight_;
    slot_ev_.notify(SC_ZERO_TIME);
}

// ── run ──────────────────────────────────────────────────────────────────────
RunResult run(const Trace& trace, const Hardware& hw, Style style, sc_time quantum, TieBreak tiebreak) {
    tlm::tlm_global_quantum::instance().set(quantum);
    // Modules live for the rest of the process: SystemC elaborates once per process.
    auto* arb = new Arbiter("arbiter", kUnits + 1, tiebreak);
    auto* tile = new Tile("tile", trace, hw, style, arb);
    auto* hbm = new Hbm("hbm", hw.hbm_gbps, arb);
    tile->socket.bind(hbm->socket);
    auto t0 = std::chrono::steady_clock::now();
    sc_start();
    RunResult r;
    r.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.stats = tile->stats();
    r.hbm_transactions = hbm->transactions();
    r.delta_cycles = sc_delta_count();
    r.grants = arb->grants();
    return r;
}

}  // namespace accel
