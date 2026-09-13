// Runtime Evolution Fabric - deterministic seeded property tests and
// deterministic race (interleaving) tests.
//
// The randomized model is fully reproducible: every case prints its seed via
// REF_NOTE and derives all choices from that seed.
#include <random>
#include <string>
#include <vector>

#include "support/fabric_harness.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace ref;
using reftest::Fabric;
using reftest::TempDir;
using reftest::TestWorker;

namespace {

// Invariants that must hold after every randomized step.
void check_invariants(Fabric& fabric, const char* stage) {
  const DurableState state = fabric->durable_state_copy();
  // 1. A retired generation is never authoritative and never mutating.
  for (const auto* version : state.components.all_versions()) {
    if (state.is_retired(version->component, version->generation)) {
      REF_CHECK_MSG(!is_authoritative_lifecycle(version->lifecycle),
                    std::string(stage) + ": retired generation is authoritative");
      REF_CHECK_MSG(!permits_mutation_lifecycle(version->lifecycle),
                    std::string(stage) + ": retired generation may still mutate");
    }
  }
  // 2. Every recorded compatibility edge is internally consistent.
  for (const auto* edge : state.matrix.all_edges()) {
    REF_CHECK_MSG(edge->permissions == derive_permissions(*edge), std::string(stage) + ": edge drifted");
  }
  // 3. A plan that crossed the rollback barrier has no rollback-available record.
  for (const auto& [id, plan] : state.plans) {
    (void)id;
    if (!plan.rollback_barrier_crossed) continue;
    for (const auto& record : state.migrations) {
      if (record.plan != plan.id) continue;
      REF_CHECK_MSG(!record.rollback_available, std::string(stage) + ": barrier crossed but rollback claimed");
    }
  }
  // 4. No enabled feature gate lacks an enabling plan.
  for (const auto* gate : state.gates.all()) {
    if (!gate->enabled) continue;
    REF_CHECK_MSG(!gate->enabled_by.empty(), std::string(stage) + ": enabled gate without a plan");
  }
}

struct RandomRun {
  std::uint64_t seed{0};
  std::mt19937_64 engine{};
  std::string state_path{};

  explicit RandomRun(std::uint64_t value, const std::string& path) : seed(value), engine(value), state_path(path) {}

  [[nodiscard]] std::uint64_t pick(std::uint64_t low, std::uint64_t high) {
    std::uniform_int_distribution<std::uint64_t> distribution(low, high);
    return distribution(engine);
  }
  [[nodiscard]] bool chance(std::uint64_t percent) { return pick(1, 100) <= percent; }
};

}  // namespace

REF_TEST(property, randomized_fleet_evolution_holds_invariants) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    TempDir dir("property-" + std::to_string(seed));
    RandomRun run(seed, dir.file("evolution.state"));
    Fabric fabric(run.state_path);
    REF_NOTE("seed " + std::to_string(seed));

    const std::string components[] = {"orders", "billing", "search"};
    for (const auto& component : components) {
      // Generation 1 is authoritative; generation 2 is a candidate.
      REF_CHECK_STATUS_OK(
          fabric.register_component(reftest::make_component(component.c_str(), 1, "1", "1",
                                                            LifecycleState::Current, {}, {},
                                                            "refworker-gen1"))
              .status);
      REF_CHECK_STATUS_OK(
          fabric.register_component(reftest::make_component(component.c_str(), 2, "1,2", "1,2",
                                                            LifecycleState::Registered, {{1, 2}, {2, 1}},
                                                            {reftest::feature("shadow-query", 1, true, true)},
                                                            "refworker-gen2"))
              .status);
      if (run.chance(80)) {
        REF_CHECK_STATUS_OK(fabric.publish_edge(1, 2).status);
      }
    }
    check_invariants(fabric, "after registration");

    std::vector<std::unique_ptr<TestWorker>> workers;
    for (int step = 0; step < 14; ++step) {
      const std::string component = components[run.pick(0, 2)];
      const std::uint64_t action = run.pick(0, 9);
      if (action == 0) {
        // Start a worker of either generation.
        const std::uint64_t generation = run.pick(1, 2);
        auto worker = std::make_unique<TestWorker>(fabric, ("worker-" + std::to_string(step)).c_str(),
                                                   run.pick(1, 3), generation,
                                                   generation == 2 ? "1,2" : "1",
                                                   generation == 2 ? "1,2" : "1");
        if (worker->connect().is_ok()) workers.push_back(std::move(worker));
      } else if (action == 1) {
        // Publish compatibility again, which must bump the matrix generation.
        if (run.chance(60)) (void)fabric.publish_edge(1, 2);
      } else if (action == 2) {
        (void)fabric.create_plan(component.c_str(), 2, "canary-a", "migration-1-2", false);
      } else if (action == 3) {
        const RolloutStage stages[] = {RolloutStage::CompatibilityProven, RolloutStage::CanaryCohort,
                                       RolloutStage::MixedVersionCohort, RolloutStage::ExpandedCohort,
                                       RolloutStage::MigrationBarrier, RolloutStage::NewWriterEnabled,
                                       RolloutStage::OldWriterDrain, RolloutStage::FullPromotion};
        (void)fabric.advance(stages[run.pick(0, 7)]);
      } else if (action == 4) {
        (void)fabric->command_rebind_plan(fabric.op(), EvolutionPlanId::from_valid("plan-1"),
                                          EvolutionPlanGeneration::unset());
      } else if (action == 5) {
        (void)fabric->command_enable_feature(fabric.op(), FeatureGateId::from_valid("shadow-query"),
                                             EvolutionPlanId::from_valid("plan-1"));
      } else if (action == 6) {
        (void)fabric->command_request_drain(fabric.op(), RuntimeComponentId::from_valid(component.c_str()),
                                            RuntimeGeneration::from_raw(run.pick(1, 2)));
      } else if (action == 7) {
        (void)fabric->command_request_rollback(fabric.op(), EvolutionPlanId::from_valid("plan-1"));
      } else if (action == 8) {
        (void)fabric->command_request_retirement(fabric.op(),
                                                 RuntimeComponentId::from_valid(component.c_str()),
                                                 RuntimeGeneration::from_raw(run.pick(1, 2)));
      } else {
        // Workers report completions, including duplicates, which must never
        // double commit.
        if (!workers.empty()) {
          const EvolutionPlan plan = fabric.plan();
          (void)workers[run.pick(0, workers.size() - 1)]->complete(
              EvolutionPlanId::from_valid("plan-1"), RolloutStageId::from_valid("drain"),
              plan.stage_generation, MigrationOutcome::NotRequired, true);
        }
      }
      check_invariants(fabric, ("step " + std::to_string(step)).c_str());
    }

    // Persistence round trip: a restart must not change any invariant, and no
    // process authority may be restored.
    fabric.restart();
    check_invariants(fabric, "after restart");
    const DurableState reloaded = fabric->durable_state_copy();
    REF_CHECK(reloaded.components.version_count() >= 6u);
    REF_CHECK(fabric->live_workers().empty());
  }
}

REF_TEST(property, deterministic_state_yields_deterministic_explanation) {
  TempDir dir("property-determinism");
  const std::string path = dir.file("evolution.state");
  std::string first;
  std::string second;
  for (int round = 0; round < 2; ++round) {
    // A fresh coordinator over the same script must produce identical
    // explanation output for identical state.
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
    Fabric fabric(path);
    REF_CHECK_STATUS_OK(fabric.register_component(reftest::make_component(
                            "orders", 1, "1", "1", LifecycleState::Current, {}, {}, "g1"))
                            .status);
    REF_CHECK_STATUS_OK(fabric.register_component(reftest::make_component(
                            "orders", 2, "1,2", "1,2", LifecycleState::Registered, {{1, 2}, {2, 1}},
                            {reftest::feature("shadow-query", 1, true, true)}, "g2"))
                            .status);
    REF_CHECK_STATUS_OK(fabric.publish_edge(1, 2).status);
    REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
    REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
    const std::string json = fabric->authority(RuntimeComponentId::from_valid("orders")).to_json();
    if (round == 0) {
      first = json;
    } else {
      second = json;
    }
  }
  REF_CHECK_EQ(first, second);
  REF_CHECK(!first.empty());
}

REF_TEST(race, stage_advance_versus_compatibility_change) {
  TempDir dir("race-compat");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  // The compatibility change lands first: the advance must observe the new
  // generation and refuse, never silently accept stale evidence.
  REF_CHECK_STATUS_OK(fabric.publish_edge(1, 2).status);
  REF_CHECK_STATUS_CODE(fabric.advance(RolloutStage::CompatibilityProven).status, ErrorCode::StaleEvidence);
}

REF_TEST(race, protocol_negotiation_versus_runtime_fence) {
  TempDir dir("race-negotiate");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  TestWorker worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(worker.connect());
  REF_CHECK_STATUS_OK(fabric->command_fence(fabric.op(), WorkerId::from_valid("worker-a"),
                                            WorkerBootId::from_raw(1), "fenced during negotiation")
                          .status);
  // A fenced boot may not negotiate again.
  TestWorker again(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_CODE(again.connect(), ErrorCode::StaleBoot);
}

REF_TEST(race, migration_commit_versus_coordinator_restart) {
  TempDir dir("race-migration");
  const std::string path = dir.file("evolution.state");
  Fabric fabric(path);
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MixedVersionCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MigrationBarrier).status);

  // A migration record committed before the restart survives exactly, and the
  // barrier it crossed is preserved.
  MigrationRecord record;
  record.id = MigrationId::from_valid("migration-1-2");
  record.generation = MigrationGeneration::from_raw(1);
  record.schema = SchemaId::from_valid("component-state");
  record.source = SchemaGeneration::from_raw(1);
  record.target = SchemaGeneration::from_raw(2);
  record.runtime_generation = RuntimeGeneration::from_raw(2);
  record.epoch = fabric.plan().epoch;
  record.plan = EvolutionPlanId::from_valid("plan-1");
  record.irreversible = true;
  record.rollback_available = false;
  record.outcome = MigrationOutcome::Irreversible;
  {
    // Commit through the durable store so the restart has real state to load.
    DurableState state = fabric->durable_state_copy();
    state.migrations.push_back(record);
    auto it = state.plans.find(EvolutionPlanId::from_valid("plan-1"));
    REF_CHECK(it != state.plans.end());
    if (it != state.plans.end()) it->second.rollback_barrier_crossed = true;
    std::string bytes;
    REF_CHECK_STATUS_OK(DurableStore::encode(state, bytes));
    REF_CHECK_STATUS_OK(write_file_atomic(path, bytes));
  }
  fabric.restart();
  const DurableState reloaded = fabric->durable_state_copy();
  REF_CHECK_EQ(reloaded.migrations.size(), 1u);
  const auto plan = reloaded.plans.find(EvolutionPlanId::from_valid("plan-1"));
  REF_CHECK(plan != reloaded.plans.end());
  REF_CHECK(plan != reloaded.plans.end() && plan->second.rollback_barrier_crossed);
  // Rollback is now refused: the barrier is durable, not in-memory.
  REF_CHECK_STATUS_CODE(
      fabric->command_request_rollback(fabric.op(), EvolutionPlanId::from_valid("plan-1")).status,
      ErrorCode::Incompatible);
}

REF_TEST(race, rollback_versus_late_upgrade_completion) {
  TempDir dir("race-rollback");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  TestWorker old_worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(old_worker.connect());
  const StageGeneration stage_before = fabric.plan().stage_generation;

  REF_CHECK_STATUS_OK(fabric->command_request_rollback(fabric.op(), EvolutionPlanId::from_valid("plan-1"))
                          .status);
  // A completion from the superseded stage is rejected after the rollback.
  REF_CHECK(old_worker
                .complete(EvolutionPlanId::from_valid("plan-1"),
                          RolloutStageId::from_valid("stage-" + std::to_string(stage_before.raw())),
                          stage_before, MigrationOutcome::Committed)
                .is_failure());
  const DurableState state = fabric->durable_state_copy();
  REF_CHECK(state.migrations.empty());
}

REF_TEST(race, retirement_versus_reconnect) {
  TempDir dir("race-retire");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
  TestWorker candidate(fabric, "worker-b", 1, 2, "1,2", "1,2");
  REF_CHECK_STATUS_OK(candidate.connect());
  TestWorker old_worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(old_worker.connect());
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MixedVersionCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::ExpandedCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MigrationBarrier).status);
  // The old generation is drained and fenced before new writers start.
  REF_CHECK_STATUS_OK(fabric->command_request_drain(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                                    RuntimeGeneration::from_raw(1))
                          .status);
  const StageGeneration drain_stage = fabric.plan().stage_generation;
  REF_CHECK_STATUS_OK(old_worker.complete(EvolutionPlanId::from_valid("plan-1"),
                                          RolloutStageId::from_valid("drain"), drain_stage,
                                          MigrationOutcome::NotRequired, true));
  REF_CHECK_STATUS_OK(fabric->command_fence(fabric.op(), WorkerId::from_valid("worker-a"),
                                            WorkerBootId::from_raw(1), "drained")
                          .status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::NewWriterEnabled).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::OldWriterDrain).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::FullPromotion).status);
  REF_CHECK_STATUS_OK(fabric->command_request_retirement(fabric.op(),
                                                         RuntimeComponentId::from_valid("orders"),
                                                         RuntimeGeneration::from_raw(1))
                          .status);
  // Both the old boot and a brand new boot of the retired generation are refused.
  TestWorker stale(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_CODE(stale.connect(), ErrorCode::RetiredGeneration);
  TestWorker fresh(fabric, "worker-a", 9, 1, "1", "1");
  REF_CHECK_STATUS_CODE(fresh.connect(), ErrorCode::RetiredGeneration);
}

REF_TEST(race, feature_gate_versus_stale_old_writer) {
  TempDir dir("race-gate");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MixedVersionCohort).status);

  TestWorker old_worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(old_worker.connect());
  // While the old writer is live the plan requires protocol 1, so a protocol-2
  // feature cannot be enabled.
  REF_CHECK_STATUS_CODE(
      fabric->command_enable_feature(fabric.op(), FeatureGateId::from_valid("state-token-transform"),
                                     EvolutionPlanId::from_valid("plan-1"))
          .status,
      ErrorCode::StaleGeneration);
  const DurableState state = fabric->durable_state_copy();
  const FeatureGate* gate = state.gates.find(FeatureGateId::from_valid("state-token-transform"));
  REF_CHECK(gate != nullptr && !gate->enabled);
}

REF_TEST(race, snapshot_versus_stage_change) {
  TempDir dir("race-snapshot");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  const AuthorityView before = fabric->authority(RuntimeComponentId::from_valid("orders"));
  REF_CHECK_EQ(before.stage, RolloutStage::CandidateRegistered);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  const AuthorityView after = fabric->authority(RuntimeComponentId::from_valid("orders"));
  REF_CHECK_EQ(after.stage, RolloutStage::CompatibilityProven);
  // Two reads taken across a stage change never blend: each is internally
  // consistent with the epoch it reports.
  REF_CHECK(before.epoch == after.epoch);
  REF_CHECK(!before.explanation.empty() && !after.explanation.empty());
}

REF_TEST(race, supersession_versus_old_stage_commit) {
  TempDir dir("race-supersede");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  const StageGeneration stage_before = fabric.plan().stage_generation;
  REF_CHECK_STATUS_OK(fabric->command_supersede_plan(fabric.op(), EvolutionPlanId::from_valid("plan-1"))
                          .status);
  // A completion that was in flight for the superseded stage generation is
  // rejected, and nothing is committed.
  const CommandResult late = fabric->command_publish_completion(
      fabric.op(), EvolutionPlanId::from_valid("plan-1"),
      RolloutStageId::from_valid("stage-" + std::to_string(stage_before.raw())), stage_before,
      MigrationOutcome::Committed, IntegrityDigest{});
  REF_CHECK(late.status.is_failure());
  const DurableState state = fabric->durable_state_copy();
  REF_CHECK(state.migrations.empty());
  REF_CHECK(fabric.plan().superseded);
}

REF_TEST(race, persistence_failure_never_advances_authority_silently) {
  TempDir dir("race-persistence");
  Fabric fabric(dir.file("evolution.state"));
  reftest::bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  fabric->set_fail_persistence(true);
  const CommandResult failed = fabric.advance(RolloutStage::CompatibilityProven);
  REF_CHECK(failed.status.is_failure());
  fabric->set_fail_persistence(false);
  // The in-memory stage advanced but the durable image did not, so a restart
  // must not resurrect the failed transition as committed history.
  const DurableState reloaded_after_restart = [&] {
    fabric.restart();
    return fabric->durable_state_copy();
  }();
  const auto plan = reloaded_after_restart.plans.find(EvolutionPlanId::from_valid("plan-1"));
  REF_CHECK(plan != reloaded_after_restart.plans.end());
  REF_CHECK(plan != reloaded_after_restart.plans.end() &&
            plan->second.stage == RolloutStage::CandidateRegistered);
}
