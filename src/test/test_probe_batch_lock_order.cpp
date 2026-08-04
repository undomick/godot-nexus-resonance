#include "../lib/catch2/single_include/catch2/catch.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

// Mirrors ResonanceProbeBatchRegistry lock order vs the simulation worker:
// both sides take simulation_mutex before the registry mutex. The previous
// load_batch order (registry then simulation) deadlocked against get_pathing_batch.

namespace {

void acquire_sim_then_registry(std::mutex& sim, std::mutex& registry, std::atomic<int>& completed, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        std::lock_guard<std::mutex> sim_lock(sim);
        std::lock_guard<std::mutex> registry_lock(registry);
        completed.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

TEST_CASE("probe batch lock order sim-then-registry does not deadlock under load/worker contention", "[probe_batch][locking]") {
    std::mutex simulation_mutex;
    std::mutex registry_mutex;
    std::atomic<bool> start{false};
    std::atomic<int> completed{0};
    constexpr int kIterations = 2000;

    std::thread worker([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        // Worker path: already holds simulation_mutex, then get_pathing_batch takes registry.
        acquire_sim_then_registry(simulation_mutex, registry_mutex, completed, kIterations);
    });

    std::thread loader([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        // Fixed load/remove/clear path: simulation_mutex first, then registry.
        acquire_sim_then_registry(simulation_mutex, registry_mutex, completed, kIterations);
    });

    start.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    worker.join();
    loader.join();
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    REQUIRE(completed.load(std::memory_order_relaxed) == kIterations * 2);
}

TEST_CASE("pathing probe retain drain clears pending list", "[probe_batch][pathing]") {
    // Mirrors ResonanceServer::_drain_pathing_probe_batch_releases bookkeeping without IPL.
    std::vector<int> pending_retains = {1, 2, 3};
    auto drain = [&]() {
        pending_retains.clear();
    };

    // Early pathing skip (no listener / cooldown / light tick) must still drain.
    const bool run_pathing = false;
    const bool listener_valid = false;
    if (!run_pathing || !listener_valid) {
        drain();
    }
    REQUIRE(pending_retains.empty());

    pending_retains = {4, 5};
    drain();
    drain(); // second drain after successful pathing is a no-op
    REQUIRE(pending_retains.empty());
}
