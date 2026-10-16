#include "matchmesh/loadgen/histogram.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

namespace matchmesh::loadgen {

using std::chrono::microseconds;

std::size_t LatencyHistogram::index_of(std::uint64_t micros) noexcept {
    if (micros < kSubBuckets) {
        return static_cast<std::size_t>(micros);  // small values: one bucket each
    }
    // Which power of two the value is in, and which 64th of that range.
    const auto top_bit = static_cast<unsigned>(std::bit_width(micros)) - 1;  // >= 6
    const unsigned shift = top_bit - kSubBucketBits;
    const std::uint64_t sub = (micros >> shift) - kSubBuckets;  // 0..63
    return static_cast<std::size_t>((shift + 1) * kSubBuckets + sub);
}

std::uint64_t LatencyHistogram::highest_in(std::size_t index) noexcept {
    if (index < kSubBuckets) {
        return index;
    }
    const std::uint64_t shift = index / kSubBuckets - 1;
    const std::uint64_t sub = index % kSubBuckets + kSubBuckets;
    return ((sub + 1) << shift) - 1;
}

void LatencyHistogram::record(std::chrono::nanoseconds value) {
    const auto micros = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, std::chrono::duration_cast<microseconds>(value).count()));
    const std::size_t index = index_of(micros);
    if (index >= counts_.size()) {
        counts_.resize(index + 1, 0);
    }
    ++counts_[index];
    min_ = count_ == 0 ? micros : std::min(min_, micros);
    max_ = std::max(max_, micros);
    ++count_;
    sum_ += micros;
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
    if (other.count_ == 0) {
        return;
    }
    if (other.counts_.size() > counts_.size()) {
        counts_.resize(other.counts_.size(), 0);
    }
    for (std::size_t i = 0; i < other.counts_.size(); ++i) {
        counts_[i] += other.counts_[i];
    }
    min_ = count_ == 0 ? other.min_ : std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    count_ += other.count_;
    sum_ += other.sum_;
}

std::uint64_t LatencyHistogram::count() const noexcept {
    return count_;
}

microseconds LatencyHistogram::min() const noexcept {
    return microseconds(static_cast<std::int64_t>(min_));
}

microseconds LatencyHistogram::max() const noexcept {
    return microseconds(static_cast<std::int64_t>(max_));
}

microseconds LatencyHistogram::mean() const noexcept {
    return count_ == 0 ? microseconds(0)
                       : microseconds(static_cast<std::int64_t>(sum_ / count_));
}

microseconds LatencyHistogram::percentile(double percent) const {
    if (count_ == 0) {
        return microseconds(0);
    }
    percent = std::clamp(percent, 0.0, 100.0);
    // The rank of the sample we want: p99 of 1,000 samples is the 990th.
    const auto rank = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::ceil(percent / 100.0 * static_cast<double>(count_))));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        seen += counts_[i];
        if (seen >= rank) {
            return microseconds(static_cast<std::int64_t>(std::min(highest_in(i), max_)));
        }
    }
    return max();
}

std::vector<LatencyHistogram::Range> LatencyHistogram::ranges() const {
    std::vector<Range> out;
    if (count_ == 0) {
        return out;
    }
    // Power-of-two ranges: [0, 1), [1, 2), [2, 4), [4, 8), ...
    const auto range_of = [](std::uint64_t micros) -> unsigned {
        return micros == 0 ? 0 : static_cast<unsigned>(std::bit_width(micros));
    };
    const auto low_of = [](unsigned range) -> std::uint64_t {
        return range == 0 ? 0 : std::uint64_t{1} << (range - 1);
    };
    for (unsigned r = range_of(min_); r <= range_of(max_); ++r) {
        out.push_back(Range{microseconds(static_cast<std::int64_t>(low_of(r))),
                            microseconds(static_cast<std::int64_t>(std::uint64_t{1} << r)), 0});
    }
    const unsigned first = range_of(min_);
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        if (counts_[i] != 0) {
            // Every value in bucket i is in the same power-of-two range.
            out[range_of(highest_in(i)) - first].count += counts_[i];
        }
    }
    return out;
}

std::string format_duration(microseconds value) {
    const auto us = static_cast<double>(value.count());
    char text[32];
    if (us < 1'000) {
        std::snprintf(text, sizeof text, "%.0f us", us);
    } else if (us < 1'000'000) {
        std::snprintf(text, sizeof text, us < 10'000 ? "%.2f ms" : "%.1f ms", us / 1'000);
    } else {
        std::snprintf(text, sizeof text, "%.2f s", us / 1'000'000);
    }
    return text;
}

}  // namespace matchmesh::loadgen
