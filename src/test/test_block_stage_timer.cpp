#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_block_stage_timer.h"

#include <set>
#include <string>

using namespace resonance;
using std::chrono::microseconds;
using std::chrono::steady_clock;

TEST_CASE("block stage totals accumulate per stage and reset together", "[block_stage_timer]") {
    BlockStageTotals totals;
    totals.add(BlockStage::Direct, 100);
    totals.add(BlockStage::Direct, 50);
    totals.add(BlockStage::Output, 7);
    REQUIRE(totals.ns(BlockStage::Direct) == 150);
    REQUIRE(totals.ns(BlockStage::Output) == 7);
    REQUIRE(totals.ns(BlockStage::Reflections) == 0);

    totals.reset();
    for (size_t i = 0; i < kBlockStageCount; ++i)
        REQUIRE(totals.ns(static_cast<BlockStage>(i)) == 0);
}

TEST_CASE("block stage totals keep the longest lap per stage until reset", "[block_stage_timer]") {
    BlockStageTotals totals;
    totals.add(BlockStage::Reflections, 300);
    totals.add(BlockStage::Reflections, 20'000);
    totals.add(BlockStage::Reflections, 310);
    totals.add(BlockStage::Direct, 15);
    REQUIRE(totals.max_ns(BlockStage::Reflections) == 20'000);
    REQUIRE(totals.max_ns(BlockStage::Direct) == 15);
    REQUIRE(totals.max_ns(BlockStage::Output) == 0);

    totals.reset();
    for (size_t i = 0; i < kBlockStageCount; ++i)
        REQUIRE(totals.max_ns(static_cast<BlockStage>(i)) == 0);
}

TEST_CASE("duration totals keep the sum and the longest call until reset", "[block_stage_timer]") {
    DurationTotals times;
    times.add(200);
    times.add(4'000);
    times.add(250);
    REQUIRE(times.total_ns() == 4'450);
    REQUIRE(times.max_ns() == 4'000);

    times.reset();
    REQUIRE(times.total_ns() == 0);
    REQUIRE(times.max_ns() == 0);

    times.add(30);
    REQUIRE(times.max_ns() == 30);
}

TEST_CASE("stage laps cover the whole span without gaps", "[block_stage_timer]") {
    const steady_clock::time_point start{};
    BlockStageTotals totals;
    StageLapClock clock(start);
    clock.lap(totals, BlockStage::Input, start + microseconds(10));
    clock.lap(totals, BlockStage::Reflections, start + microseconds(210));
    clock.lap(totals, BlockStage::Output, start + microseconds(215));

    REQUIRE(totals.ns(BlockStage::Input) == 10'000);
    REQUIRE(totals.ns(BlockStage::Reflections) == 200'000);
    REQUIRE(totals.ns(BlockStage::Output) == 5'000);
    uint64_t sum = 0;
    for (size_t i = 0; i < kBlockStageCount; ++i)
        sum += totals.ns(static_cast<BlockStage>(i));
    REQUIRE(sum == 215'000);
}

TEST_CASE("elapsed time never goes negative", "[block_stage_timer]") {
    const steady_clock::time_point start = steady_clock::time_point{} + microseconds(5);
    REQUIRE(elapsed_ns(start, start - microseconds(1)) == 0);
    REQUIRE(elapsed_ns(start, start) == 0);
    REQUIRE(ns_to_us(elapsed_ns(start, start + microseconds(3))) == Approx(3.0));
}

TEST_CASE("block stage names are unique and non-empty", "[block_stage_timer]") {
    std::set<std::string> names;
    for (size_t i = 0; i < kBlockStageCount; ++i) {
        const std::string name = block_stage_name(static_cast<BlockStage>(i));
        REQUIRE_FALSE(name.empty());
        names.insert(name);
    }
    REQUIRE(names.size() == kBlockStageCount);
}
