// Runtime Evolution Fabric - read-only inspection and reconciliation.
//
// Inspection never mutates the evolution structure. Reconciliation compares
// durable expectations against live reality and reports structured findings; it
// never rewrites history on its own.
#include <algorithm>

#include "coordinator_internal.hpp"
#include "ref/coordinator.hpp"

namespace ref {

AuthoritySnapshot EvolutionCoordinator::snapshot_locked(const RuntimeComponentId& component) {
  AuthoritySnapshot snapshot;
  snapshot.generation = durable_.snapshot_generation.is_set() ? durable_.snapshot_generation.next()
                                                              : SnapshotGeneration::first();
  snapshot.epoch = epoch_for_locked(component);
  snapshot.coordinator_epoch = durable_.coordinator_epoch;
  snapshot.matrix_generation = durable_.matrix.generation();
  snapshot.gate_generation = durable_.gates.generation();
  snapshot.evidence = durable_.evidence_generation;
  snapshot.canonical = query_json_locked("authority", component);
  durable_.snapshot_generation = snapshot.generation;
  if (snapshots_.size() >= limits::kSnapshots) snapshots_.erase(snapshots_.begin());
  snapshots_.push_back(snapshot);
  return snapshot;
}

ReconcileReport EvolutionCoordinator::reconcile_locked() const {
  ReconcileReport report;
  // Findings are appended in a deterministic order: plans, pending actions,
  // live workers, retirement records, feature gates.
  auto open = [&report](FindingKind kind, const RuntimeComponentId& component, const WorkerId& worker,
                        WorkerBootId boot, bool conservative,
                        std::string_view detail) -> ReconcileFinding* {
    if (report.findings.size() >= limits::kReconcileFindings) return nullptr;
    ReconcileFinding finding;
    finding.kind = kind;
    finding.component = component;
    finding.worker = worker;
    finding.boot = boot;
    finding.conservative_action_required = conservative;
    finding.detail = NoteText::from_valid(detail);
    report.findings.push_back(finding);
    if (conservative) report.requires_operator = true;
    return &report.findings.back();
  };

  for (const auto& [id, plan] : durable_.plans) {
    (void)id;
    if (!plan.is_active()) continue;
    const std::uint64_t candidate_live = live_worker_count_locked(plan.component, plan.candidate_generation);
    const std::uint64_t old_live = live_worker_count_locked(plan.component, plan.current_generation);
    if (rollout_stage_rank(plan.stage) >= rollout_stage_rank(RolloutStage::CanaryCohort) && candidate_live == 0) {
      if (ReconcileFinding* finding =
              open(FindingKind::CandidateExpectedAliveButAbsent, plan.component, WorkerId{}, WorkerBootId{}, true,
                   "candidate generation is expected alive during this stage but no live worker is present")) {
        finding->expected_generation = plan.candidate_generation;
      }
    }
    const RuntimeComponentVersion* current = durable_.components.find(plan.component, plan.current_generation);
    if (current != nullptr && current->lifecycle == LifecycleState::Draining && old_live > 0) {
      if (ReconcileFinding* finding =
              open(FindingKind::OldRuntimeStillAliveAfterDrain, plan.component, WorkerId{}, WorkerBootId{}, true,
                   "old generation is draining but still has live workers")) {
        finding->expected_generation = plan.current_generation;
        finding->observed_generation = plan.current_generation;
      }
    }
    if (plan.new_writer_enabled &&
        rollout_stage_rank(plan.stage) < rollout_stage_rank(RolloutStage::NewWriterEnabled)) {
      if (ReconcileFinding* finding =
              open(FindingKind::FeatureGateDiffersFromExpectation, plan.component, WorkerId{}, WorkerBootId{},
                   true, "plan reports new-writer enablement before the new-writer stage")) {
        finding->expected_generation = plan.candidate_generation;
      }
    }
  }

  for (const auto& action : pending_actions_) {
    if (action.state != ActionState::Dispatched && action.state != ActionState::OutcomeUnknown) continue;
    const bool worker_gone = worker_sessions_.find(action.worker) == worker_sessions_.end();
    if (!worker_gone && action.state == ActionState::Dispatched) continue;
    if (ReconcileFinding* finding =
            open(FindingKind::MigrationMayHaveCommitted, RuntimeComponentId{}, action.worker, action.boot, true,
                 "migration was dispatched and no completion was observed; the outcome is unknown and must be "
                 "inspected rather than repeated")) {
      finding->expected_schema = action.source;
      finding->observed_schema = action.source;
    }
  }

  for (const auto& [id, worker] : live_workers_) {
    (void)id;
    if (durable_.is_retired(worker.component, worker.generation)) {
      if (ReconcileFinding* finding =
              open(FindingKind::RetiredRuntimeReconnected, worker.component, worker.worker, worker.boot, true,
                   "a retired runtime generation has a live worker")) {
        finding->expected_generation = worker.generation;
        finding->observed_generation = worker.generation;
      }
      continue;
    }
    const EvolutionPlan* plan = active_plan_locked(worker.component);
    const RuntimeComponentVersion* version = durable_.components.find(worker.component, worker.generation);
    if (plan == nullptr) {
      if (version == nullptr || !is_authoritative_lifecycle(version->lifecycle)) {
        if (ReconcileFinding* finding = open(FindingKind::CandidateStartedOutsideCoordinator, worker.component,
                                             worker.worker, worker.boot, true,
                                             "a non-authoritative runtime generation is running without an "
                                             "evolution plan")) {
          finding->observed_generation = worker.generation;
        }
      }
      continue;
    }
    if (worker.generation == plan->candidate_generation) {
      if (worker.negotiated_protocol != plan->protocol_generation) {
        if (ReconcileFinding* finding = open(FindingKind::ProtocolGenerationDiffers, worker.component,
                                             worker.worker, worker.boot, false,
                                             "candidate worker negotiated a protocol generation different from "
                                             "the plan")) {
          finding->expected_protocol = plan->protocol_generation;
          finding->observed_protocol = worker.negotiated_protocol;
        }
      }
      if (worker.committed_schema > plan->schema_generation) {
        if (ReconcileFinding* finding =
                open(FindingKind::SchemaGenerationAdvancedExternally, worker.component, worker.worker, worker.boot,
                     true, "candidate worker reports a schema generation ahead of the plan")) {
          finding->expected_schema = plan->schema_generation;
          finding->observed_schema = worker.committed_schema;
        }
      }
      if (rollout_stage_rank(plan->stage) < rollout_stage_rank(RolloutStage::CanaryCohort)) {
        if (ReconcileFinding* finding = open(FindingKind::CandidateStartedOutsideCoordinator, worker.component,
                                             worker.worker, worker.boot, false,
                                             "candidate worker connected before the plan reached the canary stage")) {
          finding->expected_generation = plan->candidate_generation;
          finding->observed_generation = worker.generation;
        }
      }
    }
    if (worker.evidence.is_stale_relative_to(durable_.evidence_generation) &&
        (durable_.evidence_generation.raw() - worker.evidence.raw()) > config_.evidence_max_age) {
      if (ReconcileFinding* finding = open(FindingKind::StaleEvidence, worker.component, worker.worker, worker.boot,
                                           false, "worker evidence is older than the accepted evidence window")) {
        finding->observed_generation = worker.generation;
      }
    }
  }

  for (const auto& [key, record] : durable_.retirements) {
    (void)key;
    const RuntimeComponentVersion* version = durable_.components.find(record.component, record.generation);
    if (version != nullptr && !is_terminal_lifecycle(version->lifecycle)) {
      if (ReconcileFinding* finding =
              open(FindingKind::RetiredRuntimeReconnected, record.component, WorkerId{}, WorkerBootId{}, true,
                   "retirement record exists but the generation is not RETIRED")) {
        finding->expected_generation = record.generation;
      }
    }
  }

  for (const auto* gate : durable_.gates.all()) {
    if (!gate->enabled) continue;
    bool plan_found = false;
    for (const auto& [id, plan] : durable_.plans) {
      (void)id;
      if (plan.id != gate->enabled_by) continue;
      plan_found = true;
      if (rollout_stage_rank(plan.stage) < rollout_stage_rank(RolloutStage::NewWriterEnabled) &&
          gate->min_protocol_generation > plan.protocol_generation) {
        if (ReconcileFinding* finding = open(FindingKind::FeatureGateDiffersFromExpectation, plan.component,
                                             WorkerId{}, WorkerBootId{}, true,
                                             "feature gate is enabled while the bound plan has not reached the "
                                             "new-writer stage")) {
          finding->expected_protocol = plan.protocol_generation;
          finding->observed_protocol = gate->min_protocol_generation;
        }
      }
    }
    if (!plan_found) {
      (void)open(FindingKind::FeatureGateDiffersFromExpectation, RuntimeComponentId{}, WorkerId{}, WorkerBootId{},
                 false, "an enabled feature gate does not reference a known plan");
    }
  }
  return report;
}

CommandResult EvolutionCoordinator::command_reconcile(const PeerIdentity& peer) {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  (void)peer;
  const ReconcileReport report = reconcile_locked();
  JsonWriter writer;
  writer.begin_object();
  writer.field("status", "OK");
  writer.field("requires_operator", report.requires_operator);
  writer.field("finding_count", static_cast<std::uint64_t>(report.findings.size()));
  writer.key("findings");
  writer.begin_array();
  for (const auto& finding : report.findings) {
    writer.begin_object();
    writer.field("kind", ref::to_string(finding.kind));
    writer.field("component", finding.component.view());
    writer.field_generation("expected_generation", finding.expected_generation);
    writer.field_generation("observed_generation", finding.observed_generation);
    writer.field("worker", finding.worker.view());
    writer.field_generation("boot", finding.boot);
    writer.field("conservative_action_required", finding.conservative_action_required);
    writer.field("detail", finding.detail.view());
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  CommandResult result;
  result.status = Status::ok();
  result.json = writer.str();
  result.explanation.push_back("reconciliation completed without rewriting durable history");
  return result;
}

std::string EvolutionCoordinator::query_json_locked(std::string_view query,
                                                    const RuntimeComponentId& component) const {
  auto response_begin = [](JsonWriter& writer, std::string_view kind) {
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("query", kind);
  };
  JsonWriter writer;
  if (query == "authority") {
    return authority_locked(component).to_json();
  }
  if (query == "components") {
    response_begin(writer, query);
    writer.key("components");
    writer.begin_array();
    for (const auto* version : durable_.components.all_versions()) {
      writer.begin_object();
      writer.field("component", version->component.view());
      writer.field("version", version->version.to_string());
      writer.field("artifact", version->version.artifact.build.view());
      writer.field_generation("runtime_generation", version->generation);
      writer.field("lifecycle", ref::to_string(version->lifecycle));
      writer.field_generation("capability_generation", version->capability_generation);
      writer.field("supported_protocols", encode_generation_set(version->protocols.supported));
      writer.field("readable_protocols", encode_generation_set(version->protocols.readable));
      writer.field("writable_protocols", encode_generation_set(version->protocols.writable));
      writer.field("supported_schemas", encode_generation_set(version->schemas.supported));
      writer.field("readable_schemas", encode_generation_set(version->schemas.readable));
      writer.field("writable_schemas", encode_generation_set(version->schemas.writable));
      writer.field("provenance", ref::to_string(version->provenance));
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "plans" || query == "plan") {
    response_begin(writer, query);
    writer.key("plans");
    writer.begin_array();
    for (const auto& [id, plan] : durable_.plans) {
      (void)id;
      writer.begin_object();
      writer.field("plan", plan.id.view());
      writer.field_generation("plan_generation", plan.generation);
      writer.field("component", plan.component.view());
      writer.field_generation("current_generation", plan.current_generation);
      writer.field_generation("candidate_generation", plan.candidate_generation);
      writer.field("stage", ref::to_string(plan.stage));
      writer.field_generation("stage_generation", plan.stage_generation);
      writer.field_generation("evolution_epoch", plan.epoch);
      writer.field_generation("coordinator_epoch", plan.coordinator_epoch);
      writer.field_generation("matrix_generation", plan.matrix_generation);
      writer.field_generation("gate_generation", plan.gate_generation);
      writer.field_generation("protocol_generation", plan.protocol_generation);
      writer.field_generation("schema_generation", plan.schema_generation);
      writer.field_generation("migration_generation", plan.migration_generation);
      writer.field_generation("rollback_target", plan.rollback_target);
      writer.field("rollback_barrier_crossed", plan.rollback_barrier_crossed);
      writer.field("new_writer_enabled", plan.new_writer_enabled);
      writer.field("superseded", plan.superseded);
      writer.key("stage_history");
      writer.begin_array();
      for (const auto& record : plan.history) {
        writer.begin_object();
        writer.field("stage_id", record.stage_id.view());
        writer.field("stage", ref::to_string(record.stage));
        writer.field_generation("stage_generation", record.generation);
        writer.field_generation("evolution_epoch", record.epoch);
        writer.field("completed", record.completed);
        writer.field("note", record.note.view());
        writer.end_object();
      }
      writer.end_array();
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "compatibility") {
    response_begin(writer, query);
    writer.field_generation("matrix_generation", durable_.matrix.generation());
    writer.key("edges");
    writer.begin_array();
    for (const auto* edge : durable_.matrix.all_edges()) {
      writer.begin_object();
      writer.field_generation("from", edge->from);
      writer.field_generation("to", edge->to);
      writer.field("permissions", describe_permissions(edge->permissions));
      writer.field("requires_feature_gate", edge->requires_feature_gate);
      writer.field("requires_protocol_downgrade", edge->requires_protocol_downgrade);
      writer.field("requires_state_translation", edge->requires_state_translation);
      writer.field_generation("edge_generation", edge->generation);
      writer.key("aspects");
      writer.begin_object();
      for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
        writer.field(ref::to_string(static_cast<CompatAspect>(i)),
                     ref::to_string(edge->aspects[i].outcome));
      }
      writer.end_object();
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  } else if (query == "protocols") {
    response_begin(writer, query);
    writer.key("protocols");
    writer.begin_array();
    for (const auto* descriptor : durable_.protocols.all()) {
      writer.begin_object();
      writer.field("protocol", descriptor->id.view());
      writer.field_generation("generation", descriptor->generation);
      writer.field_generation("minimum_safety_generation", descriptor->minimum_safety_generation);
      writer.field("unknown_fields", ref::to_string(descriptor->unknown_fields));
      writer.field("unknown_messages", ref::to_string(descriptor->unknown_messages));
      writer.field("message_types", static_cast<std::uint64_t>(descriptor->messages.size()));
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "schemas") {
    response_begin(writer, query);
    writer.key("schemas");
    writer.begin_array();
    for (const auto* descriptor : durable_.schemas.all()) {
      writer.begin_object();
      writer.field("schema", descriptor->id.view());
      writer.field_generation("generation", descriptor->generation);
      writer.field_generation("readable_min", descriptor->readable_formats.min);
      writer.field_generation("readable_max", descriptor->readable_formats.max);
      writer.field_generation("writable_min", descriptor->writable_formats.min);
      writer.field_generation("writable_max", descriptor->writable_formats.max);
      writer.field_generation("reverse_migration_to", descriptor->reverse_migration_to);
      writer.field("irreversible", descriptor->has_irreversible_field());
      writer.field("canonicalization", ref::to_string(descriptor->canonicalization));
      writer.field("integrity", ref::to_string(descriptor->integrity));
      writer.field("fields", static_cast<std::uint64_t>(descriptor->fields.size()));
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "gates") {
    response_begin(writer, query);
    writer.field_generation("gate_generation", durable_.gates.generation());
    writer.key("gates");
    writer.begin_array();
    for (const auto* gate : durable_.gates.all()) {
      writer.begin_object();
      writer.field("feature", gate->id.view());
      writer.field_generation("generation", gate->generation);
      writer.field("enabled", gate->enabled);
      writer.field_generation("min_runtime_generation", gate->min_runtime_generation);
      writer.field_generation("min_protocol_generation", gate->min_protocol_generation);
      writer.field_generation("min_schema_generation", gate->min_schema_generation);
      writer.field("enabled_by", gate->enabled_by.view());
      writer.field("scope", gate->scope.view());
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "migrations") {
    response_begin(writer, query);
    writer.key("migrations");
    writer.begin_array();
    for (const auto& record : durable_.migrations) {
      writer.begin_object();
      writer.field("migration", record.id.view());
      writer.field_generation("migration_generation", record.generation);
      writer.field("schema", record.schema.view());
      writer.field_generation("source", record.source);
      writer.field_generation("target", record.target);
      writer.field("outcome", ref::to_string(record.outcome));
      writer.field("irreversible", record.irreversible);
      writer.field("rollback_available", record.rollback_available);
      writer.field("plan", record.plan.view());
      writer.end_object();
    }
    writer.end_array();
    writer.key("pending_actions");
    writer.begin_array();
    for (const auto& action : pending_actions_) {
      writer.begin_object();
      writer.field("action_id", action.action_id);
      writer.field("plan", action.plan.view());
      writer.field("worker", action.worker.view());
      writer.field_generation("boot", action.boot);
      writer.field_generation("source", action.source);
      writer.field_generation("target", action.target);
      writer.field("state", static_cast<std::uint64_t>(action.state));
      writer.field("detail", action.detail.view());
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "workers") {
    response_begin(writer, query);
    writer.field_generation("coordinator_epoch", durable_.coordinator_epoch);
    writer.field_generation("evolution_epoch", durable_.evolution_epoch);
    writer.key("workers");
    writer.begin_array();
    for (const auto& [id, worker] : live_workers_) {
      (void)id;
      writer.begin_object();
      writer.field("worker", worker.worker.view());
      writer.field("component", worker.component.view());
      writer.field_generation("generation", worker.generation);
      writer.field_generation("boot", worker.boot);
      writer.field_generation("negotiated_protocol", worker.negotiated_protocol);
      writer.field_generation("committed_schema", worker.committed_schema);
      writer.field("operation_class", ref::to_string(worker.operation));
      writer.field("active", worker.active);
      writer.field("draining", worker.draining);
      writer.field("fenced", worker.fenced);
      writer.field("frames", worker.frames);
      writer.end_object();
    }
    writer.end_array();
    writer.key("durable_worker_records");
    writer.begin_array();
    for (const auto& [id, record] : durable_.workers) {
      (void)id;
      writer.begin_object();
      writer.field("worker", record.worker.view());
      writer.field("component", record.component.view());
      writer.field_generation("generation", record.generation);
      writer.field_generation("last_boot", record.last_boot);
      writer.field("fenced", record.fenced);
      writer.field("drained", record.drained);
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "epochs") {
    response_begin(writer, query);
    writer.field_generation("coordinator_epoch", durable_.coordinator_epoch);
    writer.field_generation("evolution_epoch", durable_.evolution_epoch);
    writer.field_generation("stage_generation", durable_.stage_generation);
    writer.field_generation("matrix_generation", durable_.matrix.generation());
    writer.field_generation("gate_generation", durable_.gates.generation());
    writer.field_generation("evidence_generation", durable_.evidence_generation);
    writer.field_generation("snapshot_generation", durable_.snapshot_generation);
    writer.field_generation("migration_generation", durable_.migration_generation);
    writer.field_generation("rollback_generation", durable_.rollback_generation);
    writer.field_generation("component_epoch", epoch_for_locked(component));
    writer.end_object();
  } else if (query == "retirement") {
    response_begin(writer, query);
    writer.key("retirements");
    writer.begin_array();
    for (const auto& [key, record] : durable_.retirements) {
      (void)key;
      writer.begin_object();
      writer.field("component", record.component.view());
      writer.field_generation("generation", record.generation);
      writer.field_generation("recorded_at", record.recorded_at);
      writer.field_generation("evolution_epoch", record.epoch);
      writer.field_generation("coordinator_epoch", record.coordinator_epoch);
      writer.field("checkpoints_retained", record.checkpoints_retained);
      writer.end_object();
    }
    writer.end_array();
    writer.key("fenced_boots");
    writer.begin_array();
    for (const auto& [key, record] : durable_.fenced_boots) {
      (void)key;
      writer.begin_object();
      writer.field("worker", record.worker.view());
      writer.field_generation("boot", record.boot);
      writer.field("component", record.component.view());
      writer.field_generation("generation", record.generation);
      writer.field("reason", record.reason.view());
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "snapshots") {
    response_begin(writer, query);
    writer.key("snapshots");
    writer.begin_array();
    for (const auto& snapshot : snapshots_) {
      writer.begin_object();
      writer.field_generation("snapshot_generation", snapshot.generation);
      writer.field_generation("evolution_epoch", snapshot.epoch);
      writer.field_generation("coordinator_epoch", snapshot.coordinator_epoch);
      writer.field("canonical_bytes", static_cast<std::uint64_t>(snapshot.canonical.size()));
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "reconcile") {
    response_begin(writer, query);
    const ReconcileReport report = reconcile_locked();
    writer.field("requires_operator", report.requires_operator);
    writer.key("findings");
    writer.begin_array();
    for (const auto& finding : report.findings) {
      writer.begin_object();
      writer.field("kind", ref::to_string(finding.kind));
      writer.field("component", finding.component.view());
      writer.field_generation("expected_generation", finding.expected_generation);
      writer.field_generation("observed_generation", finding.observed_generation);
      writer.field_generation("expected_protocol", finding.expected_protocol);
      writer.field_generation("observed_protocol", finding.observed_protocol);
      writer.field_generation("expected_schema", finding.expected_schema);
      writer.field_generation("observed_schema", finding.observed_schema);
      writer.field("worker", finding.worker.view());
      writer.field("conservative_action_required", finding.conservative_action_required);
      writer.field("detail", finding.detail.view());
      writer.end_object();
    }
    writer.end_array();
  } else if (query == "explain") {
    response_begin(writer, query);
    const EvolutionPlan* plan = active_plan_locked(component);
    writer.key("explanation");
    writer.begin_array();
    if (plan != nullptr) {
      const CompatibilityEdge* edge = durable_.matrix.find(plan->current_generation, plan->candidate_generation);
      if (edge != nullptr) {
        for (const auto& line : durable_.matrix.explain(plan->current_generation, plan->candidate_generation)) {
          writer.string(line);
        }
      } else {
        writer.string("no compatibility edge is recorded for the active plan pair");
      }
      const StagePreconditions preconditions = stage_preconditions_locked(*plan);
      writer.string(std::string("stage ") + ref::to_string(plan->stage) + " at generation " +
                    std::to_string(plan->stage_generation.raw()));
      writer.string(std::string("compatibility proven: ") +
                    (preconditions.compatibility_proven ? "yes" : "no"));
      writer.string(std::string("evidence current: ") + (preconditions.evidence_current ? "yes" : "no"));
      writer.string(std::string("old writers drained: ") +
                    (preconditions.old_writers_drained ? "yes" : "no"));
      writer.string(std::string("new writer fence satisfied: ") +
                    (preconditions.new_writer_fence_satisfied ? "yes" : "no"));
      writer.string(std::string("rollback valid: ") + (preconditions.rollback_valid ? "yes" : "no"));
      writer.string(std::string("rollback barrier crossed: ") +
                    (plan->rollback_barrier_crossed ? "yes" : "no"));
    } else {
      writer.string("no active evolution plan for this component");
    }
    writer.end_array();
  } else {
    writer.begin_object();
    writer.field("status", "INVALID_ARGUMENT");
    writer.field("detail", "unknown query kind");
    writer.end_object();
  }
  // Defensive net: whatever path produced this document, it leaves the writer
  // balanced, so a truncated document can never be handed to a peer or an
  // operator.
  writer.close_all();
  return writer.str();
}

CommandResult EvolutionCoordinator::command_query(const PeerIdentity& peer, std::string_view query,
                                               const RuntimeComponentId& requested_component) {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  if (!peer.is_operator && !peer.registered) {
    return internal::failure_result(
        Status::failure(ErrorCode::Unauthorized, "queries require a registered runtime generation"));
  }
  RuntimeComponentId component = requested_component;
  if (component.empty()) component = peer.component;
  if (component.empty()) {
    const auto components = durable_.components.components();
    if (!components.empty()) component = components.front();
  }
  const std::string json = query_json_locked(query, component);
  if (json.size() > limits::kQueryBytes) {
    return internal::failure_result(Status::failure(ErrorCode::LimitExceeded, "query result exceeds the bound"));
  }
  CommandResult result;
  result.status = Status::ok();
  result.json = json;
  result.explanation.push_back("read-only inspection completed");
  return result;
}

}  // namespace ref