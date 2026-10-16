// Tests for LatencyHistogram: exact for small values, within 1/64 for large
// ones, correct percentiles, and merging.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>

#include <gtest/gtest.h>

#include "matchmesh/loadgen/histogram.hpp"

namespace matchmesh::loadgen {
namespace {

using namespace std::chrono_literals;
using std::chrono::microseconds;

TEST(HistogramTest, EmptyHistogramReportsZeros) {
    const LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(99), 0us);
    EXPECT_EQ(h.max(), 0us);
    EXPECT_EQ(h.mean(), 0us);
    EXPECT_TRUE(h.ranges().empty());
}

TEST(HistogramTest, SmallValuesAreExact) {
    LatencyHistogram h;
    for (int us = 1; us <= 10; ++us) {
        h.record(microseconds(us));
    }
    EXPECT_EQ(h.count(), 10u);
    EXPECT_EQ(h.min(), 1us);
    EXPECT_EQ(h.max(), 10us);
    EXPECT_EQ(h.percentile(50), 5us);  // the 5th of 10
    EXPECT_EQ(h.percentile(90), 9us);
    EXPECT_EQ(h.percentile(100), 10us);
    EXPECT_EQ(h.mean(), 5us);  // 55 / 10, rounded down
}

TEST(HistogramTest, LargeValuesAreWithinOneSixtyFourth) {
    for (const std::int64_t us : {100LL, 1'234LL, 99'999LL, 2'500'000LL, 3'600'000'000LL}) {
        LatencyHistogram h;
        h.record(microseconds(us));
        h.record(microseconds(us * 2));  // so the p50 isn't just the exact max
        const auto reported = static_cast<double>(h.percentile(50).count());
        EXPECT_GE(reported, static_cast<double>(us)) << us;  // never under-reports
        EXPECT_LE(reported, static_cast<double>(us) * (1.0 + 1.0 / 64.0)) << us;
    }
}

TEST(HistogramTest, PercentilesOfAUniformSpread) {
    LatencyHistogram h;
    for (int us = 1; us <= 10'000; ++us) {
        h.record(microseconds(us));
    }
    // 1..10,000: the p-th percentile is p * 100 us, give or take a bucket.
    for (const double p : {50.0, 90.0, 99.0, 99.9}) {
        const double exact = p * 100.0;
        const auto reported = static_cast<double>(h.percentile(p).count());
        EXPECT_NEAR(reported, exact, exact / 64.0 + 1.0) << "p" << p;
    }
    EXPECT_EQ(h.percentile(100), 10'000us);
}

TEST(HistogramTest, AnOutlierShowsUpInTheTailNotTheMedian) {
    LatencyHistogram h;
    for (int i = 0; i < 999; ++i) {
        h.record(1ms);
    }
    h.record(2s);  // one stall
    EXPECT_LE(h.percentile(50), 1016us);
    EXPECT_LE(h.percentile(99), 1016us);
    EXPECT_EQ(h.percentile(99.95), 2s);
    EXPECT_EQ(h.max(), 2s);
}

TEST(HistogramTest, NegativeDurationsCountAsZero) {
    LatencyHistogram h;
    h.record(-5ms);
    EXPECT_EQ(h.count(), 1u);
    EXPECT_EQ(h.max(), 0us);
}

TEST(HistogramTest, MergingGivesTheSameAnswersAsRecordingEverythingInOne) {
    std::mt19937 rng(3);
    std::lognormal_distribution<double> latency(7.0, 1.0);  // around 1 ms, long tail
    LatencyHistogram all;
    LatencyHistogram parts[4];
    for (int i = 0; i < 20'000; ++i) {
        const microseconds sample(static_cast<std::int64_t>(latency(rng)));
        all.record(sample);
        parts[i % 4].record(sample);
    }
    LatencyHistogram merged;
    for (const LatencyHistogram& part : parts) {
        merged.merge(part);
    }
    EXPECT_EQ(merged.count(), all.count());
    EXPECT_EQ(merged.min(), all.min());
    EXPECT_EQ(merged.max(), all.max());
    EXPECT_EQ(merged.mean(), all.mean());
    for (const double p : {1.0, 50.0, 99.0, 99.9}) {
        EXPECT_EQ(merged.percentile(p), all.percentile(p)) << "p" << p;
    }
}

TEST(HistogramTest, RangesCoverEverySampleInPowersOfTwo) {
    LatencyHistogram h;
    h.record(3us);     // [2, 4)
    h.record(5us);     // [4, 8)
    h.record(6us);     // [4, 8)
    h.record(100us);   // [64, 128)
    const auto ranges = h.ranges();
    ASSERT_EQ(ranges.size(), 6u);  // [2,4) [4,8) [8,16) [16,32) [32,64) [64,128)
    EXPECT_EQ(ranges.front().low, 2us);
    EXPECT_EQ(ranges.front().count, 1u);
    EXPECT_EQ(ranges[1].count, 2u);
    EXPECT_EQ(ranges[2].count, 0u);  // empty ranges in between are kept
    EXPECT_EQ(ranges.back().high, 128us);
    EXPECT_EQ(ranges.back().count, 1u);
}

TEST(HistogramTest, DurationsPrintReadably) {
    EXPECT_EQ(format_duration(850us), "850 us");
    EXPECT_EQ(format_duration(1'234us), "1.23 ms");
    EXPECT_EQ(format_duration(12'400us), "12.4 ms");
    EXPECT_EQ(format_duration(3'210'000us), "3.21 s");
}

}  // namespace
}  // namespace matchmesh::loadgen
