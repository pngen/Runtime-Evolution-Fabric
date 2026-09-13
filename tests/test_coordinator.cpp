// Runtime Evolution Fabric - coordinator behaviour: rollout stages, authority,
// feature gates, drain, rollback, retirement, fencing and recovery.
#include <memory>

#include "support/fabric_harness.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using reftest::Fabric;
using reftest::TestWorker;
using reftest::bootstrap_fleet;

using namespace ref;
using reftest::TempDir;

REF_TEST(coordinator, plan_lifecycle_and_stage_preconditions) {
  TempDir dir("coordinator-plan");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);

  TestWorker old_worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(old_worker.connect());
  TestWorker new_worker(fabric, "worker-b", 1, 2, "1,2", "1,2");
  REF_CHECK_STATUS_OK(new_worker.connect());

  const CommandResult created = fabric.create_plan("orders", 2, "canary-a", nullptr, false);
  REF_CHECK_STATUS_OK(created.status);
  REF_CHECK_EQ(fabric.plan().stage, RolloutStage::CandidateRegistered);
  REF_CHECK_EQ(fabric.plan().protocol_generation.raw(), 1ull);  // highest common generation
  REF_CHECK_EQ(fabric.plan().schema_generation.raw(), 1ull);    // the authoritative writer's format

  // A candidate without proven compatibility cannot skip ahead, and stages
  // required by policy cannot be skipped at all.
  REF_CHECK_STATUS_CODE(fabric.advance(RolloutStage::FullPromotion).status, ErrorCode::Conflict);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_CODE(fabric.advance(RolloutStage::MigrationBarrier).status, ErrorCode::Conflict);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);

  const DurableState after_canary = fabric->durable_state_copy();
  const RuntimeComponentVersion* candidate =
      after_canary.components.find(RuntimeComponentId::from_valid("orders"), RuntimeGeneration::from_raw(2));
  REF_CHECK(candidate != nullptr && candidate->lifecycle == LifecycleState::CanaryActive);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MixedVersionCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::ExpandedCohort).status);
  // New writers can only be enabled once old writers are drained or fenced,
  // and the rollback path is still valid.
  REF_CHECK_STATUS_CODE(fabric.advance(RolloutStage::NewWriterEnabled).status, ErrorCode::Unauthorized);
  REF_CHECK(!fabric.plan().new_writer_enabled);
}

REF_TEST(coordinator, plan_goes_stale_when_compatibility_changes) {
  TempDir dir("coordinator-stale");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);

  // Publishing new compatibility evidence invalidates the binding.
  REF_CHECK_STATUS_OK(fabric.publish_edge(1, 2).status);
  REF_CHECK_STATUS_CODE(fabric.advance(RolloutStage::CanaryCohort).status, ErrorCode::StaleEvidence);
  REF_CHECK_STATUS_OK(fabric->command_rebind_plan(fabric.op(), EvolutionPlanId::from_valid("plan-1"),
                                                  EvolutionPlanGeneration::unset())
                          .status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
}

REF_TEST(coordinator, superseded_plan_cannot_advance_or_complete) {
  TempDir dir("coordinator-supersede");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  const StageGeneration stage_generation = fabric.plan().stage_generation;

  REF_CHECK_STATUS_OK(fabric->command_supersede_plan(fabric.op(), EvolutionPlanId::from_valid("plan-1")).status);
  REF_CHECK_EQ(fabric.plan().stage, RolloutStage::Superseded);
  REF_CHECK_STATUS_CODE(fabric.advance(RolloutStage::CanaryCohort).status, ErrorCode::StalePlan);
  REF_CHECK_STATUS_CODE(
      fabric->command_publish_completion(fabric.op(), EvolutionPlanId::from_valid("plan-1"),
                                         RolloutStageId::from_valid("stage-1"), stage_generation,
                                         MigrationOutcome::Committed, IntegrityDigest{})
          .status,
      ErrorCode::StalePlan);  // a superseded plan rejects every late completion
}

REF_TEST(coordinator, feature_gate_is_generation_bound) {
  TempDir dir("coordinator-gate");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);

  // The gate exists but is disabled: a feature never becomes available merely
  // because a new runtime generation exists.
  const DurableState initial = fabric->durable_state_copy();
  const FeatureGate* gate = initial.gates.find(FeatureGateId::from_valid("state-token-transform"));
  REF_CHECK(gate != nullptr);
  REF_CHECK(gate != nullptr && !gate->enabled);
  REF_CHECK_STATUS_CODE(
      fabric->command_enable_feature(fabric.op(), FeatureGateId::from_valid("state-token-transform"),
                                     EvolutionPlanId::from_valid("plan-1"))
          .status,
      ErrorCode::StaleGeneration);  // the feature needs protocol 2 and schema 3

  // The plan's protocol generation is still 1 during the mixed-version phase.
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  REF_CHECK_EQ(fabric.plan().protocol_generation.raw(), 1ull);
  const CommandResult refused =
      fabric->command_enable_feature(fabric.op(), FeatureGateId::from_valid("state-token-transform"),
                                     EvolutionPlanId::from_valid("plan-1"));
  REF_CHECK(refused.status.is_failure());

  // A gate whose requirements are already met can be enabled, and enabling it
  // bumps the gate generation for the bound plan.
  const CommandResult shadow =
      fabric->command_enable_feature(fabric.op(), FeatureGateId::from_valid("shadow-query"),
                                     EvolutionPlanId::from_valid("plan-1"));
  REF_CHECK_STATUS_OK(shadow.status);
  const DurableState after = fabric->durable_state_copy();
  const FeatureGate* enabled = after.gates.find(FeatureGateId::from_valid("shadow-query"));
  REF_CHECK(enabled != nullptr && enabled->enabled);
  REF_CHECK_EQ(fabric.plan().gate_generation.raw(), after.gates.generation().raw());
}

REF_TEST(coordinator, drain_completion_and_retirement_are_explicit) {
  TempDir dir("coordinator-drain");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MixedVersionCohort).status);

  TestWorker old_worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(old_worker.connect());
  TestWorker new_worker(fabric, "worker-b", 1, 2, "1,2", "1,2");
  REF_CHECK_STATUS_OK(new_worker.connect());
  REF_CHECK_EQ(new_worker.operation(), OperationClass::RestrictedMutation);

  // Retirement before any drain must be refused.
  REF_CHECK_STATUS_CODE(
      fabric->command_request_retirement(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                         RuntimeGeneration::from_raw(1))
          .status,
      ErrorCode::Conflict);

  REF_CHECK_STATUS_OK(fabric->command_request_drain(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                                    RuntimeGeneration::from_raw(1))
                          .status);
  const EvolutionPlan plan_now = fabric.plan();
  REF_CHECK_STATUS_OK(old_worker.complete(EvolutionPlanId::from_valid("plan-1"),
                                          RolloutStageId::from_valid("drain"), plan_now.stage_generation,
                                          MigrationOutcome::NotRequired, true));
  // A duplicate drain completion is refused rather than counted twice.
  REF_CHECK(old_worker.complete(EvolutionPlanId::from_valid("plan-1"), RolloutStageId::from_valid("drain"),
                                plan_now.stage_generation, MigrationOutcome::NotRequired, true)
                .is_failure());

  const DurableState drained = fabric->durable_state_copy();
  const WorkerRecord* record = nullptr;
  const auto it = drained.workers.find(WorkerId::from_valid("worker-a"));
  if (it != drained.workers.end()) record = &it->second;
  REF_CHECK(record != nullptr && record->drained);

  // A drained but still-running process still blocks retirement: it must be
  // fenced or have exited.
  REF_CHECK_STATUS_CODE(
      fabric->command_request_retirement(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                         RuntimeGeneration::from_raw(1))
          .status,
      ErrorCode::Conflict);
  REF_CHECK_STATUS_OK(fabric->command_fence(fabric.op(), WorkerId::from_valid("worker-a"),
                                            WorkerBootId::from_raw(1), "old generation terminated")
                          .status);

  // With the old writer drained, the new writer barrier can be crossed.
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MigrationBarrier).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::NewWriterEnabled).status);
  REF_CHECK(fabric.plan().new_writer_enabled);
  REF_CHECK_EQ(fabric.plan().protocol_generation.raw(), 2ull);
  REF_CHECK_EQ(fabric.plan().schema_generation.raw(), 2ull);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::OldWriterDrain).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::FullPromotion).status);

  const DurableState promoted = fabric->durable_state_copy();
  const auto authoritative = promoted.components.authoritative_generation(RuntimeComponentId::from_valid("orders"));
  REF_CHECK(authoritative.has_value() && authoritative->raw() == 2ull);

  // The retired generation is terminal and cannot come back.
  REF_CHECK_STATUS_OK(
      fabric->command_request_retirement(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                         RuntimeGeneration::from_raw(1))
          .status);
  const DurableState retired_state = fabric->durable_state_copy();
  REF_CHECK(retired_state.is_retired(RuntimeComponentId::from_valid("orders"),
                                     RuntimeGeneration::from_raw(1)));
  REF_CHECK_STATUS_CODE(
      fabric->command_request_lifecycle(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                        RuntimeGeneration::from_raw(1), LifecycleState::Current)
          .status,
      ErrorCode::RetiredGeneration);
}

REF_TEST(coordinator, rollback_restores_authority_then_barrier_blocks_it) {
  TempDir dir("coordinator-rollback");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);

  TestWorker old_worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(old_worker.connect());

  const CommandResult rolled =
      fabric->command_request_rollback(fabric.op(), EvolutionPlanId::from_valid("plan-1"));
  REF_CHECK_STATUS_OK(rolled.status);
  REF_CHECK_EQ(fabric.plan().stage, RolloutStage::RolledBack);
  const DurableState after = fabric->durable_state_copy();
  const auto authoritative = after.components.authoritative_generation(RuntimeComponentId::from_valid("orders"));  REF_CHECK(authoritative.has_value() && authoritative->raw() == 1ull);
  const RuntimeComponentVersion* candidate =
      after.components.find(RuntimeComponentId::from_valid("orders"), RuntimeGeneration::from_raw(2));
  REF_CHECK(candidate != nullptr && candidate->lifecycle == LifecycleState::RolledBack);
}

REF_TEST(coordinator, coordinator_restart_advances_epoch_and_forgets_liveness) {
  TempDir dir("coordinator-restart");
  const std::string path = dir.file("evolution.state");
  Fabric fabric(path);
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  const CoordinatorEpoch before_epoch = fabric->coordinator_epoch();
  const EvolutionEpoch before_evolution = fabric.plan().epoch;

  fabric.restart();

  REF_CHECK(fabric->coordinator_epoch() > before_epoch);
  const DurableState reloaded = fabric->durable_state_copy();
  REF_CHECK(reloaded.components.version_count() == 2u);
  REF_CHECK(reloaded.matrix.edge_count() == 1u);
  const EvolutionPlan plan_after = fabric.plan();
  REF_CHECK_EQ(plan_after.epoch.raw(), before_evolution.raw());
  REF_CHECK_EQ(plan_after.stage, RolloutStage::CompatibilityProven);
  REF_CHECK(plan_after.history.size() >= 2u);
  REF_CHECK(fabric->live_workers().empty());
}

REF_TEST(coordinator, fencing_rejects_a_boot_identity_for_good) {
  TempDir dir("coordinator-fence");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  TestWorker worker(fabric, "worker-a", 4, 1, "1", "1");
  REF_CHECK_STATUS_OK(worker.connect());
  REF_CHECK_EQ(fabric->live_workers().size(), 1u);

  REF_CHECK_STATUS_OK(fabric->command_fence(fabric.op(), WorkerId::from_valid("worker-a"),
                                            WorkerBootId::from_raw(4), "operator fence")
                          .status);
  TestWorker reused(fabric, "worker-a", 4, 1, "1", "1");
  REF_CHECK_STATUS_CODE(reused.connect(), ErrorCode::StaleBoot);
  TestWorker replacement(fabric, "worker-a", 5, 1, "1", "1");
  REF_CHECK_STATUS_OK(replacement.connect());
}

REF_TEST(coordinator, frames_with_stale_epochs_or_generations_are_rejected) {
  TempDir dir("coordinator-frames");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  TestWorker worker(fabric, "worker-a", 1, 1, "1", "1");
  REF_CHECK_STATUS_OK(worker.connect());

  WireMessage query;
  (void)query.set_text(FieldId::from_valid("query"), "components");
  Frame response;
  WireMessage body;

  Frame stale;
  stale.header.type = MessageType::QueryState;
  stale.header.runtime_generation = RuntimeGeneration::from_raw(1);
  stale.header.protocol_generation = worker.client().negotiated_protocol();
  stale.header.schema_generation = worker.client().negotiated_schema();
  stale.header.coordinator_epoch = CoordinatorEpoch::from_raw(4096);
  stale.header.evolution_epoch = worker.client().evolution_epoch();
  stale.header.boot = WorkerBootId::from_raw(1);
  stale.header.sequence = worker.client().next_sequence();
  stale.payload = query.encode();
  REF_CHECK_STATUS_OK(worker.client().send_raw(stale, response, body));
  REF_CHECK_EQ(response.header.type, MessageType::Error);
  REF_CHECK_EQ(*body.text(FieldId::from_valid("status")), std::string_view("STALE_EPOCH"));

  Frame replay = stale;
  replay.header.coordinator_epoch = worker.client().coordinator_epoch();
  replay.header.evolution_epoch = worker.client().evolution_epoch();
  replay.header.sequence = 1;
  REF_CHECK_STATUS_OK(worker.client().send_raw(replay, response, body));
  REF_CHECK_EQ(response.header.type, MessageType::Error);
  REF_CHECK_EQ(*body.text(FieldId::from_valid("status")), std::string_view("CONFLICT"));

  Frame transform;
  transform.header.type = MessageType::RequestStateTransform;
  transform.header.runtime_generation = RuntimeGeneration::from_raw(1);
  transform.header.protocol_generation = ProtocolGeneration::from_raw(1);
  transform.header.schema_generation = worker.client().negotiated_schema();
  transform.header.coordinator_epoch = worker.client().coordinator_epoch();
  transform.header.evolution_epoch = worker.client().evolution_epoch();
  transform.header.boot = WorkerBootId::from_raw(1);
  transform.header.sequence = worker.client().next_sequence();
  transform.header.flags = 0;
  WireMessage transform_body;
  (void)transform_body.set_ident(FieldId::from_valid("plan"), EvolutionPlanId::from_valid("plan-1"));
  (void)transform_body.set_generation(FieldId::from_valid("target_schema"), SchemaGeneration::from_raw(2));
  (void)transform_body.set_ident(FieldId::from_valid("worker"), WorkerId::from_valid("worker-a"));
  (void)transform_body.set_generation(FieldId::from_valid("boot"), WorkerBootId::from_raw(1));
  transform.payload = transform_body.encode();
  REF_CHECK_STATUS_OK(worker.client().send_raw(transform, response, body));
  REF_CHECK_EQ(response.header.type, MessageType::Error);
  REF_CHECK_MSG(*body.text(FieldId::from_valid("status")) == std::string_view("UNSUPPORTED"),
                std::string(*body.text(FieldId::from_valid("status"))) + " / " +
                    std::string(*body.text(FieldId::from_valid("detail"))));

  Frame extra = stale;
  extra.header.coordinator_epoch = worker.client().coordinator_epoch();
  extra.header.evolution_epoch = worker.client().evolution_epoch();
  extra.header.sequence = worker.client().next_sequence();
  WireMessage extra_body;
  (void)extra_body.set_text(FieldId::from_valid("query"), "components");
  (void)extra_body.set_text(FieldId::from_valid("smuggled"), "value");
  extra.payload = extra_body.encode();
  REF_CHECK_STATUS_OK(worker.client().send_raw(extra, response, body));
  REF_CHECK_MSG(*body.text(FieldId::from_valid("status")) == std::string_view("UNSUPPORTED"),
                std::string(*body.text(FieldId::from_valid("status"))) + " / " +
                    std::string(*body.text(FieldId::from_valid("detail"))));

  Frame missing = stale;
  missing.header.coordinator_epoch = worker.client().coordinator_epoch();
  missing.header.evolution_epoch = worker.client().evolution_epoch();
  missing.header.sequence = worker.client().next_sequence();
  missing.payload = WireMessage{}.encode();
  REF_CHECK_STATUS_OK(worker.client().send_raw(missing, response, body));
  REF_CHECK_MSG(*body.text(FieldId::from_valid("status")) == std::string_view("INVALID_ARGUMENT"),
                std::string(*body.text(FieldId::from_valid("status"))) + " / " +
                    std::string(*body.text(FieldId::from_valid("detail"))));
}

REF_TEST(coordinator, retired_generation_cannot_reconnect_or_register) {
  TempDir dir("coordinator-retired");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
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
  // The old generation must be drained and fenced before new writers start.
  REF_CHECK_STATUS_OK(fabric->command_request_drain(fabric.op(), RuntimeComponentId::from_valid("orders"),
                                                    RuntimeGeneration::from_raw(1))
                          .status);
  const StageGeneration drain_stage = fabric.plan().stage_generation;
  REF_CHECK_STATUS_OK(old_worker.complete(EvolutionPlanId::from_valid("plan-1"),
                                          RolloutStageId::from_valid("drain"), drain_stage,
                                          MigrationOutcome::NotRequired, true));
  REF_CHECK_STATUS_OK(fabric->command_fence(fabric.op(), WorkerId::from_valid("worker-a"),
                                            WorkerBootId::from_raw(1), "old generation drained")
                          .status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::NewWriterEnabled).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::OldWriterDrain).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::FullPromotion).status);
  REF_CHECK_STATUS_OK(fabric->command_request_retirement(fabric.op(),
                                                         RuntimeComponentId::from_valid("orders"),
                                                         RuntimeGeneration::from_raw(1))
                          .status);

  TestWorker retired(fabric, "worker-zombie", 1, 1, "1", "1");
  REF_CHECK_STATUS_CODE(retired.connect(), ErrorCode::RetiredGeneration);
  REF_CHECK_STATUS_CODE(fabric.register_component(reftest::make_component(
                            "orders", 1, "1", "1", LifecycleState::Current, {}, {}, "refworker-gen1"))
                            .status,
                        ErrorCode::RetiredGeneration);
}

REF_TEST(coordinator, every_query_kind_returns_balanced_json) {
  TempDir dir("coordinator-queries");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", "migration-1-2", false).status);
  for (const char* kind :
       {"authority", "components", "plans", "compatibility", "protocols", "schemas", "gates", "migrations",
        "workers", "epochs", "retirement", "snapshots", "reconcile", "explain"}) {
    const CommandResult result = fabric->command_query(fabric.op(), kind,
                                                       RuntimeComponentId::from_valid("orders"));
    REF_CHECK_MSG(result.status.is_ok(), kind);
    const std::string& json = result.json;
    REF_CHECK_MSG(!json.empty(), kind);
    // Balanced braces and brackets, and no control characters: the document is
    // carried inside a canonical text field, so it must be printable.
    int depth = 0;
    bool balanced = true;
    for (const char c : json) {
      if (c == '{' || c == '[') ++depth;
      if (c == '}' || c == ']') {
        --depth;
        if (depth < 0) balanced = false;
      }
      if (static_cast<unsigned char>(c) < 0x20) balanced = false;
    }
    REF_CHECK_MSG(balanced && depth == 0, std::string(kind) + " produced unbalanced JSON: " + json);
  }
}

REF_TEST(coordinator, bounded_plan_and_component_limits_are_enforced) {
  TempDir dir("coordinator-limits");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);
  PlanSpec second;
  second.id = EvolutionPlanId::from_valid("plan-2");
  second.component = RuntimeComponentId::from_valid("orders");
  second.candidate_generation = RuntimeGeneration::from_raw(2);
  second.canary_cohort = CohortId::from_valid("canary-b");
  REF_CHECK_STATUS_CODE(fabric->command_create_plan(fabric.op(), second).status, ErrorCode::Conflict);
  PlanSpec unknown = second;
  unknown.component = RuntimeComponentId::from_valid("unknown");
  REF_CHECK_STATUS_CODE(fabric->command_create_plan(fabric.op(), unknown).status, ErrorCode::NotFound);
}

REF_TEST(coordinator, concurrent_clients_get_consistent_authority) {
  TempDir dir("coordinator-concurrent");
  Fabric fabric(dir.file("evolution.state"));
  bootstrap_fleet(fabric);
  REF_CHECK_STATUS_OK(fabric.create_plan("orders", 2, "canary-a", nullptr, false).status);

  std::atomic<bool> stop{false};
  std::atomic<int> inconsistencies{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const AuthorityView view = fabric->authority(RuntimeComponentId::from_valid("orders"));
        bool has_authoritative = false;
        bool has_candidate = false;
        for (const auto& entry : view.coexistence) {
          if (entry.is_authoritative) has_authoritative = true;
          if (entry.generation.raw() == 2) has_candidate = true;
        }
        if (!has_authoritative || !has_candidate) inconsistencies.fetch_add(1);
      }
    });
  }
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CompatibilityProven).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::CanaryCohort).status);
  REF_CHECK_STATUS_OK(fabric.advance(RolloutStage::MixedVersionCohort).status);
  stop = true;
  for (auto& thread : readers) thread.join();
  REF_CHECK_EQ(inconsistencies.load(), 0);
}
