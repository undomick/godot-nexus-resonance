#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_listener_sync_policy.h"

using namespace resonance;

TEST_CASE("only active listener node may publish validity", "[listener][sync]") {
    REQUIRE(listener_node_should_publish_validity(true));
    REQUIRE_FALSE(listener_node_should_publish_validity(false));
}

TEST_CASE("camera fallback marks listener valid for pathing and baked probe", "[listener][sync]") {
    REQUIRE(camera_fallback_listener_validity());
}

TEST_CASE("no driving listener uses camera fallback", "[listener][sync]") {
    REQUIRE(listener_sync_should_use_camera_fallback(false));
    REQUIRE_FALSE(listener_sync_should_use_camera_fallback(true));
}

TEST_CASE("no driver validity follows camera fallback or invalid without camera", "[listener][sync]") {
    REQUIRE(listener_sync_validity_when_no_driver(true));
    REQUIRE_FALSE(listener_sync_validity_when_no_driver(false));
}

TEST_CASE("listener sync tick call site primary_runtime_syncs_viewport_listeners matrix", "[listener][sync]") {
    // resonance_listener.cpp: listener_node_should_push_pose(ResonanceRuntime::primary_runtime_syncs_viewport_listeners())
    const auto call_site_should_push = [](bool primary_runtime_syncs_viewport) {
        return listener_node_should_push_pose(primary_runtime_syncs_viewport);
    };
    REQUIRE_FALSE(call_site_should_push(true));
    REQUIRE(call_site_should_push(false));
}
