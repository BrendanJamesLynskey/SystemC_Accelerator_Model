// SystemC's library provides main() and calls sc_main(); run GoogleTest from there.
#include <gtest/gtest.h>
#include <systemc>

int sc_main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
