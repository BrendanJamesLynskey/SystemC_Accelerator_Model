// accel_sim: run an FHE_Accelerator_Sim trace on the SystemC tile model.
//
//   accel_sim --trace trace.json --hw hw.json [--style at|lt] [--quantum-ns 1000]
//             [--tiebreak oldest|kernel] [--out result.json]
//
// trace.json is FHE_Accelerator_Sim's dump_trace() output; hw.json comes from
// tools/export_case.py. The result is printed (and written to --out) as JSON.
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "accel/tile.hpp"

int sc_main(int argc, char* argv[]) {
    std::string trace_path, hw_path, out_path, style = "at", tiebreak = "oldest";
    double quantum_ns = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--trace") trace_path = next();
        else if (a == "--hw") hw_path = next();
        else if (a == "--style") style = next();
        else if (a == "--quantum-ns") quantum_ns = std::stod(next());
        else if (a == "--out") out_path = next();
        else if (a == "--tiebreak") tiebreak = next();
        else {
            std::cerr << "usage: accel_sim --trace T --hw H [--style at|lt] [--quantum-ns Q] "
                         "[--tiebreak oldest|kernel] [--out F]\n";
            return 2;
        }
    }
    if (trace_path.empty() || hw_path.empty() || (style != "at" && style != "lt") ||
        (tiebreak != "oldest" && tiebreak != "kernel")) {
        std::cerr << "need --trace and --hw; --style is at or lt; --tiebreak is oldest or kernel\n";
        return 2;
    }
    sc_core::sc_set_time_resolution(1, sc_core::SC_FS);
    sc_core::sc_report_handler::set_actions("/IEEE_Std_1666/deprecated", sc_core::SC_DO_NOTHING);
    accel::Trace trace = accel::load_trace(trace_path);
    accel::Hardware hw = accel::load_hardware(hw_path);
    auto r = accel::run(trace, hw, style == "at" ? accel::Style::AT : accel::Style::LT,
                        sc_core::sc_time(quantum_ns, sc_core::SC_NS),
                        tiebreak == "oldest" ? accel::TieBreak::Oldest : accel::TieBreak::Kernel);
    const auto& s = r.stats;
    nlohmann::json j = {
        {"style", style}, {"quantum_ns", quantum_ns}, {"tiebreak", tiebreak}, {"horizon_s", s.horizon}, {"wall_s", r.wall_s},
        {"hbm_transactions", r.hbm_transactions}, {"delta_cycles", r.delta_cycles},
        {"busy", {{"ntt", s.busy[0]}, {"mac", s.busy[1]}, {"auto", s.busy[2]}, {"hbm", s.hbm_busy}}},
        {"bytes", s.bytes}, {"op_start", s.op_start}, {"op_end", s.op_end}};
    std::string text = j.dump(1);
    if (!out_path.empty()) std::ofstream(out_path) << text << "\n";
    std::cout << text << "\n";
    return 0;
}
