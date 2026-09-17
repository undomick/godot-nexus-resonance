#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_energy_field_query_policy.h"

using namespace resonance;

TEST_CASE("energy field pack skip on null empty or missing data", "[energy_field][query]") {
    REQUIRE(energy_field_pack_skip(false, 1, 8, true));
    REQUIRE(energy_field_pack_skip(true, 0, 8, true));
    REQUIRE(energy_field_pack_skip(true, 1, 0, true));
    REQUIRE(energy_field_pack_skip(true, 1, 8, false));
    REQUIRE_FALSE(energy_field_pack_skip(true, 1, 8, true));
}

TEST_CASE("energy field sample count is channels times 3 bands times bins", "[energy_field][query]") {
    REQUIRE(energy_field_sample_count(0, 16) == 0);
    REQUIRE(energy_field_sample_count(1, 0) == 0);
    REQUIRE(energy_field_sample_count(1, 8) == 24);
    REQUIRE(energy_field_sample_count(4, 16) == 192);
}

TEST_CASE("energy field total sums only positive samples", "[energy_field][query]") {
    REQUIRE(energy_field_total_from_samples(nullptr, 4) == 0.0f);
    const float zeros[] = {0.0f, 0.0f};
    REQUIRE(energy_field_total_from_samples(zeros, 2) == 0.0f);
    const float mixed[] = {0.25f, -1.0f, 0.75f, 0.0f};
    REQUIRE(energy_field_total_from_samples(mixed, 4) == Approx(1.0f));
}

TEST_CASE("energy field IR reconstruct gated by request and total", "[energy_field][query]") {
    REQUIRE_FALSE(energy_field_should_reconstruct_ir(false, 1.0f));
    REQUIRE_FALSE(energy_field_should_reconstruct_ir(true, 0.0f));
    REQUIRE_FALSE(energy_field_should_reconstruct_ir(true, kEnergyFieldReconstructMinTotal));
    REQUIRE(energy_field_should_reconstruct_ir(true, 0.01f));
    REQUIRE_FALSE(energy_field_reconstruct_ir_args_ok(false, true, true));
    REQUIRE_FALSE(energy_field_reconstruct_ir_args_ok(true, false, true));
    REQUIRE_FALSE(energy_field_reconstruct_ir_args_ok(true, true, false));
    REQUIRE(energy_field_reconstruct_ir_args_ok(true, true, true));
}

TEST_CASE("probe query fail reasons are distinct SSOT strings", "[energy_field][query]") {
    REQUIRE(kProbeQueryErrNoContext != kProbeQueryErrProbeDataMissing);
    REQUIRE(kProbeQueryErrEnergyFieldCreateFailed != kProbeQueryErrNoNeighborsInRadius);
    REQUIRE(kProbeQueryErrInvalidContextOrProbeData[0] != '\0');
}

namespace {

struct FakeField {
    int id = 0;
};

int g_live_fields = 0;

bool fake_create(FakeField& out, bool ok) {
    if (!ok)
        return false;
    out.id = ++g_live_fields;
    return true;
}

void fake_release(FakeField* p) {
    if (!p || p->id == 0)
        return;
    --g_live_fields;
    p->id = 0;
}

struct FakeGuard {
    FakeField* ptr = nullptr;
    ~FakeGuard() {
        if (ptr)
            fake_release(ptr);
    }
};

} // namespace

TEST_CASE("energy field pair: second create fail still releases first", "[energy_field][query][leak]") {
    g_live_fields = 0;
    FakeField accum{};
    FakeField temp{};
    {
        if (!fake_create(accum, true))
            FAIL("first create");
        FakeGuard accum_guard{&accum};
        REQUIRE(g_live_fields == 1);
        if (!fake_create(temp, false)) {
            REQUIRE(g_live_fields == 1);
        }
    }
    REQUIRE(g_live_fields == 0);
    REQUIRE(accum.id == 0);
}

TEST_CASE("energy field pair: both creates success release both on scope exit", "[energy_field][query][leak]") {
    g_live_fields = 0;
    FakeField accum{};
    FakeField temp{};
    {
        REQUIRE(fake_create(accum, true));
        FakeGuard accum_guard{&accum};
        REQUIRE(fake_create(temp, true));
        FakeGuard temp_guard{&temp};
        REQUIRE(g_live_fields == 2);
    }
    REQUIRE(g_live_fields == 0);
}
