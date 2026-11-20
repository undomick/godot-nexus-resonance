#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_exit_pin_registry.h"

#include <vector>

using resonance::ExitPinRegistry;

TEST_CASE("pin_survivors pins every object still tracked", "[exit_pin_registry]") {
    int playback = 0;
    int effect = 0;
    ExitPinRegistry<int> registry;
    registry.track(&playback);
    registry.track(&effect);

    std::vector<int*> pinned;
    registry.pin_survivors([&](int* obj) { pinned.push_back(obj); });

    REQUIRE(pinned.size() == 2);
    REQUIRE_THAT(pinned, Catch::Matchers::UnorderedEquals(std::vector<int*>{&playback, &effect}));
}

TEST_CASE("pin_survivors skips objects untracked before exit", "[exit_pin_registry]") {
    int freed_playback = 0;
    int live_effect = 0;
    ExitPinRegistry<int> registry;
    registry.track(&freed_playback);
    registry.track(&live_effect);
    registry.untrack(&freed_playback);

    std::vector<int*> pinned;
    registry.pin_survivors([&](int* obj) { pinned.push_back(obj); });

    REQUIRE(pinned == std::vector<int*>{&live_effect});
}
