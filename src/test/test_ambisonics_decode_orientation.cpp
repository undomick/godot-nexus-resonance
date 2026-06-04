#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ambisonics_decode_orientation.h"
#include <cmath>

using namespace resonance;

namespace {

float dot(const IPLVector3& a, const IPLVector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const IPLVector3& v) {
    return std::sqrt(dot(v, v));
}

bool is_finite(const IPLVector3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void set_identity(float m[16]) {
    for (int i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

} // namespace

TEST_CASE("decode orientation with aligned bed and listener is the IPL default basis", "[ambisonics]") {
    float S[16];
    float L[16];
    set_identity(S);
    set_identity(L);

    IPLCoordinateSpace3 out{};
    ambisonics_decode_orientation_row_major(S, L, &out);

    REQUIRE(out.ahead.x == Approx(0.0f).margin(1e-5f));
    REQUIRE(out.ahead.y == Approx(0.0f).margin(1e-5f));
    REQUIRE(out.ahead.z == Approx(-1.0f).margin(1e-5f));

    REQUIRE(out.up.x == Approx(0.0f).margin(1e-5f));
    REQUIRE(out.up.y == Approx(1.0f).margin(1e-5f));
    REQUIRE(out.up.z == Approx(0.0f).margin(1e-5f));

    REQUIRE(out.right.x == Approx(1.0f).margin(1e-5f));
    REQUIRE(out.right.y == Approx(0.0f).margin(1e-5f));
    REQUIRE(out.right.z == Approx(0.0f).margin(1e-5f));

    REQUIRE(out.origin.x == Approx(0.0f).margin(1e-6f));
    REQUIRE(out.origin.y == Approx(0.0f).margin(1e-6f));
    REQUIRE(out.origin.z == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("decode orientation yields an orthonormal basis for a rotated listener", "[ambisonics]") {
    float S[16];
    set_identity(S);

    // Listener rotated 90 deg about its up axis (columns stored at 0/4/8, 1/5/9, 2/6/10).
    float L[16];
    for (int i = 0; i < 16; i++)
        L[i] = 0.0f;
    L[0] = 0.0f;
    L[1] = 0.0f;
    L[2] = -1.0f;
    L[4] = 0.0f;
    L[5] = 1.0f;
    L[6] = 0.0f;
    L[8] = 1.0f;
    L[9] = 0.0f;
    L[10] = 0.0f;
    L[15] = 1.0f;

    IPLCoordinateSpace3 out{};
    ambisonics_decode_orientation_row_major(S, L, &out);

    REQUIRE(length(out.ahead) == Approx(1.0f).margin(1e-4f));
    REQUIRE(length(out.up) == Approx(1.0f).margin(1e-4f));
    REQUIRE(length(out.right) == Approx(1.0f).margin(1e-4f));

    REQUIRE(dot(out.ahead, out.up) == Approx(0.0f).margin(1e-4f));
    REQUIRE(dot(out.ahead, out.right) == Approx(0.0f).margin(1e-4f));
    REQUIRE(dot(out.up, out.right) == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("decode orientation tolerates degenerate matrices without NaN", "[ambisonics]") {
    float S[16] = {0};
    float L[16] = {0};

    IPLCoordinateSpace3 out{};
    ambisonics_decode_orientation_row_major(S, L, &out);

    REQUIRE(is_finite(out.ahead));
    REQUIRE(is_finite(out.up));
    REQUIRE(is_finite(out.right));
}

TEST_CASE("decode orientation ignores a null output pointer", "[ambisonics]") {
    float S[16];
    float L[16];
    set_identity(S);
    set_identity(L);
    ambisonics_decode_orientation_row_major(S, L, nullptr);
    SUCCEED();
}
