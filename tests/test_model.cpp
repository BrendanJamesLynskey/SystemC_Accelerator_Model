// The kernel-free parts: trace loading, the cost model and the scratchpad planner,
// checked against FHE_Accelerator_Sim's numbers exported in tests/data.
#include <gtest/gtest.h>

#include <fstream>
#include <numeric>

#include <nlohmann/json.hpp>

#include "accel/model.hpp"

using namespace accel;

static std::string data(const std::string& c, const std::string& f) { return std::string(ACCEL_DATA_DIR) + "/" + c + "/" + f; }

static nlohmann::json expected(const std::string& c) {
    std::ifstream f(data(c, "expected.json"));
    return nlohmann::json::parse(f);
}

TEST(Trace, LoadsTheFheSimFormat) {
    Trace t = load_trace(data("boot_ark", "trace.json"));
    EXPECT_EQ(t.log_n, 16);
    EXPECT_EQ(t.ops.size(), 203u);
    std::size_t kernels = 0;
    for (const auto& o : t.ops) kernels += o.kernels.size();
    EXPECT_EQ(kernels, 1153u);
    EXPECT_THROW(load_hardware(data("boot_ark", "missing.json")), std::runtime_error);
}

TEST(CostModel, NttTimeIsButterfliesOverRate) {
    Hardware hw = load_hardware(data("boot_ark", "hw.json"));
    Kernel k{"ntt", 3, 10};                                      // 3 limbs of N = 2^16, few words: logic-bound
    Segment s = segment(hw, 16, k);
    double butterflies = 3.0 * (65536 / 2) * 16;
    EXPECT_EQ(s.unit, Unit::Ntt);
    EXPECT_DOUBLE_EQ(s.work, butterflies);
    EXPECT_DOUBLE_EQ(s.time, butterflies / (4096 * 1.0 * 1e9 * 1.0));
}

TEST(CostModel, SramBoundKernelsTakeTheSramTime) {
    Hardware hw = load_hardware(data("boot_ark", "hw.json"));
    Kernel k{"mac", 1, 1'000'000};                               // one multiply-add, a million words
    Segment s = segment(hw, 16, k);
    EXPECT_EQ(s.unit, Unit::Mac);
    EXPECT_DOUBLE_EQ(s.time, 1'000'000.0 * 8 / (20000.0 * 1e9 * 1.0));
    EXPECT_THROW(unit_of("fft"), std::runtime_error);
}

TEST(Scratchpad, LruEvictsTheOldestUnpinned) {
    Scratchpad sp(100);
    std::vector<Scratchpad::Evicted> ev;
    EXPECT_TRUE(sp.alloc("a", 40, "ct", true, {}, ev));
    EXPECT_TRUE(sp.alloc("b", 40, "key", false, {}, ev));
    EXPECT_TRUE(sp.hit("a", 40));                                // a becomes most recent
    EXPECT_TRUE(sp.alloc("c", 40, "ct", false, {}, ev));         // evicts b, not a
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].name, "b");
    EXPECT_FALSE(sp.hit("b", 40));
    EXPECT_FALSE(sp.alloc("d", 90, "ct", false, {"a", "c"}, ev));   // everything pinned
    EXPECT_FALSE(sp.alloc("huge", 101, "ct", false, {}, ev));
    EXPECT_FALSE(sp.hit("a", 41));                               // a smaller copy is a miss
}

class PlannerBytes : public ::testing::TestWithParam<std::string> {};

TEST_P(PlannerBytes, MatchTheSimPyModelExactly) {
    // The planner decides every HBM byte at issue, in program order, so its totals must
    // equal the SimPy model's to the byte.
    const std::string c = GetParam();
    Trace t = load_trace(data(c, "trace.json"));
    Hardware hw = load_hardware(data(c, "hw.json"));
    Planner pl(t, hw);
    std::int64_t key = 0, pt = 0, rd = 0, wr = 0;
    for (const auto& o : t.ops) {
        OpPlan p = pl.plan(o);
        key += p.key_load;
        pt += p.pt_load;
        for (const auto& x : p.ct_load) rd += x.second;
        for (const auto& x : p.writebacks) wr += x.second;
        wr += p.out_write;
    }
    auto e = expected(c)["bytes"];
    EXPECT_EQ(key, e["key"].get<std::int64_t>());
    EXPECT_EQ(pt, e["pt"].get<std::int64_t>());
    EXPECT_EQ(rd, e["ct_read"].get<std::int64_t>());
    EXPECT_EQ(wr, e["ct_write"].get<std::int64_t>());
}

INSTANTIATE_TEST_SUITE_P(Cases, PlannerBytes, ::testing::Values("hmult", "hrot", "boot_ark", "boot_small"));

TEST(CostModel, UnitBusyTimesMatchTheSimPyModel) {
    // Summing every kernel's time per unit reproduces the SimPy model's busy times.
    for (std::string c : {"boot_ark", "boot_small"}) {
        Trace t = load_trace(data(c, "trace.json"));
        Hardware hw = load_hardware(data(c, "hw.json"));
        double busy[kUnits] = {};
        for (const auto& o : t.ops)
            for (const auto& k : o.kernels) {
                Segment s = segment(hw, t.log_n, k);
                busy[static_cast<int>(s.unit)] += s.time;
            }
        auto e = expected(c)["busy"];
        EXPECT_NEAR(busy[0], e["ntt"].get<double>(), 1e-12 * e["ntt"].get<double>()) << c;
        EXPECT_NEAR(busy[1], e["mac"].get<double>(), 1e-12 * e["mac"].get<double>()) << c;
        EXPECT_NEAR(busy[2], e["auto"].get<double>(), 1e-12 * e["auto"].get<double>()) << c;
    }
}
