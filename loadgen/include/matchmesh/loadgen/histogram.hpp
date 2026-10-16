#pragma once

// LatencyHistogram: records many durations cheaply and answers percentile
// questions ("what was the p99?") afterwards.
//
// Keeping every sample and sorting would work for 10,000 values, but a
// histogram uses fixed memory however many samples arrive, merges in O(buckets)
// (one histogram per thread, combined at the end, so recording needs no lock),
// and is how real load tools (HdrHistogram, wrk2, Prometheus) do it.
//
// Buckets are log-linear, the HdrHistogram layout: values under 64 us get a
// bucket each, and every power-of-two range above that ([64, 128), [128, 256),
// ...) is split into 64 equal buckets. So any recorded value is known to
// within 1/64 (about 1.6%) whether it's 80 us or 8 s, with a few thousand
// buckets in total. Percentiles report the top of the bucket they land in
// (never under-reporting), capped at the exact maximum seen.
//
// Not thread-safe: give each thread its own and merge them.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matchmesh::loadgen {

class LatencyHistogram {
public:
    // Records one duration, at microsecond resolution. Negative durations
    // (possible when two clocks disagree slightly) count as zero.
    void record(std::chrono::nanoseconds value);

    // Adds every sample from `other` to this histogram.
    void merge(const LatencyHistogram& other);

    [[nodiscard]] std::uint64_t count() const noexcept;
    // All of these are zero for an empty histogram.
    [[nodiscard]] std::chrono::microseconds min() const noexcept;
    [[nodiscard]] std::chrono::microseconds max() const noexcept;
    [[nodiscard]] std::chrono::microseconds mean() const noexcept;
    // The value below which `percent` of samples fall, e.g. 99.0 for p99.
    // percent is clamped to [0, 100]; 100 gives the exact maximum.
    [[nodiscard]] std::chrono::microseconds percentile(double percent) const;

    // The distribution in power-of-two ranges ([1, 2) us, [2, 4) us, ...),
    // for printing. Covers min() to max(), including empty ranges between.
    struct Range {
        std::chrono::microseconds low;   // inclusive
        std::chrono::microseconds high;  // exclusive
        std::uint64_t count = 0;
    };
    [[nodiscard]] std::vector<Range> ranges() const;

private:
    static constexpr unsigned kSubBucketBits = 6;  // 64 buckets per power of two
    static constexpr std::uint64_t kSubBuckets = std::uint64_t{1} << kSubBucketBits;

    [[nodiscard]] static std::size_t index_of(std::uint64_t micros) noexcept;
    [[nodiscard]] static std::uint64_t highest_in(std::size_t index) noexcept;

    std::vector<std::uint64_t> counts_;  // grows to the largest bucket used
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;  // microseconds
    std::uint64_t min_ = 0;
    std::uint64_t max_ = 0;
};

// "850 us", "12.4 ms", "3.21 s": short, human-readable.
[[nodiscard]] std::string format_duration(std::chrono::microseconds value);

}  // namespace matchmesh::loadgen
