#include <gtest/gtest.h>
#include <cmath>
#include <stdexcept>
#include "include/Instrument.h"

namespace {

constexpr double EPS = 1e-12;

TEST(TickConversion, default_tick_size_is_one_cent) {
    Instrument ins;
    EXPECT_DOUBLE_EQ(ins.tick_size, 0.01);
}

TEST(TickConversion, round_trip_on_grid_values) {
    Instrument ins(0.01);
    for (double p : {0.01, 0.10, 1.00, 99.99, 100.00, 100.01, 12345.67}) {
        Ticks t = ins.to_ticks(p);
        double back = ins.to_price(t);
        EXPECT_NEAR(back, p, EPS) << "round-trip failed at p=" << p;
    }
}

TEST(TickConversion, snaps_off_grid_values_to_nearest) {
    Instrument ins(0.01);
    EXPECT_EQ(ins.to_ticks(100.004), Ticks{10000});
    EXPECT_EQ(ins.to_ticks(100.005), Ticks{10001});  // half-away-from-zero
    EXPECT_EQ(ins.to_ticks(100.006), Ticks{10001});
    EXPECT_EQ(ins.to_ticks(100.013), Ticks{10001});
    EXPECT_EQ(ins.to_ticks(100.015), Ticks{10002});
}

TEST(TickConversion, integer_arithmetic_preserves_equality) {
    Instrument ins(0.01);
    Ticks a = ins.to_ticks(100.10);
    Ticks b = ins.to_ticks(100.10);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, Ticks{10010});
}

TEST(TickConversion, monotonic) {
    Instrument ins(0.01);
    Ticks lo = ins.to_ticks(99.99);
    Ticks mid = ins.to_ticks(100.00);
    Ticks hi = ins.to_ticks(100.01);
    EXPECT_LT(lo, mid);
    EXPECT_LT(mid, hi);
}

TEST(TickConversion, custom_tick_size) {
    Instrument ins(0.05);
    EXPECT_EQ(ins.to_ticks(100.00), Ticks{2000});
    EXPECT_EQ(ins.to_ticks(100.05), Ticks{2001});
    EXPECT_DOUBLE_EQ(ins.to_price(Ticks{2001}), 100.05);
}

TEST(TickConversion, rejects_nonpositive_tick_size) {
    EXPECT_THROW(Instrument{0.0}, std::invalid_argument);
    EXPECT_THROW(Instrument{-0.01}, std::invalid_argument);
}

}  // namespace
