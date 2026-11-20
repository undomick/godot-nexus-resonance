#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "resonance_reflections_pass_gate.h"
#include "resonance_writer_preferring_shared_mutex.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using resonance::DirectPassLock;
using resonance::DirectPassMode;
using resonance::ReflectionsPassGate;

namespace {

constexpr auto kTimeout = std::chrono::seconds(5);

template <typename Pred>
bool wait_until(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

auto never_stop() {
    return [] { return false; };
}

} // namespace

TEST_CASE("direct pass takes the simulation mutex when no reflections pass runs", "[reflections_pass_gate]") {
    std::mutex simulation_mutex;
    ReflectionsPassGate gate;
    DirectPassLock lock = gate.lock_direct_pass(simulation_mutex, never_stop());
    REQUIRE(lock.mode == DirectPassMode::Full);
    REQUIRE(lock.simulation.owns_lock());
    REQUIRE_FALSE(lock.direct_only.owns_lock());
}

TEST_CASE("direct passes keep running while a reflections pass holds the simulation mutex", "[reflections_pass_gate]") {
    std::mutex simulation_mutex;
    ReflectionsPassGate gate;
    std::atomic<bool> running{false};
    std::atomic<bool> release{false};
    std::atomic<int> direct_only_passes{0};

    std::thread reflections([&] {
        std::lock_guard<std::mutex> sim_lock(simulation_mutex);
        gate.begin_run();
        running.store(true);
        wait_until([&] { return release.load(); });
        gate.end_run();
        running.store(false);
    });
    REQUIRE(wait_until([&] { return running.load(); }));

    std::thread direct([&] {
        for (int i = 0; i < 5; ++i) {
            DirectPassLock lock = gate.lock_direct_pass(simulation_mutex, never_stop());
            if (lock.mode == DirectPassMode::DirectOnly)
                direct_only_passes.fetch_add(1);
        }
    });
    direct.join();
    REQUIRE(direct_only_passes.load() == 5);

    release.store(true);
    reflections.join();
    DirectPassLock after = gate.lock_direct_pass(simulation_mutex, never_stop());
    REQUIRE(after.mode == DirectPassMode::Full);
}

TEST_CASE("a commit requested during a reflections pass waits for the pass to end", "[reflections_pass_gate]") {
    std::mutex simulation_mutex;
    ReflectionsPassGate gate;
    std::atomic<bool> running{false};
    std::atomic<bool> release{false};
    std::atomic<bool> pass_ended{false};
    std::atomic<bool> committed{false};
    std::atomic<bool> committed_after_pass{false};

    std::thread reflections([&] {
        std::lock_guard<std::mutex> sim_lock(simulation_mutex);
        gate.begin_run();
        running.store(true);
        wait_until([&] { return release.load(); });
        gate.end_run();
        pass_ended.store(true);
    });
    REQUIRE(wait_until([&] { return running.load(); }));

    std::thread committer([&] {
        std::lock_guard<std::mutex> sim_lock(simulation_mutex);
        committed_after_pass.store(pass_ended.load());
        committed.store(true);
    });

    DirectPassLock during = gate.lock_direct_pass(simulation_mutex, never_stop());
    REQUIRE(during.mode == DirectPassMode::DirectOnly);
    during = DirectPassLock{};
    REQUIRE_FALSE(committed.load());

    release.store(true);
    reflections.join();
    committer.join();
    REQUIRE(committed.load());
    REQUIRE(committed_after_pass.load());
}

TEST_CASE("direct pass retries while another thread briefly holds the simulation mutex", "[reflections_pass_gate]") {
    std::mutex simulation_mutex;
    ReflectionsPassGate gate;
    std::atomic<bool> held{false};

    std::thread holder([&] {
        std::lock_guard<std::mutex> sim_lock(simulation_mutex);
        held.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    REQUIRE(wait_until([&] { return held.load(); }));

    DirectPassLock lock = gate.lock_direct_pass(simulation_mutex, never_stop());
    REQUIRE(lock.mode == DirectPassMode::Full);
    holder.join();
}

TEST_CASE("direct pass gives up once asked to stop", "[reflections_pass_gate]") {
    std::mutex simulation_mutex;
    ReflectionsPassGate gate;
    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    std::atomic<int> polls{0};
    DirectPassLock lock = gate.lock_direct_pass(simulation_mutex, [&] { return polls.fetch_add(1) >= 3; });
    REQUIRE(lock.mode == DirectPassMode::None);
    REQUIRE_FALSE(lock.simulation.owns_lock());
    REQUIRE_FALSE(lock.direct_only.owns_lock());
}

TEST_CASE("a reflections pass starts only from a full direct pass while the reflections worker is idle",
          "[reflections_pass_gate]") {
    REQUIRE(resonance::reflections_pass_may_start(DirectPassMode::Full, false));
    REQUIRE_FALSE(resonance::reflections_pass_may_start(DirectPassMode::Full, true));
    REQUIRE_FALSE(resonance::reflections_pass_may_start(DirectPassMode::DirectOnly, false));
    REQUIRE_FALSE(resonance::reflections_pass_may_start(DirectPassMode::None, false));
}

TEST_CASE("bake exclusive context lock does not deadlock against both workers", "[reflections_pass_gate][locking]") {
    resonance::WriterPreferringSharedMutex context_mutex;
    std::mutex simulation_mutex;
    ReflectionsPassGate gate;
    std::atomic<int> bakes{0};
    std::atomic<int> direct_passes{0};
    std::atomic<int> reflections_passes{0};
    constexpr int kIterations = 200;

    std::thread reflections([&] {
        for (int i = 0; i < kIterations; ++i) {
            std::shared_lock<resonance::WriterPreferringSharedMutex> context_lock(context_mutex);
            std::lock_guard<std::mutex> sim_lock(simulation_mutex);
            gate.begin_run();
            std::this_thread::yield();
            gate.end_run();
            reflections_passes.fetch_add(1);
        }
    });
    std::thread direct([&] {
        for (int i = 0; i < kIterations; ++i) {
            std::shared_lock<resonance::WriterPreferringSharedMutex> context_lock(context_mutex);
            DirectPassLock lock = gate.lock_direct_pass(simulation_mutex, never_stop());
            if (lock.mode != DirectPassMode::None)
                direct_passes.fetch_add(1);
        }
    });
    std::thread bake([&] {
        for (int i = 0; i < kIterations; ++i) {
            std::unique_lock<resonance::WriterPreferringSharedMutex> context_lock(context_mutex);
            std::lock_guard<std::mutex> sim_lock(simulation_mutex);
            bakes.fetch_add(1);
        }
    });

    reflections.join();
    bake.join();
    direct.join();
    REQUIRE(reflections_passes.load() == kIterations);
    REQUIRE(bakes.load() == kIterations);
    REQUIRE(direct_passes.load() == kIterations);
}

TEST_CASE("a waiting exclusive locker keeps new shared lockers out", "[reflections_pass_gate][locking]") {
    resonance::WriterPreferringSharedMutex context_mutex;
    std::shared_lock<resonance::WriterPreferringSharedMutex> first_reader(context_mutex);
    std::atomic<bool> writer_done{false};

    std::thread writer([&] {
        std::unique_lock<resonance::WriterPreferringSharedMutex> exclusive(context_mutex);
        writer_done.store(true);
    });
    REQUIRE(wait_until([&] {
        if (!context_mutex.try_lock_shared())
            return true;
        context_mutex.unlock_shared();
        return false;
    }));
    REQUIRE_FALSE(writer_done.load());

    first_reader.unlock();
    writer.join();
    REQUIRE(writer_done.load());
}
