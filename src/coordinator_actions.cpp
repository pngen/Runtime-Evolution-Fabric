// Runtime Evolution Fabric - coordinator authority transitions.
//
// Drain, migration dispatch, rollback, retirement, supersession, fencing and
// completion handling. External side effects are registered before dispatch and
// are never treated as complete on submission.
#include "coordinator_internal.hpp"
#include "ref/coordinator.hpp"

#include <algorithm>

namespace ref {
namespace {
using internal::component_state_schema_id;
using internal::field_id;
using internal::ok_json;
using internal::ok_result;
using internal::shadow_query_feature_id;
using internal::token_transform_feature_id;

CommandResult failure_json(const Status& status) { return internal::failure_result(status); }

}  // namespace

Status EvolutionCoordinator::install_builtin_features() {
  std::string encoded;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    // Bootstrap only defines gates that do not exist yet: a restart must never
    // resurrect, re-enable or re-disable a gate the operator has decided on.
    const bool shadow_exists = durable_.gates.find(shadow_query_feature_id()) != nullptr;
    const bool transform_exists = durable_.gates.find(token_transform_feature_id()) != nullptr;
    if (shadow_exists && transform_exists) {
      durable_.gate_generation = durable_.gates.generation();
      const Status encoded_status = encode_locked(encoded);
      if (encoded_status.is_failure()) return encoded_status;
      return persist(encoded);
    }
    FeatureGate shadow;
    shadow.id = shadow_query_feature_id();
    shadow.generation = FeatureGateGeneration::first();
    shadow.min_runtime_generation = RuntimeGeneration::from_raw(1);
    shadow.min_protocol_generation = ProtocolGeneration::from_raw(1);
    shadow.min_schema_generation = SchemaGeneration::from_raw(1);
    shadow.required_peer_aspect = CompatAspect::PeerVersion;
    shadow.required_peer_outcome = CompatOutcome::MixedVersionCompatible;
    shadow.provenance = EvidenceClass::Real;
    shadow.evidence = EvidenceGeneration::first();
    shadow.notes = NoteText::from_valid("read-only shadow queries on a candidate runtime");
    const Status shadow_status = durable_.gates.define(shadow);
    if (shadow_status.is_failure()) return shadow_status;

    FeatureGate transform;
    transform.id = token_transform_feature_id();
    transform.generation = FeatureGateGeneration::first();
    transform.min_runtime_generation = RuntimeGeneration::from_raw(2);
    transform.min_protocol_generation = ProtocolGeneration::from_raw(2);
    transform.min_schema_generation = SchemaGeneration::from_raw(3);
    transform.required_peer_aspect = CompatAspect::PeerVersion;
    transform.required_peer_outcome = CompatOutcome::FullyCompatible;
    transform.provenance = EvidenceClass::Real;
    transform.evidence = EvidenceGeneration::first();
    transform.notes = NoteText::from_valid("protocol-2 state transformation; requires drained old writers");
    const Status transform_status = durable_.gates.define(transform);
    if (transform_status.is_failure()) return transform_status;
    durable_.gate_generation = durable_.gates.generation();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return encoded_status;
  }
  return persist(encoded);
}

// ---------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_request_drain(const PeerIdentity& peer,
                                                          const RuntimeComponentId& component,
                                                          RuntimeGeneration generation) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    if (!peer.is_operator && !(peer.component == component && peer.runtime_generation == generation)) {
      return failure_json(Status::failure(ErrorCode::Unauthorized,
                                          "drain requests require operator authority or the drained generation"));
    }
    if (component.empty() || !generation.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "drain requires a component and generation"));
    }
    const RuntimeComponentVersion* version = durable_.components.find(component, generation);
    if (version == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "runtime generation is not registered"));
    }
    if (is_terminal_lifecycle(version->lifecycle)) {
      return failure_json(Status::failure(ErrorCode::RetiredGeneration, "runtime generation is retired"));
    }
    std::uint64_t marked = 0;
    for (auto& [id, worker] : live_workers_) {
      (void)id;
      if (worker.component != component || worker.generation != generation) continue;
      worker.draining = true;
      ++marked;
    }
    Status transition = Status::ok();
    if (version->lifecycle != LifecycleState::Draining) {
      transition = advance_lifecycle_locked(component, generation, LifecycleState::Draining);
      if (transition.is_failure() && version->lifecycle != LifecycleState::Current &&
          version->lifecycle != LifecycleState::MixedVersionActive &&
          version->lifecycle != LifecycleState::RolloutActive &&
          version->lifecycle != LifecycleState::Eligible) {
        return failure_json(transition);
      }
    }
    const Status dispatched = dispatch_drain_locked(component, generation);
    if (dispatched.is_failure() && dispatched.code() != ErrorCode::NotFound) {
      return failure_json(dispatched);
    }
    refresh_worker_authority_locked(component);
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);

    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("component", component.view());
    writer.field_generation("generation", generation);
    writer.field("workers_marked", marked);
    writer.field("live_processes", live_worker_count_locked(component, generation));
    writer.end_object();
    result = ok_json(writer.str(), "drain recorded; workers must report completion before retirement");
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

// ---------------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------------
Status EvolutionCoordinator::dispatch_migration_locked(const EvolutionPlan& plan, const WorkerId& worker,
                                                       const WorkerBootId& boot) {
  for (const auto& action : pending_actions_) {
    if (action.plan == plan.id && action.worker == worker && action.state != ActionState::Acknowledged &&
        action.state != ActionState::Abandoned) {
      return Status::failure(ErrorCode::Conflict,
                             "a migration action for this plan and worker is already outstanding");
    }
  }
  if (pending_actions_.size() >= limits::kMigrationRecords) {
    return Status::failure(ErrorCode::LimitExceeded, "pending action bound reached");
  }
  PendingAction action;
  action.action_id = ++action_counter_;
  action.plan = plan.id;
  action.worker = worker;
  action.boot = boot;
  action.migration = plan.migration;
  action.migration_generation = plan.migration_generation;
  action.source = plan.migration_source;
  action.target = plan.migration_target;
  action.state = ActionState::Registered;
  action.stage = plan.stage_generation;
  action.detail = NoteText::from_valid("migration attempt registered before dispatch");
  pending_actions_.push_back(action);

  WireMessage payload;
  (void)payload.set_ident(FieldId::from_valid("plan"), plan.id);
  (void)payload.set_ident(FieldId::from_valid("worker"), worker);
  (void)payload.set_generation(FieldId::from_valid("boot"), boot);
  (void)payload.set_ident(FieldId::from_valid("migration"), plan.migration);
  (void)payload.set_generation(FieldId::from_valid("migration_generation"), plan.migration_generation);
  (void)payload.set_generation(FieldId::from_valid("source"), plan.migration_source);
  (void)payload.set_generation(FieldId::from_valid("target"), plan.migration_target);
  (void)payload.set_u64(FieldId::from_valid("action_id"), action.action_id);
  const SchemaDescriptor* target_schema =
      durable_.schemas.find(component_state_schema_id(), plan.migration_target);
  const bool irreversible = target_schema != nullptr &&
                            (target_schema->has_irreversible_field() ||
                             !target_schema->reverse_migration_to.is_set());
  (void)payload.set_bool(FieldId::from_valid("irreversible"), irreversible);
  Frame frame;
  frame.header.type = MessageType::RequestMigration;
  frame.header.runtime_generation = plan.candidate_generation;
  frame.header.protocol_generation = plan.protocol_generation;
  frame.header.schema_generation = plan.schema_generation;
  frame.header.coordinator_epoch = durable_.coordinator_epoch;
  frame.header.evolution_epoch = plan.epoch;
  frame.header.boot = boot;
  const Status sent = send_to_worker_locked(worker, MessageType::RequestMigration, payload, frame);
  auto& stored = pending_actions_.back();
  if (sent.is_failure()) {
    // Submission failed: the outcome is ambiguous, never assumed.
    stored.state = ActionState::OutcomeUnknown;
    stored.detail = NoteText::from_valid("dispatch failed; outcome unknown and not retried automatically");
    return Status::failure(ErrorCode::OutcomeUnknown,
                           "migration dispatch failed; the attempt is recorded as OUTCOME_UNKNOWN");
  }
  stored.state = ActionState::Dispatched;
  return Status::ok();
}

CommandResult EvolutionCoordinator::command_request_migration(const PeerIdentity& peer,
                                                              const EvolutionPlanId& plan_id) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    EvolutionPlan* plan = mutable_plan_locked(plan_id);
    if (plan == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
    if (plan->superseded) {
      return failure_json(Status::failure(ErrorCode::StalePlan, "superseded plan cannot dispatch migration"));
    }
    if (plan->migration.empty() || !plan->migration_generation.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "plan binds no migration"));
    }
    if (rollout_stage_rank(plan->stage) < rollout_stage_rank(RolloutStage::MigrationBarrier)) {
      return failure_json(Status::failure(
          ErrorCode::Conflict, "migration may only be dispatched once the migration barrier stage is reached"));
    }
    const SchemaDescriptor* target = durable_.schemas.find(component_state_schema_id(), plan->migration_target);
    if (target == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "migration target schema generation is unknown"));
    }
    // Pick a live candidate worker that currently holds mutation authority.
    const WorkerId* chosen = nullptr;
    WorkerBootId chosen_boot{};
    for (const auto& [id, worker] : live_workers_) {
      if (worker.component != plan->component) continue;
      if (worker.generation != plan->candidate_generation) continue;
      if (!worker.active || worker.fenced) continue;
      if (!permits_mutation(worker.operation)) continue;
      chosen = &id;
      chosen_boot = worker.boot;
      break;
    }
    if (chosen == nullptr) {
      return failure_json(Status::failure(
          ErrorCode::NotFound, "no candidate worker currently holds mutation authority for this migration"));
    }
    const Status dispatched = dispatch_migration_locked(*plan, *chosen, chosen_boot);
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
    if (dispatched.is_failure()) {
      result = failure_json(dispatched);
      result.explanation.push_back("attempt recorded; outcome unknown outcomes require reconciliation");
    } else {
      JsonWriter writer;
      writer.begin_object();
      writer.field("status", "OK");
      writer.field("plan", plan->id.view());
      writer.field("worker", chosen->view());
      writer.field_generation("migration_generation", plan->migration_generation);
      writer.field_generation("source", plan->migration_source);
      writer.field_generation("target", plan->migration_target);
      writer.end_object();
      result = ok_json(writer.str(), "migration dispatched; completion is required before authority advances");
    }
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

Status EvolutionCoordinator::dispatch_drain_locked(const RuntimeComponentId& component,
                                                   RuntimeGeneration generation) {
  bool any = false;
  for (const auto& [id, worker] : live_workers_) {
    if (worker.component != component || worker.generation != generation) continue;
    any = true;
    WireMessage payload;
    (void)payload.set_ident(FieldId::from_valid("component"), component);
    (void)payload.set_generation(FieldId::from_valid("runtime_generation"), generation);
    const EvolutionPlan* plan = active_plan_locked(component);
    if (plan != nullptr) {
      (void)payload.set_ident(FieldId::from_valid("plan"), plan->id);
      (void)payload.set_generation(FieldId::from_valid("stage_generation"), plan->stage_generation);
    }
    Frame frame;
    frame.header.type = MessageType::RequestDrain;
    frame.header.runtime_generation = generation;
    frame.header.protocol_generation = worker.negotiated_protocol;
    frame.header.schema_generation = worker.committed_schema;
    frame.header.coordinator_epoch = durable_.coordinator_epoch;
    frame.header.boot = worker.boot;
    frame.header.evolution_epoch = epoch_for_locked(component);
    (void)send_to_worker_locked(id, MessageType::RequestDrain, payload, frame);
  }
  if (!any) return Status::failure(ErrorCode::NotFound, "no live workers for the requested generation");
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_request_rollback(const PeerIdentity& peer,
                                                            const EvolutionPlanId& plan_id) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    EvolutionPlan* plan = mutable_plan_locked(plan_id);
    if (plan == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
    if (plan->stage == RolloutStage::RolledBack) {
      return failure_json(Status::failure(ErrorCode::Conflict, "plan has already been rolled back"));
    }
    const RuntimeComponentVersion* target =
        durable_.components.find(plan->component, plan->rollback_target);
    bool target_available = target != nullptr && !is_terminal_lifecycle(target->lifecycle);
    if (target_available) {
      for (const auto& [id, worker] : live_workers_) {
        (void)id;
        if (worker.component == plan->component && worker.generation == plan->rollback_target && worker.active) {
          target_available = true;
          break;
        }
      }
    }
    const SchemaDescriptor* schema = durable_.schemas.find(component_state_schema_id(), plan->schema_generation);
    const bool reverse_migration_available =
        schema != nullptr && schema->reverse_migration_to.is_set() && !plan->rollback_barrier_crossed;

    RollbackRequest request;
    request.id = RollbackId::from_valid("rollback-" + std::to_string(plan->rollback_generation.raw()));
    request.generation = plan->rollback_generation.next();
    request.component = plan->component;
    request.plan = plan->id;
    request.plan_generation = plan->generation;
    request.schema = component_state_schema_id();
    request.current_generation = plan->candidate_generation;
    request.target_generation = plan->rollback_target;
    request.current_protocol = plan->protocol_generation;
    const RuntimeComponentVersion* target_version = target;
    request.target_protocol =
        target_version != nullptr ? target_version->protocols.supported.highest() : ProtocolGeneration::unset();
    request.current_schema = plan->schema_generation;
    request.target_schema =
        target_version != nullptr ? target_version->schemas.writable.highest() : SchemaGeneration::unset();
    request.irreversible_migration_crossed = plan->rollback_barrier_crossed;
    request.reverse_migration_available = reverse_migration_available;
    request.state_restore_available = reverse_migration_available;
    request.target_process_available = target_available;
    request.plan_superseded = plan->superseded;
    request.gate_generation = durable_.gates.generation();
    request.policy = plan->policy.generation;
    request.epoch = plan->epoch;
    request.coordinator_epoch = durable_.coordinator_epoch;
    request.evidence = durable_.evidence_generation;
    request.evidence_floor = durable_.evidence_generation;
    request.evidence_current = true;
    request.current_stage = plan->stage;

    const RollbackAssessment assessment =
        evaluate_rollback(request, durable_.matrix, durable_.components, durable_.schemas);

    RollbackRecord record;
    record.id = request.id;
    record.generation = request.generation;
    record.component = plan->component;
    record.from_generation = request.current_generation;
    record.to_generation = request.target_generation;
    record.plan = plan->id;
    record.outcome = assessment.outcome;
    record.epoch = plan->epoch;
    record.coordinator_epoch = durable_.coordinator_epoch;
    record.recorded_at = durable_.stage_generation;
    record.detail = DetailText::from_valid(assessment.explanation.empty() ? "rollback evaluated"
                                                                          : assessment.explanation.back());

    if (!assessment.is_allowed()) {
      if (durable_.rollbacks.size() >= limits::kRollbackRecords) {
        return failure_json(Status::failure(ErrorCode::LimitExceeded, "rollback history bound reached"));
      }
      durable_.rollbacks.push_back(record);
      const Status encoded_status = encode_locked(encoded);
      if (encoded_status.is_failure()) return failure_json(encoded_status);
      result = failure_json(assessment.status);
      result.explanation = assessment.explanation;
      JsonWriter writer;
      writer.begin_object();
      writer.field("status", to_string(assessment.status.code()));
      writer.field("detail", assessment.status.detail());
      writer.field("rollback_outcome", to_string(assessment.outcome));
      writer.end_object();
      result.json = writer.str();
      const Status stored = persist(encoded);
      if (stored.is_failure()) return failure_json(stored);
      return result;
    }

    // Apply: candidate loses authority, the recorded target resumes it.
    const RuntimeComponentVersion* candidate =
        durable_.components.find(plan->component, plan->candidate_generation);
    if (candidate != nullptr) {
      Status transition = advance_lifecycle_locked(plan->component, plan->candidate_generation,
                                                   LifecycleState::RollbackPending);
      if (transition.is_ok()) {
        transition = advance_lifecycle_locked(plan->component, plan->candidate_generation,
                                              LifecycleState::RollingBack);
      }
      if (transition.is_ok()) {
        transition = advance_lifecycle_locked(plan->component, plan->candidate_generation,
                                              LifecycleState::RolledBack);
      }
      if (transition.is_failure()) {
        return failure_json(transition);
      }
    }
    const RuntimeComponentVersion* target_now =
        durable_.components.find(plan->component, plan->rollback_target);
    if (target_now != nullptr) {
      Status transition = Status::ok();
      if (target_now->lifecycle == LifecycleState::Draining) {
        transition = advance_lifecycle_locked(plan->component, plan->rollback_target, LifecycleState::RolledBack);
        if (transition.is_ok()) {
          target_now = durable_.components.find(plan->component, plan->rollback_target);
        }
      }
      if (transition.is_ok() && target_now != nullptr &&
          target_now->lifecycle != LifecycleState::Current) {
        transition = advance_lifecycle_locked(plan->component, plan->rollback_target, LifecycleState::Current);
      }
      if (transition.is_failure()) return failure_json(transition);
    }
    const Status dispatched = dispatch_rollback_state_locked(*plan, WorkerId{}, WorkerBootId{});
    if (dispatched.is_failure() && dispatched.code() != ErrorCode::NotFound) {
      return failure_json(dispatched);
    }
    plan->stage = RolloutStage::RolledBack;
    plan->rollback_generation = request.generation;
    plan->new_writer_enabled = false;
    plan->stage_generation = durable_.stage_generation.next();
    durable_.stage_generation = plan->stage_generation;
    PlanStageRecord stage_record;
    stage_record.stage_id = RolloutStageId::from_valid("stage-" + std::to_string(plan->stage_generation.raw()));
    stage_record.stage = RolloutStage::RolledBack;
    stage_record.generation = plan->stage_generation;
    stage_record.epoch = plan->epoch;
    stage_record.coordinator_epoch = durable_.coordinator_epoch;
    stage_record.evidence = durable_.evidence_generation;
    stage_record.note = NoteText::from_valid("rollback committed; candidate generation lost authority");
    plan->history.push_back(stage_record);
    durable_.stage_history.push_back(stage_record);
    if (durable_.stage_history.size() > limits::kStageHistory) {
      durable_.stage_history.erase(durable_.stage_history.begin());
    }
    record.outcome = RollbackOutcome::Committed;
    record.recorded_at = plan->stage_generation;
    record.detail = DetailText::from_valid("rollback committed");
    if (durable_.rollbacks.size() < limits::kRollbackRecords) durable_.rollbacks.push_back(record);
    ++stats_.rollbacks_committed;
    refresh_worker_authority_locked(plan->component);
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);

    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("plan", plan->id.view());
    writer.field("rollback_outcome", to_string(RollbackOutcome::Committed));
    writer.field_generation("rollback_generation", plan->rollback_generation);
    writer.field_generation("authoritative_generation", plan->rollback_target);
    writer.end_object();
    result = ok_json(writer.str(), "rollback committed");
    result.explanation = assessment.explanation;
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

Status EvolutionCoordinator::dispatch_rollback_state_locked(const EvolutionPlan& plan, const WorkerId& worker,
                                                            const WorkerBootId& boot) {
  WireMessage payload;
  (void)payload.set_ident(FieldId::from_valid("plan"), plan.id);
  (void)payload.set_generation(FieldId::from_valid("target_schema"), plan.migration_source);
  (void)payload.set_ident(FieldId::from_valid("worker"), worker);
  (void)payload.set_generation(FieldId::from_valid("boot"), boot);
  Frame frame;
  frame.header.type = MessageType::RequestRollback;
  frame.header.runtime_generation = plan.rollback_target;
  frame.header.protocol_generation = plan.protocol_generation;
  frame.header.schema_generation = plan.migration_source;
  frame.header.coordinator_epoch = durable_.coordinator_epoch;
  frame.header.evolution_epoch = plan.epoch;
  if (worker.empty()) return Status::failure(ErrorCode::NotFound, "no worker session to notify");
  return send_to_worker_locked(worker, MessageType::RequestRollback, payload, frame);
}

// ---------------------------------------------------------------------------
// Retirement and supersession
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_request_retirement(const PeerIdentity& peer,
                                                               const RuntimeComponentId& component,
                                                               RuntimeGeneration generation) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    const RuntimeComponentVersion* version = durable_.components.find(component, generation);
    if (version == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "runtime generation is not registered"));
    }
    if (is_terminal_lifecycle(version->lifecycle)) {
      return failure_json(Status::failure(ErrorCode::RetiredGeneration, "runtime generation is already retired"));
    }
    RetirementPreconditions preconditions;
    preconditions.component = component;
    preconditions.generation = generation;
    for (const auto& [id, worker] : live_workers_) {
      (void)id;
      if (worker.component != component || worker.generation != generation) continue;
      if (worker.fenced) continue;
      ++preconditions.live_processes;
      if (worker.active && !worker.draining) ++preconditions.active_workers;
    }
    for (const auto& [id, record] : durable_.workers) {
      (void)id;
      if (record.component != component || record.generation != generation) continue;
      if (record.fenced) continue;
      if (!record.drained) preconditions.pending_work = true;
    }
    bool incompatible_writers_fenced = true;
    for (const auto& [id, worker] : live_workers_) {
      (void)id;
      if (worker.component != component || worker.generation != generation) continue;
      if (!worker.fenced && !worker.draining) incompatible_writers_fenced = false;
    }
    preconditions.incompatible_writers_fenced = incompatible_writers_fenced;
    const EvolutionPlan* plan = active_plan_locked(component);
    bool migration_complete = true;
    if (plan != nullptr && !plan->migration.empty()) {
      migration_complete = false;
      for (const auto& record : durable_.migrations) {
        if (record.plan != plan->id) continue;
        if (record.outcome == MigrationOutcome::Committed || record.outcome == MigrationOutcome::RollbackAvailable ||
            record.outcome == MigrationOutcome::Irreversible) {
          migration_complete = true;
        }
      }
    }
    preconditions.migration_complete = migration_complete;
    preconditions.superseded = plan != nullptr && plan->superseded;
    // Retirement of the rollback target is only legal when the plan has crossed
    // its barrier or explicitly permits it.
    preconditions.rollback_policy_allows_retirement =
        plan == nullptr || plan->rollback_barrier_crossed ||
        plan->rollback_target != generation;
    preconditions.checkpoints_retained = true;
    for (const auto& record : durable_.migrations) {
      if (record.plan.empty()) continue;
      if (plan != nullptr && record.plan != plan->id) continue;
      if (record.irreversible && !record.rollback_metadata_preserved) preconditions.checkpoints_retained = false;
    }
    const RetirementAssessment assessment = evaluate_retirement(preconditions);
    if (!assessment.allowed) {
      result = failure_json(assessment.status);
      result.explanation = assessment.explanation;
      return result;
    }
    if (version->lifecycle != LifecycleState::RetirementPending) {
      Status transition = advance_lifecycle_locked(component, generation, LifecycleState::RetirementPending);
      if (transition.is_failure()) {
        if (version->lifecycle == LifecycleState::Failed || version->lifecycle == LifecycleState::Draining ||
            version->lifecycle == LifecycleState::Current || version->lifecycle == LifecycleState::Eligible) {
          transition = advance_lifecycle_locked(component, generation, LifecycleState::Draining);
          if (transition.is_ok()) {
            transition = advance_lifecycle_locked(component, generation, LifecycleState::RetirementPending);
          }
        }
      }
      if (transition.is_failure()) return failure_json(transition);
    }
    const Status retired = advance_lifecycle_locked(component, generation, LifecycleState::Retired);
    if (retired.is_failure()) return failure_json(retired);
    if (durable_.retirements.size() >= limits::kRetiredGenerations) {
      return failure_json(Status::failure(ErrorCode::LimitExceeded, "retirement table bound reached"));
    }
    RetirementRecord record;
    record.component = component;
    record.generation = generation;
    record.recorded_at = durable_.stage_generation;
    record.epoch = durable_.evolution_epoch;
    record.coordinator_epoch = durable_.coordinator_epoch;
    record.checkpoints_retained = preconditions.checkpoints_retained;
    record.detail = DetailText::from_valid("retired; reconnects and late frames are rejected");
    durable_.retirements[std::make_pair(component, generation.raw())] = record;

    // Fence every boot of the retired generation.
    std::vector<WorkerId> to_fence;
    for (auto& [id, worker] : live_workers_) {
      if (worker.component != component || worker.generation != generation) continue;
      worker.fenced = true;
      worker.active = false;
      to_fence.push_back(id);
      FencedBoot fenced;
      fenced.worker = id;
      fenced.boot = worker.boot;
      fenced.component = component;
      fenced.generation = generation;
      fenced.reason = NoteText::from_valid("runtime generation retired");
      fenced.fenced_at = durable_.stage_generation;
      if (durable_.fenced_boots.size() < limits::kFencedBoots) {
        durable_.fenced_boots[std::make_pair(id, worker.boot.raw())] = fenced;
      }
    }
    for (auto& [id, worker] : durable_.workers) {
      if (worker.component != component || worker.generation != generation) continue;
      worker.fenced = true;
    }
    ++stats_.retirements_committed;
    ++stats_.fenced_boots;
    refresh_worker_authority_locked(component);
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);

    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("component", component.view());
    writer.field_generation("generation", generation);
    writer.field("fenced_workers", static_cast<std::uint64_t>(to_fence.size()));
    writer.end_object();
    result = ok_json(writer.str(), "runtime generation retired; authority is terminal");
    result.explanation = assessment.explanation;
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

CommandResult EvolutionCoordinator::command_supersede_plan(const PeerIdentity& peer,
                                                           const EvolutionPlanId& plan_id) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    EvolutionPlan* plan = mutable_plan_locked(plan_id);
    if (plan == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
    if (plan->superseded) {
      return failure_json(Status::failure(ErrorCode::Conflict, "plan is already superseded"));
    }
    plan->superseded = true;
    plan->stage = RolloutStage::Superseded;
    plan->generation = plan->generation.next();
    plan->stage_generation = durable_.stage_generation.next();
    durable_.stage_generation = plan->stage_generation;
    PlanStageRecord record;
    record.stage_id = RolloutStageId::from_valid("stage-" + std::to_string(plan->stage_generation.raw()));
    record.stage = RolloutStage::Superseded;
    record.generation = plan->stage_generation;
    record.epoch = plan->epoch;
    record.coordinator_epoch = durable_.coordinator_epoch;
    record.evidence = durable_.evidence_generation;
    record.note = NoteText::from_valid("plan superseded; late completions are rejected");
    plan->history.push_back(record);
    durable_.stage_history.push_back(record);
    if (durable_.stage_history.size() > limits::kStageHistory) {
      durable_.stage_history.erase(durable_.stage_history.begin());
    }
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("plan", plan->id.view());
    writer.field("stage", to_string(plan->stage));
    writer.end_object();
    result = ok_json(writer.str(), "plan superseded; historical record preserved");
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

// ---------------------------------------------------------------------------
// Lifecycle administration
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_request_lifecycle(const PeerIdentity& peer,
                                                              const RuntimeComponentId& component,
                                                              RuntimeGeneration generation,
                                                              LifecycleState next) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    if (component.empty() || !generation.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "lifecycle change requires a generation"));
    }
    const RuntimeComponentVersion* version = durable_.components.find(component, generation);
    if (version == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "runtime generation is not registered"));
    }
    if (is_terminal_lifecycle(version->lifecycle)) {
      return failure_json(Status::failure(ErrorCode::RetiredGeneration,
                                          "RETIRED is terminal for this runtime generation"));
    }
    // Promotion to CURRENT is only reachable through the states that carry its
    // preconditions, so the chain is walked explicitly rather than jumped.
    Status transition = Status::ok();
    if (next == LifecycleState::Current && version->lifecycle == LifecycleState::Registered) {
      transition = advance_lifecycle_locked(component, generation, LifecycleState::CompatibilityPending);
      if (transition.is_ok()) {
        transition = advance_lifecycle_locked(component, generation, LifecycleState::Eligible);
      }
    }
    if (transition.is_ok()) transition = advance_lifecycle_locked(component, generation, next);
    if (transition.is_failure()) return failure_json(transition);
    refresh_worker_authority_locked(component);
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("component", component.view());
    writer.field_generation("runtime_generation", generation);
    writer.field("lifecycle", ref::to_string(next));
    writer.end_object();
    result = ok_json(writer.str(), "lifecycle transition committed");
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

// ---------------------------------------------------------------------------
// Fencing
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_fence(const PeerIdentity& peer, const WorkerId& worker,
                                                  WorkerBootId boot, std::string_view reason) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    if (worker.empty() || !boot.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "fencing requires a worker and boot identity"));
    }
    if (durable_.fenced_boots.size() >= limits::kFencedBoots) {
      return failure_json(Status::failure(ErrorCode::LimitExceeded, "fenced boot table bound reached"));
    }
    FencedBoot record;
    record.worker = worker;
    record.boot = boot;
    record.reason = NoteText::from_valid(reason);
    record.fenced_at = durable_.stage_generation;
    const auto live = live_workers_.find(worker);
    if (live != live_workers_.end()) {
      record.component = live->second.component;
      record.generation = live->second.generation;
      live->second.fenced = true;
      live->second.active = false;
    }
    const auto stored_worker = durable_.workers.find(worker);
    if (stored_worker != durable_.workers.end()) {
      if (record.component.empty()) record.component = stored_worker->second.component;
      if (!record.generation.is_set()) record.generation = stored_worker->second.generation;
      stored_worker->second.fenced = true;
      stored_worker->second.detail = DetailText::from_valid("fenced by operator");
    }
    durable_.fenced_boots[std::make_pair(worker, boot.raw())] = record;
    ++stats_.fenced_boots;
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("worker", worker.view());
    writer.field_generation("boot", boot);
    writer.end_object();
    result = ok_json(writer.str(), "boot identity fenced durably");
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_publish_completion(const PeerIdentity& peer,
                                                               const EvolutionPlanId& plan_id,
                                                               const RolloutStageId& stage_id,
                                                               StageGeneration stage_generation,
                                                               MigrationOutcome migration_outcome,
                                                               IntegrityDigest state_digest, bool drain) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    if (stage_id.empty()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "completion has no stage identity"));
    }
    // Drain completions mark a worker as finished with its work. They are bound
    // to the plan only when the coordinator announced one for that drain.
    const bool drain_completion = drain || stage_id.view().rfind("drain", 0) == 0;
    if (drain_completion) {
      EvolutionPlan* plan = plan_id.empty() ? nullptr : mutable_plan_locked(plan_id);
      if (!plan_id.empty() && plan == nullptr) {
        ++stats_.completions_rejected;
        return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
      }
      if (plan != nullptr && stage_generation.is_set() && stage_generation != plan->stage_generation) {
        ++stats_.completions_rejected;
        return failure_json(
            Status::failure(ErrorCode::StalePlan, "drain completion targets a superseded stage generation"));
      }
      const auto live = live_workers_.find(peer.worker);
      if (live == live_workers_.end()) {
        ++stats_.completions_rejected;
        return failure_json(Status::failure(ErrorCode::StaleBoot, "worker is not current"));
      }
      if (live->second.boot != peer.boot) {
        ++stats_.completions_rejected;
        return failure_json(Status::failure(ErrorCode::StaleBoot, "boot identity does not match the session"));
      }
      if (!live->second.draining) {
        ++stats_.completions_rejected;
        return failure_json(Status::failure(ErrorCode::Conflict, "worker was not asked to drain"));
      }
      if (!live->second.active) {
        ++stats_.completions_rejected;
        return failure_json(Status::failure(ErrorCode::Conflict, "duplicate drain completion rejected"));
      }
      live->second.active = false;
      auto record = durable_.workers.find(peer.worker);
      if (record != durable_.workers.end()) {
        record->second.drained = true;
        record->second.detail = DetailText::from_valid("drained");
      }
      ++stats_.completions_accepted;
      durable_.evidence_generation = durable_.evidence_generation.next();
      const Status encoded_status = encode_locked(encoded);
      if (encoded_status.is_failure()) return failure_json(encoded_status);
      const Status stored = persist(encoded);
      if (stored.is_failure()) return failure_json(stored);
      JsonWriter writer;
      writer.begin_object();
      writer.field("status", "OK");
      writer.field("detail", "drain completion recorded");
      writer.field("duplicate", false);
      writer.end_object();
      return ok_json(writer.str(), "drain completion recorded");
    }

    if (plan_id.empty() || !stage_generation.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "completion is incomplete"));
    }
    EvolutionPlan* plan = mutable_plan_locked(plan_id);
    if (plan == nullptr) {
      ++stats_.completions_rejected;
      return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
    }
    if (plan->superseded) {
      ++stats_.completions_rejected;
      return failure_json(
          Status::failure(ErrorCode::StalePlan, "plan is superseded; late completion is rejected"));
    }
    if (stage_generation != plan->stage_generation) {
      ++stats_.completions_rejected;
      return failure_json(
          Status::failure(ErrorCode::StalePlan, "completion targets a superseded stage generation"));
    }
    if (!plan->history.empty() && plan->history.back().stage_id != stage_id) {
      ++stats_.completions_rejected;
      return failure_json(
          Status::failure(ErrorCode::StalePlan, "completion does not reference the current stage identity"));
    }
    PendingAction* action = nullptr;
    for (auto& candidate : pending_actions_) {
      if (candidate.plan != plan_id) continue;
      if (candidate.worker != peer.worker) continue;
      if (candidate.state == ActionState::Acknowledged || candidate.state == ActionState::Abandoned) continue;
      action = &candidate;
      break;
    }
    if (action == nullptr) {
      ++stats_.completions_rejected;
      return failure_json(
          Status::failure(ErrorCode::Conflict, "no outstanding action for this completion; duplicate rejected"));
    }
    const bool completed = migration_outcome == MigrationOutcome::Committed ||
                           migration_outcome == MigrationOutcome::RollbackAvailable ||
                           migration_outcome == MigrationOutcome::Irreversible;
    action->state = ActionState::Acknowledged;
    if (completed) {
      MigrationRecord record;
      record.id = action->migration;
      record.generation = action->migration_generation;
      record.schema = component_state_schema_id();
      record.source = action->source;
      record.target = action->target;
      record.runtime_generation = plan->candidate_generation;
      record.epoch = plan->epoch;
      record.policy = plan->policy.generation;
      record.plan = plan->id;
      record.target_digest = state_digest;
      record.irreversible = migration_outcome == MigrationOutcome::Irreversible;
      record.rollback_metadata_preserved = migration_outcome == MigrationOutcome::RollbackAvailable;
      record.rollback_available = migration_outcome == MigrationOutcome::RollbackAvailable;
      record.outcome = migration_outcome;
      record.provenance = EvidenceClass::Real;
      record.recorded_at = plan->stage_generation;
      record.detail = DetailText::from_valid("migration completion observed");
      if (durable_.migrations.size() >= limits::kMigrationRecords) {
        return failure_json(Status::failure(ErrorCode::LimitExceeded, "migration history bound reached"));
      }
      durable_.migrations.push_back(record);
      if (migration_outcome == MigrationOutcome::Irreversible) {
        plan->rollback_barrier_crossed = true;
      }
      ++stats_.migrations_committed;
    }
    if (!plan->history.empty()) plan->history.back().completed = true;
    ++stats_.completions_accepted;
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("plan", plan->id.view());
    writer.field("migration_outcome", to_string(migration_outcome));
    writer.field("duplicate", false);
    writer.end_object();
    result = ok_json(writer.str(), "completion accepted exactly once");
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

// ---------------------------------------------------------------------------
// Negotiation, state transform
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_negotiate(const PeerIdentity& peer,
                                                      const ProtocolGenerationSet& supported) {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  ProtocolGenerationSet local;
  for (const auto* descriptor : durable_.protocols.all()) {
    if (descriptor->id != peer.protocol) continue;
    (void)local.add(descriptor->generation);
  }
  NegotiationRequest request;
  request.protocol = peer.protocol;
  request.local_supported = local;
  request.peer_supported = supported;
  request.required_minimum = ProtocolGeneration::from_raw(1);
  ProtocolNegotiator negotiator(&durable_.protocols);
  const NegotiationResult negotiation = negotiator.negotiate(request);
  if (!negotiation.is_ok()) {
    CommandResult result = failure_json(negotiation.status);
    result.explanation = negotiation.explanation;
    return result;
  }
  const OperationClass operation =
      derive_operation_class_locked(peer.component, peer.runtime_generation, negotiation.chosen);
  if (negotiation.chosen < peer.agreed_protocol) {
    return failure_json(Status::failure(ErrorCode::Unauthorized,
                                        "protocol downgrade below the session's agreed generation is refused"));
  }
  JsonWriter writer;
  writer.begin_object();
  writer.field("status", "OK");
  writer.field_generation("negotiated_protocol", negotiation.chosen);
  writer.field("operation_class", to_string(operation));
  writer.field("downgraded", negotiation.downgraded);
  writer.key("explanation");
  writer.begin_array();
  for (const auto& line : negotiation.explanation) writer.string(line);
  writer.end_array();
  writer.end_object();
  CommandResult result;
  result.status = Status::ok();
  result.json = writer.str();
  result.explanation = negotiation.explanation;
  return result;
}

CommandResult EvolutionCoordinator::command_state_transform(const PeerIdentity& peer,
                                                            const EvolutionPlanId& plan_id,
                                                            const SchemaGeneration target) {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  const EvolutionPlan* plan = plan_locked(plan_id);
  if (plan == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
  if (plan->superseded) {
    return failure_json(Status::failure(ErrorCode::StalePlan, "superseded plan cannot transform state"));
  }
  const FeatureGate* gate = durable_.gates.find(token_transform_feature_id());
  if (gate == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "feature gate is not defined"));
  GateContext context;
  context.runtime_generation = peer.runtime_generation;
  context.protocol_generation = peer.agreed_protocol;
  context.schema_generation = peer.agreed_schema;
  context.cohort = plan->canary_cohort;
  context.plan = plan->id;
  context.stage = plan->stage_generation;
  const CompatibilityEdge* edge = durable_.matrix.find(plan->current_generation, plan->candidate_generation);
  bool old_writers_live = live_worker_count_locked(plan->component, plan->current_generation) > 0;
  if (edge != nullptr) {
    context.peer_outcome = old_writers_live ? CompatOutcome::MixedVersionCompatible
                                             : edge->aspect(CompatAspect::PeerVersion).outcome;
  }
  const GateDecision decision = durable_.gates.evaluate(token_transform_feature_id(), context);
  if (!decision.permitted) {
    CommandResult result = failure_json(decision.status);
    result.explanation = decision.explanation;
    return result;
  }
  if (target != plan->migration_target && target.is_set()) {
    return failure_json(Status::failure(ErrorCode::StaleGeneration,
                                        "state transformation target is not the plan's migration target"));
  }
  if (peer.agreed_protocol < ProtocolGeneration::from_raw(2)) {
    return failure_json(Status::failure(ErrorCode::Unauthorized,
                                        "state transformation requires the protocol-2 negotiation"));
  }
  JsonWriter writer;
  writer.begin_object();
  writer.field("status", "OK");
  writer.field("plan", plan->id.view());
  writer.field_generation("target_schema", plan->migration_target);
  writer.field_generation("protocol_generation", peer.agreed_protocol);
  writer.end_object();
  CommandResult result;
  result.status = Status::ok();
  result.json = writer.str();
  result.explanation.push_back("state transformation authorized under the negotiated protocol");
  return result;
}

}  // namespace ref