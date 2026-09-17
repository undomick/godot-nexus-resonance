#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflection_mixer_policy.h"

using namespace resonance;

TEST_CASE("reflection mixer release wait finishes when readers reach zero", "[reflection_mixer][policy]") {
    REQUIRE(reflection_mixer_release_wait_finished(0, 0));
    REQUIRE(reflection_mixer_release_wait_finished(0, kReflectionMixerReleaseWaitTimeoutMs));
    REQUIRE_FALSE(reflection_mixer_release_wait_finished(1, 0));
}

TEST_CASE("reflection mixer release wait times out with stuck readers", "[reflection_mixer][policy]") {
    REQUIRE(reflection_mixer_release_wait_finished(2, kReflectionMixerReleaseWaitTimeoutMs));
    REQUIRE_FALSE(reflection_mixer_release_wait_finished(2, kReflectionMixerReleaseWaitTimeoutMs - 1));
}

TEST_CASE("reflection mixer release is safe only with zero readers", "[reflection_mixer][policy]") {
    REQUIRE(reflection_mixer_safe_to_release(0));
    REQUIRE_FALSE(reflection_mixer_safe_to_release(1));
}

TEST_CASE("reflection mixer defers release after timeout with stuck readers", "[reflection_mixer][policy]") {
    REQUIRE(reflection_mixer_should_defer_release(1, kReflectionMixerReleaseWaitTimeoutMs));
    REQUIRE_FALSE(reflection_mixer_should_defer_release(1, kReflectionMixerReleaseWaitTimeoutMs - 1));
    REQUIRE_FALSE(reflection_mixer_should_defer_release(0, kReflectionMixerReleaseWaitTimeoutMs));
}

TEST_CASE("reflection mixer requests deferred drain when last reader drops", "[reflection_mixer][policy][f-08]") {
    REQUIRE(reflection_mixer_should_request_deferred_drain(1));
    REQUIRE_FALSE(reflection_mixer_should_request_deferred_drain(2));
    REQUIRE_FALSE(reflection_mixer_should_request_deferred_drain(0));
}

TEST_CASE("reflection mixer deferred release cap evicts oldest only when readers drained", "[reflection_mixer][policy]") {
    REQUIRE_FALSE(reflection_mixer_deferred_at_capacity(kMaxDeferredReflectionMixerReleases - 1));
    REQUIRE(reflection_mixer_deferred_at_capacity(kMaxDeferredReflectionMixerReleases));
    REQUIRE(reflection_mixer_defer_should_evict_oldest(kMaxDeferredReflectionMixerReleases, 0));
    REQUIRE_FALSE(reflection_mixer_defer_should_evict_oldest(kMaxDeferredReflectionMixerReleases, 1));
    REQUIRE(reflection_mixer_defer_exceeds_soft_cap(kMaxDeferredReflectionMixerReleases, 1));
    REQUIRE_FALSE(reflection_mixer_defer_exceeds_soft_cap(kMaxDeferredReflectionMixerReleases - 1, 1));
}
