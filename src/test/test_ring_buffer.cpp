#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ring_buffer.h"
#include <cstring>

using namespace godot;

TEST_CASE("RingBuffer resize resets state", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(16);
    REQUIRE(rb.get_available_read() == 0);
    REQUIRE(rb.get_available_write() == 16);

    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(data, 4);
    REQUIRE(rb.get_available_read() == 4);

    rb.resize(32);
    REQUIRE(rb.get_available_read() == 0);
    REQUIRE(rb.get_available_write() == 32);
}

TEST_CASE("RingBuffer write and read normal usage", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(16);

    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(in, 4);
    REQUIRE(rb.get_available_read() == 4);
    REQUIRE(rb.get_available_write() == 12);

    float out[4] = {};
    rb.read(out, 4);
    REQUIRE(rb.get_available_read() == 0);
    REQUIRE(rb.get_available_write() == 16);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
    REQUIRE(out[2] == 3.0f);
    REQUIRE(out[3] == 4.0f);
}

TEST_CASE("RingBuffer wrap-around when full", "[ring_buffer]") {
    RingBuffer<int> rb;
    rb.resize(4);

    int in1[2] = {1, 2};
    int in2[2] = {3, 4};
    rb.write(in1, 2);
    rb.write(in2, 2);
    REQUIRE(rb.get_available_read() == 4);
    REQUIRE(rb.get_available_write() == 0);

    int out[4] = {};
    rb.read(out, 4);
    REQUIRE(out[0] == 1);
    REQUIRE(out[1] == 2);
    REQUIRE(out[2] == 3);
    REQUIRE(out[3] == 4);
    REQUIRE(rb.get_available_read() == 0);

    // Write again to trigger wrap
    int in3[2] = {5, 6};
    rb.write(in3, 2);
    rb.read(out, 2);
    REQUIRE(out[0] == 5);
    REQUIRE(out[1] == 6);
}

TEST_CASE("RingBuffer empty has zero available read", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(8);
    REQUIRE(rb.get_available_read() == 0);
    REQUIRE(rb.get_available_write() == 8);

    float out[4];
    rb.read(out, 4); // Read on empty - should not crash
    REQUIRE(rb.get_available_read() == 0);
}

TEST_CASE("RingBuffer clear resets state", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(8);
    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(in, 4);
    REQUIRE(rb.get_available_read() == 4);

    rb.clear();
    REQUIRE(rb.get_available_read() == 0);
    REQUIRE(rb.get_available_write() == 8);

    float out[4];
    rb.read(out, 4);
    REQUIRE(rb.get_available_read() == 0);
}

TEST_CASE("RingBuffer write clamps to available space", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(4);
    float in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const size_t written = rb.write(in, 8); // Only 4 should be written
    REQUIRE(written == 4);
    REQUIRE(rb.get_available_read() == 4);
    REQUIRE(rb.get_available_write() == 0);
    REQUIRE(rb.get_overflow_drop_count() == 4);

    float out[4] = {};
    rb.read(out, 4);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[3] == 4.0f);
}

TEST_CASE("RingBuffer overflow drop counter accumulates across writes", "[ring_buffer]") {
    RingBuffer<int> rb;
    rb.resize(2);
    int in[3] = {1, 2, 3};
    REQUIRE(rb.write(in, 3) == 2);
    REQUIRE(rb.get_overflow_drop_count() == 1);

    int out[2] = {};
    rb.read(out, 2);
    REQUIRE(rb.write(in, 3) == 2);
    REQUIRE(rb.get_overflow_drop_count() == 2);
}

TEST_CASE("RingBuffer clear resets overflow drop counter", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(2);
    float in[4] = {1, 2, 3, 4};
    rb.write(in, 4);
    REQUIRE(rb.get_overflow_drop_count() == 2);
    rb.clear();
    REQUIRE(rb.get_overflow_drop_count() == 0);
}

TEST_CASE("RingBuffer zero capacity write and read are no-ops", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(0);
    REQUIRE(rb.get_available_read() == 0);
    REQUIRE(rb.get_available_write() == 0);

    float in[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    rb.write(in, 4);
    REQUIRE(rb.get_available_read() == 0);

    float out[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.read(out, 4);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
}

TEST_CASE("RingBuffer zero-length write and read are no-ops", "[ring_buffer]") {
    RingBuffer<float> rb;
    rb.resize(8);
    float in[2] = {5.0f, 6.0f};
    rb.write(in, 0);
    REQUIRE(rb.get_available_read() == 0);

    float out[2] = {};
    rb.read(out, 0);
    REQUIRE(rb.get_available_read() == 0);
}
