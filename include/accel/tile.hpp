// A SystemC TLM-2.0 model of one FHE accelerator tile: an issuer with a window of
// HE operations in flight, a DMA engine that moves keys, plaintexts and ciphertexts
// between HBM and the scratchpad, and NTT, MAC and automorphism units.
//
//   Tile (initiator) ──TLM-2.0 socket──> Hbm (target)
//
// Two coding styles of the same model:
//
// * Approximately timed (AT): every HBM chunk is a non-blocking transaction with the
//   four-phase base protocol (BEGIN_REQ, END_REQ, BEGIN_RESP, END_RESP); the HBM target
//   serves requests in arrival order. Units are FIFO resources. Every process stays in
//   step with simulated time.
// * Loosely timed (LT): every HBM chunk is a blocking b_transport call that returns
//   the time it would finish; units are booked by "busy until" times; each process
//   runs ahead of simulated time by up to a global quantum (temporal decoupling) and
//   synchronises only when it must wait for another process. Larger quanta mean fewer
//   context switches and resources booked out of time order.
#pragma once

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include <systemc>
#include <tlm>
#include <tlm_utils/peq_with_get.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/tlm_quantumkeeper.h>

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "accel/model.hpp"

namespace accel {

enum class Style { AT, LT };

// How simultaneous requests for a resource are ordered.
//
// IEEE 1666 does not specify the order in which processes that become runnable at the
// same time execute, so "first come, first served" is ambiguous when two requests arrive
// at the same instant. Two policies:
//
// * Kernel:  whichever process the kernel happens to run first gets a free resource; the
//            rest queue in arrival order (a plain FIFO resource). Implementation-defined.
// * Oldest:  wait until all activity at the instant has settled
//            (sc_pending_activity_at_current_time), then grant the request with the
//            smallest (request time, HE op id): among simultaneous requests, the oldest
//            operation wins. Explicit and kernel-independent; the default.
enum class TieBreak { Kernel, Oldest };

class Arbiter : public sc_core::sc_module {
public:
    Arbiter(sc_core::sc_module_name name, int resources, TieBreak policy = TieBreak::Oldest);
    void request(int res, int op);      // blocks the calling thread until granted
    void release(int res);
    std::uint64_t grants() const { return grants_; }

private:
    struct Key {
        sc_core::sc_time t;
        int op;
        std::uint64_t seq;
        bool operator<(const Key& o) const {
            if (t != o.t) return t < o.t;
            if (op != o.op) return op < o.op;
            return seq < o.seq;
        }
    };
    struct Res {
        bool busy = false;
        std::map<Key, sc_core::sc_event*> pending;
    };
    std::vector<Res> res_;
    TieBreak policy_;
    sc_core::sc_event kick_;
    std::uint64_t seq_ = 0, grants_ = 0;
    void run();
};

// The requesting HE operation, carried with each HBM transaction for arbitration.
struct Requester : tlm::tlm_extension<Requester> {
    int op = 0;
    tlm::tlm_extension_base* clone() const override { return new Requester(*this); }
    void copy_from(const tlm::tlm_extension_base& e) override { op = static_cast<const Requester&>(e).op; }
};

inline constexpr int kHbmResource = kUnits;

// A one-shot condition (an op finished, a prefetch or write-back finished).
class Gate {
public:
    void wait();                        // returns at once if already open
    void open();
    bool is_open() const { return open_; }

private:
    bool open_ = false;
    std::deque<sc_core::sc_event*> waiters_;
};

// HBM: one channel group with the accelerator's aggregate bandwidth, first come first served.
class Hbm : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<Hbm> socket;

    Hbm(sc_core::sc_module_name name, double gbps, Arbiter* arbiter);
    std::uint64_t transactions() const { return transactions_; }

private:
    double bw_;                                    // bytes per second
    Arbiter* arb_;
    std::uint64_t transactions_ = 0;
    // LT: the time the channel is booked until
    sc_core::sc_time busy_until_ = sc_core::SC_ZERO_TIME;

    void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay);
    tlm::tlm_sync_enum nb_transport_fw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase,
                                       sc_core::sc_time& delay);
    void serve(tlm::tlm_generic_payload* trans);   // AT: one thread per accepted request
};

struct TileStats {
    std::array<double, kUnits> busy{};             // seconds each unit was occupied
    double hbm_busy = 0;
    std::map<std::string, std::int64_t> bytes{{"key", 0}, {"pt", 0}, {"ct_read", 0}, {"ct_write", 0}};
    std::vector<double> op_start, op_end;
    double horizon = 0;
};

class Tile : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<Tile> socket;

    Tile(sc_core::sc_module_name name, const Trace& trace, const Hardware& hw, Style style, Arbiter* arbiter);
    const TileStats& stats() const { return st_; }

private:
    const Trace& t_;
    Hardware hw_;
    Style style_;
    Planner planner_;
    TileStats st_;
    Arbiter* arb_;
    std::array<sc_core::sc_time, kUnits> unit_busy_until_{};      // LT booking
    std::vector<std::unique_ptr<Gate>> done_;                  // one per HE op
    std::unordered_map<std::string, std::shared_ptr<Gate>> wb_;  // pending write-backs by object
    std::unordered_map<tlm::tlm_generic_payload*, sc_core::sc_event*> pending_;
    std::vector<unsigned char> dummy_;           // payload data: timing-only, the target never reads it
    int inflight_ = 0;
    sc_core::sc_event slot_ev_;

    using Qk = tlm_utils::tlm_quantumkeeper;
    void issuer();
    void run_op(const HeOp& o, OpPlan pl);
    void prefetch(OpPlan pl, int op, std::shared_ptr<Gate> finished);
    void writeback(std::string name, std::int64_t size, int op, std::shared_ptr<Gate> finished);
    void wait_done(int id, Qk& qk);
    void xfer(std::int64_t nbytes, const std::string& cls, int op, Qk& qk);
    void run_segment(const Segment& seg, int op, Qk& qk);
    double now(Qk& qk) const;
    void note_end(double t);
    tlm::tlm_sync_enum nb_transport_bw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase,
                                       sc_core::sc_time& delay);
};

struct RunResult {
    TileStats stats;
    std::uint64_t hbm_transactions = 0;
    double wall_s = 0;
    std::uint64_t delta_cycles = 0;
    std::uint64_t grants = 0;
};

// Elaborate a tile and HBM, run to completion, and return the statistics.
// quantum: the LT global quantum (ignored for AT).
RunResult run(const Trace& trace, const Hardware& hw, Style style, sc_core::sc_time quantum,
              TieBreak tiebreak = TieBreak::Oldest);

}  // namespace accel
