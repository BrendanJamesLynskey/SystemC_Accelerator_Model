// The SystemC model against the SimPy model, on the same traces.
//
// SystemC elaborates once per process, so every simulation runs in a forked child
// that sends its results back as JSON.
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include "accel/tile.hpp"

using namespace accel;
using nlohmann::json;

static std::string data(const std::string& c, const std::string& f) { return std::string(ACCEL_DATA_DIR) + "/" + c + "/" + f; }

static json simulate_in_child(const std::string& c, Style style, double quantum_ns) {
    int fd[2];
    if (pipe(fd) != 0) throw std::runtime_error("pipe");
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        sc_core::sc_set_time_resolution(1, sc_core::SC_FS);
        sc_core::sc_report_handler::set_actions("/IEEE_Std_1666/deprecated", sc_core::SC_DO_NOTHING);
        Trace t = load_trace(data(c, "trace.json"));
        Hardware hw = load_hardware(data(c, "hw.json"));
        RunResult r = run(t, hw, style, sc_core::sc_time(quantum_ns, sc_core::SC_NS));
        json j = {{"horizon", r.stats.horizon}, {"op_end", r.stats.op_end}, {"op_start", r.stats.op_start},
                  {"busy", r.stats.busy}, {"hbm_busy", r.stats.hbm_busy}, {"bytes", r.stats.bytes},
                  {"transactions", r.hbm_transactions}};
        std::string s = j.dump();
        ssize_t off = 0;
        while (off < static_cast<ssize_t>(s.size())) {
            ssize_t n = write(fd[1], s.data() + off, s.size() - off);
            if (n <= 0) break;
            off += n;
        }
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    std::string s;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) s.append(buf, n);
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || s.empty()) throw std::runtime_error("child failed");
    return json::parse(s);
}

static json expected(const std::string& c) {
    std::ifstream f(data(c, "expected.json"));
    return json::parse(f);
}

static double max_abs_diff(const json& a, const json& b) {
    double m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i].get<double>() - b[i].get<double>()));
    return m;
}

// AT: the same costs, the same FIFO arbitration -> the same schedule as SimPy, up to
// the 1 fs time resolution (each delay is rounded to the nearest femtosecond).
class AtAgreement : public ::testing::TestWithParam<std::string> {};

TEST_P(AtAgreement, MatchesSimPyToFemtoseconds) {
    const std::string c = GetParam();
    json r = simulate_in_child(c, Style::AT, 0), e = expected(c);
    double H = e["horizon_s"];
    EXPECT_NEAR(r["horizon"].get<double>(), H, 1e-12) << c;            // within 1 ps on a ms-scale run
    EXPECT_LT(max_abs_diff(r["op_end"], e["op_end"]), 1e-12) << c;
    EXPECT_EQ(r["bytes"], e["bytes"]);
    EXPECT_NEAR(r["busy"][0].get<double>(), e["busy"]["ntt"].get<double>(), 1e-12 * H);
    EXPECT_NEAR(r["hbm_busy"].get<double>(), e["busy"]["hbm"].get<double>(), 1e-12 * H);
}

INSTANTIATE_TEST_SUITE_P(Cases, AtAgreement, ::testing::Values("hmult", "hrot", "boot_ark", "boot_small"));

TEST(Lt, ZeroQuantumMatchesAt) {
    json at = simulate_in_child("boot_ark", Style::AT, 0), lt = simulate_in_child("boot_ark", Style::LT, 0);
    EXPECT_NEAR(lt["horizon"].get<double>(), at["horizon"].get<double>(), 1e-12);
    EXPECT_EQ(lt["bytes"], at["bytes"]);
}

TEST(Lt, LargeQuantaStillMoveEveryByteAndRespectResourceBounds) {
    json e = expected("boot_ark");
    for (double q : {1e3, 1e5}) {
        json lt = simulate_in_child("boot_ark", Style::LT, q);
        EXPECT_EQ(lt["bytes"], e["bytes"]) << q;
        double H = lt["horizon"];
        // no resource can be busier than the run is long
        EXPECT_GE(H * (1 + 1e-12), lt["hbm_busy"].get<double>()) << q;
        for (int u = 0; u < kUnits; ++u) EXPECT_GE(H * (1 + 1e-12), lt["busy"][u].get<double>()) << q;
        // every op ends after it starts
        for (std::size_t i = 0; i < lt["op_end"].size(); ++i)
            EXPECT_GE(lt["op_end"][i].get<double>(), lt["op_start"][i].get<double>());
    }
}

TEST(Hbm, ChunksAreTransactions) {
    json r = simulate_in_child("hmult", Style::AT, 0);
    json e = expected("hmult");
    std::int64_t total = 0;
    for (auto& [k, v] : e["bytes"].items()) total += v.get<std::int64_t>();
    std::int64_t chunk = 4LL << 20;
    // at least ceil(total / chunk) transactions (each transfer is split separately)
    EXPECT_GE(r["transactions"].get<std::int64_t>(), (total + chunk - 1) / chunk);
}
