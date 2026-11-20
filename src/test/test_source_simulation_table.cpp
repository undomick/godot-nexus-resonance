#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_source_simulation_table.h"

using namespace resonance;

namespace {
constexpr std::size_t kHandles = 8;

struct FakeParams {
    int value = 0;
};

struct FakeSource {
    int id = 0;
};

using Table = SourceSimulationTable<kHandles, FakeParams, FakeSource*>;

void create(Table& table, int32_t handle, int initial = 0, bool pathing = false) {
    table.source_created(handle, FakeParams{initial}, pathing, [] {});
}
} // namespace

TEST_CASE("created source waits for attach and is handed to the drain", "[source_simulation_table]") {
    Table table;
    create(table, 2, 7);

    REQUIRE(table.is_attach_pending(2));
    REQUIRE(table.has_pending_lifecycle());

    Table::LifecycleDrain drain = table.take_lifecycle();
    REQUIRE(drain.adds.size() == 1);
    REQUIRE(drain.adds[0].handle == 2);
    REQUIRE(drain.adds[0].initial.value == 7);
    REQUIRE_FALSE(table.has_pending_lifecycle());

    table.attached(2);
    REQUIRE_FALSE(table.is_attach_pending(2));
}

TEST_CASE("fetch state is cleared while attach is already pending and before the drain sees the add",
          "[source_simulation_table]") {
    Table table;
    bool attach_pending_during_clear = false;
    bool add_visible_during_clear = true;
    table.source_created(4, FakeParams{}, false, [&] {
        attach_pending_during_clear = table.is_attach_pending(4);
        add_visible_during_clear = table.has_pending_lifecycle();
    });

    REQUIRE(attach_pending_during_clear);
    REQUIRE_FALSE(add_visible_during_clear);
}

TEST_CASE("destroy before attach cancels the add and frees the handle at once", "[source_simulation_table]") {
    Table table;
    FakeSource source;
    create(table, 1);

    REQUIRE(table.source_destroyed(1, &source) == SourceDestroyOutcome::kReleaseAndRecycleNow);

    REQUIRE_FALSE(table.is_attach_pending(1));
    REQUIRE(table.take_lifecycle().empty());
}

TEST_CASE("destroy after attach queues the remove and recycles only after the drain", "[source_simulation_table]") {
    Table table;
    FakeSource source;
    create(table, 3);
    table.take_lifecycle();
    table.attached(3);

    REQUIRE(table.source_destroyed(3, &source) == SourceDestroyOutcome::kRemoveQueued);

    Table::LifecycleDrain drain = table.take_lifecycle();
    REQUIRE(drain.adds.empty());
    REQUIRE(drain.removes.size() == 1);
    REQUIRE(drain.removes[0] == &source);
    REQUIRE(drain.removed_handles == std::vector<int32_t>{3});
}

TEST_CASE("destroying a source that is already gone only recycles the handle", "[source_simulation_table]") {
    Table table;
    create(table, 5);

    REQUIRE(table.source_destroyed(5, nullptr) == SourceDestroyOutcome::kRecycleNow);
    REQUIRE(table.take_lifecycle().empty());
}

TEST_CASE("an update before attach replaces the initial params of the pending add", "[source_simulation_table]") {
    Table table;
    create(table, 0, 1);

    REQUIRE(table.replace_pending_add_initial(0, FakeParams{9}));
    REQUIRE_FALSE(table.replace_pending_add_initial(6, FakeParams{9}));

    REQUIRE(table.take_lifecycle().adds[0].initial.value == 9);
}

TEST_CASE("shutdown clears attach pending for every handle and moves to a new epoch", "[source_simulation_table]") {
    Table table;
    create(table, 0);
    create(table, 7);
    const uint32_t epoch_before = table.lifecycle_epoch();

    table.bump_lifecycle_epoch();
    table.take_lifecycle();
    table.clear_attach_pending();

    REQUIRE(table.lifecycle_epoch() != epoch_before);
    REQUIRE_FALSE(table.is_attach_pending(0));
    REQUIRE_FALSE(table.is_attach_pending(7));
}

TEST_CASE("handles outside the table are never attach pending", "[source_simulation_table]") {
    Table table;
    create(table, static_cast<int32_t>(kHandles));

    REQUIRE_FALSE(table.is_attach_pending(-1));
    REQUIRE_FALSE(table.is_attach_pending(static_cast<int32_t>(kHandles)));
    REQUIRE(table.take_lifecycle().adds.size() == 1);
}

namespace {
std::mutex simulation_mutex;
const std::unique_lock<std::mutex> simulation_lock(simulation_mutex);
const SimulationAccess kFull(simulation_lock);

DirectPassLock direct_only_pass() {
    DirectPassLock pass;
    pass.mode = DirectPassMode::DirectOnly;
    return pass;
}
const SimulationAccess kDirectOnly(direct_only_pass());
} // namespace

TEST_CASE("access from a direct-only pass allows direct inputs only", "[source_simulation_table]") {
    std::mutex mutex;
    std::lock_guard<std::mutex> held(mutex);
    REQUIRE_FALSE(SimulationAccess(held).direct_only());
    REQUIRE(kDirectOnly.direct_only());

    DirectPassLock full_pass;
    full_pass.mode = DirectPassMode::Full;
    REQUIRE_FALSE(SimulationAccess(full_pass).direct_only());
}

TEST_CASE("queued updates keep only the latest per source", "[source_simulation_table]") {
    Table table;
    table.enqueue_update(1, FakeParams{1});
    table.enqueue_update(1, FakeParams{2});
    table.enqueue_update(3, FakeParams{3});

    std::vector<Table::QueuedUpdate> batch = table.take_updates();

    REQUIRE(batch.size() == 2);
    REQUIRE_FALSE(table.has_queued_updates());
    for (const Table::QueuedUpdate& update : batch)
        REQUIRE(update.params.value == update.handle + (update.handle == 1 ? 1 : 0));
}

TEST_CASE("a requeued update loses to one enqueued while the batch was out", "[source_simulation_table]") {
    Table table;
    table.enqueue_update(2, FakeParams{1});
    table.enqueue_update(4, FakeParams{1});
    const std::vector<Table::QueuedUpdate> batch = table.take_updates();
    table.enqueue_update(2, FakeParams{5});

    table.requeue_not_superseded(batch);

    std::vector<Table::QueuedUpdate> again = table.take_updates();
    REQUIRE(again.size() == 2);
    for (const Table::QueuedUpdate& update : again)
        REQUIRE(update.params.value == (update.handle == 2 ? 5 : 1));
}

TEST_CASE("destroying a source drops its queued update and any taken one", "[source_simulation_table]") {
    Table table;
    FakeSource source;
    create(table, 2);
    table.take_lifecycle();
    table.attached(2);
    table.enqueue_update(2, FakeParams{1});
    const std::vector<Table::QueuedUpdate> taken = table.take_updates();
    table.enqueue_update(2, FakeParams{2});

    table.source_destroyed(2, &source);
    table.requeue_not_superseded(taken);

    REQUIRE_FALSE(table.has_queued_updates());
}

TEST_CASE("an unchanged update is skipped until its snapshot is invalidated", "[source_simulation_table]") {
    Table table;
    REQUIRE(table.begin_update(kFull, 1, FakeParams{3}, false) == SourceUpdateDecision::kApply);
    table.direct_inputs_set(kFull, 1);
    REQUIRE(table.begin_update(kFull, 1, FakeParams{3}, false) == SourceUpdateDecision::kSkipUnchanged);

    table.invalidate_snapshot(1);
    REQUIRE(table.begin_update(kFull, 1, FakeParams{3}, false) == SourceUpdateDecision::kApply);
    REQUIRE(table.begin_update(kFull, 1, FakeParams{3}, false) == SourceUpdateDecision::kSkipUnchanged);

    table.invalidate_all_snapshots();
    REQUIRE(table.begin_update(kFull, 1, FakeParams{3}, false) == SourceUpdateDecision::kApply);
}

TEST_CASE("a dirty attenuation curve forces an unchanged update through", "[source_simulation_table]") {
    Table table;
    table.begin_update(kFull, 1, FakeParams{3}, false);
    REQUIRE(table.begin_update(kFull, 1, FakeParams{3}, true) == SourceUpdateDecision::kApply);
}

TEST_CASE("a direct-only update leaves the wet inputs owed to the next full pass", "[source_simulation_table]") {
    Table table;
    REQUIRE(table.begin_update(kDirectOnly, 5, FakeParams{1}, false) == SourceUpdateDecision::kApply);
    table.direct_inputs_set(kDirectOnly, 5);

    REQUIRE(table.begin_update(kDirectOnly, 5, FakeParams{1}, false) == SourceUpdateDecision::kSkipRequeued);
    REQUIRE(table.begin_update(kFull, 5, FakeParams{1}, false) == SourceUpdateDecision::kApply);
    table.direct_inputs_set(kFull, 5);
    REQUIRE(table.begin_update(kFull, 5, FakeParams{1}, false) == SourceUpdateDecision::kSkipUnchanged);
}

TEST_CASE("the snapshot holds the last applied params until it is forgotten", "[source_simulation_table]") {
    Table table;
    REQUIRE(table.snapshot(kFull, 6) == nullptr);
    table.begin_update(kFull, 6, FakeParams{8}, false);
    REQUIRE(table.snapshot(kFull, 6)->value == 8);

    table.forget_snapshot(kFull, 6);
    REQUIRE(table.snapshot(kFull, 6) == nullptr);
    REQUIRE(table.begin_update(kFull, 6, FakeParams{8}, false) == SourceUpdateDecision::kApply);

    table.clear_snapshots(kFull);
    REQUIRE(table.snapshot(kFull, 6) == nullptr);
}

TEST_CASE("only changed inputs move the inputs generation", "[source_simulation_table]") {
    Table table;
    const uint64_t start = table.inputs_generation(kFull);

    REQUIRE(table.record_inputs(kFull, 2, Table::kDirectInputSlot, 11));
    REQUIRE_FALSE(table.record_inputs(kFull, 2, Table::kDirectInputSlot, 11));
    REQUIRE(table.record_inputs(kFull, 2, 2, 11));
    REQUIRE(table.inputs_generation(kFull) == start + 2);

    REQUIRE(table.record_inputs(kDirectOnly, 2, Table::kDirectInputSlot, 12));
    REQUIRE(table.inputs_generation(kFull) == start + 3);
}

TEST_CASE("a source added on a recycled handle does not inherit the old inputs", "[source_simulation_table]") {
    Table table;
    table.record_inputs(kFull, 3, Table::kDirectInputSlot, 11);

    table.forget_inputs(kFull, 3);

    REQUIRE(table.record_inputs(kFull, 3, Table::kDirectInputSlot, 11));
}

namespace {
void attach(Table& table, int32_t handle, bool pathing = false) {
    create(table, handle, 0, pathing);
    table.take_lifecycle();
    table.attached(handle);
}
} // namespace

TEST_CASE("a new source takes part in direct and reflections until its first update", "[source_simulation_table]") {
    Table table;
    create(table, 1, 0, true);
    create(table, 2);

    REQUIRE(table.takes_part_in(SourcePass::kDirect, 1));
    REQUIRE(table.takes_part_in(SourcePass::kReflections, 1));
    REQUIRE_FALSE(table.takes_part_in(SourcePass::kRealtimeReflections, 1));
    REQUIRE(table.takes_part_in(SourcePass::kPathing, 1));
    REQUIRE_FALSE(table.takes_part_in(SourcePass::kPathing, 2));
    REQUIRE(table.is_reflections_pending(1));
    REQUIRE_FALSE(table.is_direct_ready(1));
}

TEST_CASE("a destroyed source takes part in nothing", "[source_simulation_table]") {
    Table table;
    FakeSource source;
    attach(table, 3, true);

    table.source_destroyed(3, &source);

    REQUIRE_FALSE(table.takes_part_in(SourcePass::kDirect, 3));
    REQUIRE_FALSE(table.takes_part_in(SourcePass::kReflections, 3));
    REQUIRE_FALSE(table.takes_part_in(SourcePass::kPathing, 3));
    REQUIRE_FALSE(table.is_reflections_pending(3));
}

TEST_CASE("a recycled handle inherits no readiness, pending result, inputs or snapshot", "[source_simulation_table]") {
    Table table;
    FakeSource source;
    attach(table, 4);
    table.direct_published(4, table.readiness_generation());
    table.reflections_pass_ran(4);
    table.begin_update(kFull, 4, FakeParams{2}, false);
    table.record_inputs(kFull, 4, Table::kDirectInputSlot, 11);
    REQUIRE(table.is_direct_ready(4));

    table.source_destroyed(4, &source);
    for (int32_t removed : table.take_lifecycle().removed_handles) {
        table.forget_snapshot(kFull, removed);
        table.clear_pass_outputs(removed);
    }
    create(table, 4);
    for (const Table::PendingAdd& add : table.take_lifecycle().adds) {
        table.forget_inputs(kFull, add.handle);
        table.attached(add.handle);
    }

    REQUIRE_FALSE(table.is_direct_ready(4));
    REQUIRE(table.is_reflections_pending(4));
    REQUIRE(table.snapshot(kFull, 4) == nullptr);
    REQUIRE(table.record_inputs(kFull, 4, Table::kDirectInputSlot, 11));
}

TEST_CASE("a stopped source leaves the reflections pass and rejoins on restart", "[source_simulation_table]") {
    Table table;
    attach(table, 5);

    table.set_direct_output(5, false);
    table.set_wet_outputs(kFull, 5, false, false, false);
    REQUIRE_FALSE(table.takes_part_in(SourcePass::kDirect, 5));
    REQUIRE_FALSE(table.takes_part_in(SourcePass::kReflections, 5));
    REQUIRE_FALSE(table.is_reflections_pending(5));

    table.set_direct_output(5, true);
    table.set_wet_outputs(kFull, 5, true, true, false);
    REQUIRE(table.takes_part_in(SourcePass::kDirect, 5));
    REQUIRE(table.takes_part_in(SourcePass::kReflections, 5));
    REQUIRE(table.takes_part_in(SourcePass::kRealtimeReflections, 5));
}

TEST_CASE("a reflections pass releases attached sources only", "[source_simulation_table]") {
    Table table;
    attach(table, 1);
    create(table, 2);

    table.reflections_pass_ran(1);
    table.reflections_pass_ran(2);

    REQUIRE_FALSE(table.is_reflections_pending(1));
    REQUIRE(table.is_reflections_pending(2));
}

TEST_CASE("a scene reload clears readiness but keeps pass outputs and inputs", "[source_simulation_table]") {
    Table table;
    attach(table, 6);
    table.direct_published(6, table.readiness_generation());
    table.record_inputs(kFull, 6, Table::kDirectInputSlot, 11);
    const uint32_t generation_before_reload = table.readiness_generation();

    table.scene_reloaded();

    REQUIRE_FALSE(table.is_direct_ready(6));
    REQUIRE(table.takes_part_in(SourcePass::kDirect, 6));
    REQUIRE_FALSE(table.record_inputs(kFull, 6, Table::kDirectInputSlot, 11));
    table.direct_published(6, generation_before_reload);
    REQUIRE_FALSE(table.is_direct_ready(6));
    table.direct_published(6, table.readiness_generation());
    REQUIRE(table.is_direct_ready(6));
}

TEST_CASE("shutdown leaves no queued work and no pending reflections", "[source_simulation_table]") {
    Table table;
    attach(table, 0);
    create(table, 1);
    table.enqueue_update(0, FakeParams{1});
    table.begin_update(kFull, 0, FakeParams{1}, false);

    table.bump_lifecycle_epoch();
    table.scene_reloaded();
    table.clear_all_reflections_pending();
    table.clear_queued_updates();
    table.take_lifecycle();
    table.clear_attach_pending();
    table.clear_snapshots(kFull);

    REQUIRE_FALSE(table.has_queued_updates());
    REQUIRE_FALSE(table.has_pending_lifecycle());
    REQUIRE_FALSE(table.is_reflections_pending(0));
    REQUIRE_FALSE(table.is_reflections_pending(1));
    REQUIRE_FALSE(table.is_attach_pending(1));
    REQUIRE(table.snapshot(kFull, 0) == nullptr);
}
