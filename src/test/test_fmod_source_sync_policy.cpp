#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_fmod_source_sync_policy.h"

using namespace resonance;

TEST_CASE("fmod source sync enqueues when try_update fails", "[fmod][sources]") {
    REQUIRE(fmod_source_sync_should_enqueue_on_try_update_failure(false));
    REQUIRE_FALSE(fmod_source_sync_should_enqueue_on_try_update_failure(true));
}
