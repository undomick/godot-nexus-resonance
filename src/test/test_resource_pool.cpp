#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_resource_pool.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace resonance;

namespace {

struct FakeCounters {
    std::atomic<int> next_id{1};
    std::atomic<int> sources_created{0};
    std::atomic<int> effects_created{0};
    std::atomic<int> resets{0};
    std::atomic<int> released{0};
    std::atomic<bool> fail_sources{false};
    std::atomic<bool> fail_effects{false};
    std::atomic<int> create_delay_ms{0};
    std::atomic<int> source_creates_started{0};
};

struct FakeBackend {
    using Source = int;
    using Effect = int;
    FakeCounters* counters = nullptr;

    int create_source() const {
        counters->source_creates_started++;
        std::this_thread::sleep_for(std::chrono::milliseconds(counters->create_delay_ms.load()));
        if (counters->fail_sources.load())
            return 0;
        counters->sources_created++;
        return counters->next_id++;
    }
    int create_effect() const {
        if (counters->fail_effects.load())
            return 0;
        counters->effects_created++;
        return counters->next_id++;
    }
    void reset(int) const { counters->resets++; }
    void release(int item) const {
        if (item)
            counters->released++;
    }
    void on_pool_thread_start() const {}
};

bool eventually(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

ResourcePoolLimits limits(int min_ready, int max_kept) {
    ResourcePoolLimits l;
    l.min_ready = min_ready;
    l.max_kept = max_kept;
    return l;
}

} // namespace

TEST_CASE("resource pool builds effects only once they are asked for", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(2, 8));
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 2; }));
    REQUIRE(pool.get_stats().ready_effects == 0);
    REQUIRE(counters.effects_created == 0);
}

TEST_CASE("resource pool builds effects from the start when asked to", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    ResourcePoolLimits l = limits(1, 4);
    l.effects_from_start = true;
    pool.start(FakeBackend{&counters}, l);
    REQUIRE(eventually([&] { return pool.get_stats().ready_effects == 1; }));
    REQUIRE(pool.get_stats().effect_misses == 0);
}

TEST_CASE("resource pool fills to the minimum after start", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(2, 8));
    REQUIRE(pool.take_effect() == 0);
    REQUIRE(eventually([&] {
        const ResourcePoolStats s = pool.get_stats();
        return s.ready_sources == 2 && s.ready_effects == 2;
    }));
    REQUIRE(counters.sources_created == 2);
    REQUIRE(counters.effects_created == 2);
}

TEST_CASE("resource pool refills after a take", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(1, 4));
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 1; }));
    REQUIRE(pool.take_source() != 0);
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 1 && counters.sources_created == 2; }));
}

TEST_CASE("resource pool counts a miss when empty", "[resource_pool]") {
    FakeCounters counters;
    counters.fail_sources = true;
    ResourcePool<FakeBackend> pool;
    counters.fail_effects = true;
    pool.start(FakeBackend{&counters}, limits(2, 8));
    REQUIRE(pool.take_source() == 0);
    REQUIRE(pool.take_effect() == 0);
    const ResourcePoolStats s = pool.get_stats();
    REQUIRE(s.source_misses == 1);
    REQUIRE(s.effect_misses == 1);
}

TEST_CASE("resource pool keeps refilling sources after an effect failed", "[resource_pool]") {
    FakeCounters counters;
    counters.fail_effects = true;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(1, 4));
    REQUIRE(pool.take_effect() == 0);
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 1; }));
    REQUIRE(pool.take_source() != 0);
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 1 && counters.sources_created == 2; }));
}

TEST_CASE("resource pool resets a returned effect before handing it out again", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(1, 4));
    REQUIRE(pool.take_effect() == 0);
    REQUIRE(eventually([&] { return pool.get_stats().ready_effects == 1; }));
    const int effect = pool.take_effect();
    REQUIRE(effect != 0);
    REQUIRE(eventually([&] { return pool.get_stats().ready_effects == 1; }));
    pool.give_back_effect(effect);
    REQUIRE(eventually([&] { return counters.resets == 1 && pool.get_stats().ready_effects == 2; }));
}

TEST_CASE("resource pool releases returned effects above the maximum", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(1, 1));
    REQUIRE(pool.take_effect() == 0);
    REQUIRE(eventually([&] { return pool.get_stats().ready_effects == 1; }));
    pool.give_back_effect(1000);
    REQUIRE(counters.released == 1);
    REQUIRE(counters.resets == 0);
}

TEST_CASE("resource pool releases everything on stop and after it", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(2, 8));
    REQUIRE(pool.take_effect() == 0);
    REQUIRE(eventually([&] {
        const ResourcePoolStats s = pool.get_stats();
        return s.ready_sources == 2 && s.ready_effects == 2;
    }));
    pool.stop();
    REQUIRE(counters.released == 4);
    REQUIRE_FALSE(pool.is_running());
    REQUIRE(pool.take_source() == 0);
    REQUIRE(pool.get_stats().source_misses == 0);
    pool.give_back_effect(1000);
    REQUIRE(counters.released == 5);
}

TEST_CASE("resource pool keeps refilling effects after a source failed", "[resource_pool]") {
    FakeCounters counters;
    counters.fail_sources = true;
    ResourcePool<FakeBackend> pool;
    ResourcePoolLimits l = limits(2, 4);
    l.effects_from_start = true;
    pool.start(FakeBackend{&counters}, l);
    REQUIRE(eventually([&] { return pool.get_stats().ready_effects == 2; }));
    REQUIRE(pool.get_stats().ready_sources == 0);
}

TEST_CASE("resource pool stop waits for a create in flight and releases it", "[resource_pool]") {
    FakeCounters counters;
    counters.create_delay_ms = 100;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(1, 4));
    REQUIRE(eventually([&] { return counters.source_creates_started == 1; }));
    pool.stop();
    REQUIRE(counters.sources_created == 1);
    REQUIRE(counters.released == 1);
    REQUIRE(pool.get_stats().ready_sources == 0);
}

TEST_CASE("resource pool restart releases the old items and refills", "[resource_pool]") {
    FakeCounters counters;
    ResourcePool<FakeBackend> pool;
    pool.start(FakeBackend{&counters}, limits(2, 4));
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 2; }));
    pool.start(FakeBackend{&counters}, limits(2, 4));
    REQUIRE(counters.released == 2);
    REQUIRE(eventually([&] { return pool.get_stats().ready_sources == 2 && counters.sources_created == 4; }));
}
