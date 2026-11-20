#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_block_time_histogram.h"

#include <cstdint>

using namespace resonance;

TEST_CASE("every block time lands in the bucket whose range contains it", "[block_time_histogram]") {
    uint64_t lower_us = 0;
    for (size_t bucket = 0; bucket + 1 < kBlockTimeBucketCount; ++bucket) {
        const uint64_t upper_us = block_time_bucket_upper_us(bucket);
        REQUIRE(upper_us > lower_us);
        REQUIRE(block_time_bucket(lower_us) == bucket);
        REQUIRE(block_time_bucket(upper_us - 1) == bucket);
        lower_us = upper_us;
    }
}

TEST_CASE("bucket edges stay within a quarter octave", "[block_time_histogram]") {
    for (size_t bucket = kBlockTimeSubBuckets; bucket < kBlockTimeBucketCount; ++bucket) {
        const double lower = static_cast<double>(block_time_bucket_upper_us(bucket - 1));
        const double upper = static_cast<double>(block_time_bucket_upper_us(bucket));
        REQUIRE(upper / lower <= 1.25);
    }
}

TEST_CASE("block times beyond the last edge fall into the last bucket", "[block_time_histogram]") {
    const uint64_t last_upper = block_time_bucket_upper_us(kBlockTimeBucketCount - 1);
    REQUIRE(last_upper > 100'000);
    REQUIRE(block_time_bucket(last_upper) == kBlockTimeBucketCount - 1);
    REQUIRE(block_time_bucket(UINT64_MAX) == kBlockTimeBucketCount - 1);
}

TEST_CASE("block time histogram counts per bucket and resets", "[block_time_histogram]") {
    BlockTimeHistogram histogram;
    histogram.record(0);
    histogram.record(300);
    histogram.record(310);
    histogram.record(20'000);
    REQUIRE(histogram.count(block_time_bucket(0)) == 1);
    REQUIRE(histogram.count(block_time_bucket(300)) == 2);
    REQUIRE(histogram.count(block_time_bucket(20'000)) == 1);

    histogram.reset();
    for (size_t bucket = 0; bucket < kBlockTimeBucketCount; ++bucket)
        REQUIRE(histogram.count(bucket) == 0);
}
