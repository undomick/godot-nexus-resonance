#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_latest_value_slot.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace {

constexpr int kSnapshotFields = 48;

// Every field holds the same counter, so a torn copy shows up as mismatched fields.
struct Snapshot {
    std::array<uint32_t, kSnapshotFields> fields{};
};

Snapshot make_snapshot(uint32_t counter) {
    Snapshot s;
    s.fields.fill(counter);
    return s;
}

bool is_consistent(const Snapshot& s) {
    for (uint32_t field : s.fields) {
        if (field != s.fields[0])
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("consume before any publish reports nothing new", "[latest_value_slot]") {
    resonance::LatestValueSlot<int> slot;
    REQUIRE_FALSE(slot.consume());
}

TEST_CASE("a published value is consumed exactly once", "[latest_value_slot]") {
    resonance::LatestValueSlot<int> slot;
    slot.publish(7);

    REQUIRE(slot.consume());
    REQUIRE(slot.front() == 7);
    REQUIRE_FALSE(slot.consume());
    REQUIRE(slot.front() == 7);
}

TEST_CASE("several publishes before a consume yield only the latest", "[latest_value_slot]") {
    resonance::LatestValueSlot<int> slot;
    slot.publish(1);
    slot.publish(2);
    slot.publish(3);

    REQUIRE(slot.consume());
    REQUIRE(slot.front() == 3);
    REQUIRE_FALSE(slot.consume());
}

TEST_CASE("publishing after a consume delivers the new value", "[latest_value_slot]") {
    resonance::LatestValueSlot<int> slot;
    slot.publish(1);
    REQUIRE(slot.consume());

    slot.publish(2);
    REQUIRE(slot.consume());
    REQUIRE(slot.front() == 2);
}

TEST_CASE("concurrent producer and consumer never see a torn or stale value", "[latest_value_slot][threads]") {
    constexpr uint32_t kPublishCount = 200000;
    resonance::LatestValueSlot<Snapshot> slot;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (uint32_t i = 1; i <= kPublishCount; ++i)
            slot.publish(make_snapshot(i));
        producer_done.store(true, std::memory_order_release);
    });

    bool all_consistent = true;
    bool monotonic = true;
    uint32_t last_seen = 0;
    auto consume_and_check = [&]() {
        if (!slot.consume())
            return;
        const Snapshot& s = slot.front();
        all_consistent = all_consistent && is_consistent(s);
        monotonic = monotonic && s.fields[0] > last_seen;
        last_seen = s.fields[0];
    };

    while (!producer_done.load(std::memory_order_acquire))
        consume_and_check();
    producer.join();
    consume_and_check();

    REQUIRE(all_consistent);
    REQUIRE(monotonic);
    REQUIRE(last_seen == kPublishCount);
}
