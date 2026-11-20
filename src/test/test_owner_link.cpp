#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_owner_link.h"

#include <atomic>
#include <chrono>
#include <thread>

using resonance::OwnerLink;

TEST_CASE("a lease sees the owner until it is orphaned", "[owner_link]") {
    int owner = 7;
    OwnerLink<int> link;
    link.set(&owner);
    {
        const auto lease = link.lease();
        REQUIRE(lease.get() == &owner);
    }
    link.orphan();
    const auto lease = link.lease();
    REQUIRE(lease.get() == nullptr);
}

TEST_CASE("orphan waits until an open lease ends", "[owner_link]") {
    int owner = 7;
    OwnerLink<int> link;
    link.set(&owner);
    std::atomic<bool> orphaned{false};
    std::thread orphaner;
    {
        const auto lease = link.lease();
        orphaner = std::thread([&]() {
            link.orphan();
            orphaned.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE_FALSE(orphaned.load());
        REQUIRE(lease.get() == &owner);
    }
    orphaner.join();
    REQUIRE(orphaned.load());
}
