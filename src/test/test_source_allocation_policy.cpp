#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include "../resonance_source_handle_policy.h"
#include <climits>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace resonance;

TEST_CASE("source handle fits cache within user max", "[source_allocation]") {
    REQUIRE(source_handle_fits_cache(0));
    REQUIRE(source_handle_fits_cache(kMaxSimulationSourcesUserMax - 1));
    REQUIRE_FALSE(source_handle_fits_cache(-1));
    REQUIRE_FALSE(source_handle_fits_cache(kMaxSimulationSourcesUserMax));
}

TEST_CASE("source count limit matches max_simulation_sources", "[source_allocation]") {
    REQUIRE(source_count_at_simulation_limit(32, 32));
    REQUIRE_FALSE(source_count_at_simulation_limit(31, 32));
    REQUIRE(source_count_at_simulation_limit(8, 8));
}

TEST_CASE("sequential source handle allocation stops at cache capacity", "[source_allocation]") {
    REQUIRE(can_alloc_sequential_source_handle(0));
    REQUIRE(can_alloc_sequential_source_handle(kMaxSimulationSourcesUserMax - 1));
    REQUIRE_FALSE(can_alloc_sequential_source_handle(kMaxSimulationSourcesUserMax));
}

TEST_CASE("deferred recycle cannot grow handle ids past cache capacity", "[source_allocation]") {
    struct CappedDeferRecycleManager {
        int32_t next_handle = 0;
        std::priority_queue<int32_t, std::vector<int32_t>, std::greater<int32_t>> free_handles;
        std::unordered_map<int32_t, int> items;
        std::unordered_set<int32_t> deferred_recycle;

        int32_t alloc_handle() {
            if (!free_handles.empty()) {
                int32_t h = free_handles.top();
                free_handles.pop();
                return h;
            }
            if (!can_alloc_sequential_source_handle(next_handle))
                return -1;
            if (next_handle >= INT32_MAX)
                return -1;
            return next_handle++;
        }

        int32_t add(int value) {
            int32_t h = alloc_handle();
            if (h < 0)
                return -1;
            items[h] = value;
            return h;
        }

        void remove(int32_t handle, bool recycle) {
            items.erase(handle);
            if (recycle)
                free_handles.push(handle);
            else
                deferred_recycle.insert(handle);
        }
    };

    CappedDeferRecycleManager m;
    for (int i = 0; i < kMaxSimulationSourcesUserMax; i++) {
        int32_t h = m.add(i);
        REQUIRE(h >= 0);
        m.remove(h, false);
    }
    REQUIRE(m.add(999) == -1);
}

TEST_CASE("reflection output scan ignores invalid handles", "[source_allocation]") {
    std::vector<int32_t> handles = {-1, kMaxSimulationSourcesUserMax, 42};
    const bool active = any_source_has_reflection_outputs(handles, [](int32_t h) -> uint8_t {
        return h == 42 ? 0u : 1u;
    });
    REQUIRE_FALSE(active);
}

TEST_CASE("reflection output scan detects valid handle with reflections", "[source_allocation]") {
    std::vector<int32_t> handles = {-1, 3, kMaxSimulationSourcesUserMax};
    const bool active = any_source_has_reflection_outputs(handles, [](int32_t h) -> uint8_t {
        return h == 3 ? 1u : 0u;
    });
    REQUIRE(active);
}
