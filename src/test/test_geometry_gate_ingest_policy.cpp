#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_geometry_gate_ingest_policy.h"

using namespace resonance;

TEST_CASE("geometry gate pauses input ingest while decode is held", "[playback][geometry-gate][ingest]") {
    REQUIRE(geometry_gate_should_pause_input_ingest(1, false));
    REQUIRE_FALSE(geometry_gate_should_pause_input_ingest(1, true));
    REQUIRE_FALSE(geometry_gate_should_pause_input_ingest(-1, false));
}

TEST_CASE("geometry gate holds decoder advance while decode is held", "[playback][geometry-gate][ingest]") {
    REQUIRE(geometry_gate_should_hold_decoder_advance(1, false));
    REQUIRE_FALSE(geometry_gate_should_hold_decoder_advance(1, true));
    REQUIRE_FALSE(geometry_gate_should_hold_decoder_advance(-1, false));
}

TEST_CASE("geometry gate pauses ambisonic ingest without source handle", "[playback][geometry-gate][ambisonic]") {
    REQUIRE(geometry_gate_should_pause_ambisonic_input_ingest(false));
    REQUIRE_FALSE(geometry_gate_should_pause_ambisonic_input_ingest(true));
}

TEST_CASE("geometry gate holds ambisonic decoder advance without source handle", "[playback][geometry-gate][ambisonic]") {
    REQUIRE(geometry_gate_should_hold_ambisonic_decoder_advance(false));
    REQUIRE_FALSE(geometry_gate_should_hold_ambisonic_decoder_advance(true));
    REQUIRE(geometry_gate_should_hold_ambisonic_decoder_advance(false) ==
            geometry_gate_should_pause_ambisonic_input_ingest(false));
}
