#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_owner_link.h"
#include "../resonance_reverb_send_policy.h"

#include <atomic>
#include <memory>
#include <thread>

using resonance::OwnerLink;
using resonance::reverb_send_child_should_play;

TEST_CASE("reverb child plays while the parent plays", "[reverb_send]") {
    REQUIRE(reverb_send_child_should_play(true, false));
    REQUIRE(reverb_send_child_should_play(true, true));
}

TEST_CASE("reverb child drains the tail after the parent stops", "[reverb_send]") {
    REQUIRE(reverb_send_child_should_play(false, true));
    REQUIRE_FALSE(reverb_send_child_should_play(false, false));
}

namespace {

struct FakeParent {
    std::atomic<bool> playing_for_audio{true};
};

bool mix_like_reverb_child(OwnerLink<FakeParent>& link) {
    const auto lease = link.lease();
    const FakeParent* parent = lease.get();
    if (!parent)
        return false;
    return reverb_send_child_should_play(parent->playing_for_audio.load(std::memory_order_acquire), false);
}

} // namespace

TEST_CASE("reverb child never reads a parent freed after tree exit", "[reverb_send]") {
    constexpr int kRounds = 200;
    for (int round = 0; round < kRounds; ++round) {
        auto parent = std::make_unique<FakeParent>();
        auto link = std::make_shared<OwnerLink<FakeParent>>();
        link->set(parent.get());
        std::atomic<bool> stop_mixing{false};
        std::thread audio([&, link]() {
            while (!stop_mixing.load(std::memory_order_acquire))
                mix_like_reverb_child(*link);
        });
        link->orphan();
        parent.reset();
        REQUIRE_FALSE(mix_like_reverb_child(*link));
        stop_mixing.store(true, std::memory_order_release);
        audio.join();
    }
}
