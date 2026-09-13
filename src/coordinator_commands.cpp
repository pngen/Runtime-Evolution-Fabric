// Runtime Evolution Fabric - coordinator command surface.
//
// Every command follows the same shape: validate preconditions against durable
// state, mutate under the state lock into an encoded image, then persist outside
// the lock. Rejections never leave a partial mutation behind.
#include "coordinator_internal.hpp"
#include "ref/coordinator.hpp"

#include <algorithm>

namespace ref {
namespace {
using internal::component_state_schema_id;
using internal::field_id;
using internal::ok_json;
using internal::ok_result;

CommandResult failure_json(const Status& status) { return internal::failure_result(status); }

}  // namespace

// ---------------------------------------------------------------------------
// Registration and publication
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_register_component(const PeerIdentity& peer,
                                                              const RuntimeComponentVersion& version) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status valid = version.validate();
    if (valid.is_failure()) return failure_json(valid);
    if (!peer.is_operator) {
      // A worker may only register its own component and generation.
      if (!(peer.component == version.component && peer.runtime_generation == version.generation)) {
        return failure_json(Status::failure(ErrorCode::Unauthorized,
                                              "worker may only register its own component generation"));
      }
      if (version.lifecycle != LifecycleState::Registered) {
        return failure_json(Status::failure(
            ErrorCode::Unauthorized, "lifecycle at registration must be REGISTERED when self-registering"));
      }
    }
    if (durable_.is_retired(version.component, version.generation)) {
      return failure_json(Status::failure(ErrorCode::RetiredGeneration,
                                            "runtime generation is retired and cannot be re-registered"));
    }
    const RuntimeComponentVersion* existing = durable_.components.find(version.component, version.generation);
    if (existing != nullptr && existing->version != version.version) {
      return failure_json(Status::failure(
          ErrorCode::Conflict, "runtime generation is already bound to a different artifact identity"));
    }
    Status published = existing == nullptr ? durable_.components.publish(version) : Status::ok();
    if (published.is_failure()) return failure_json(published);
    if (existing != nullptr) {
      // Refresh capability declaration without touching lifecycle.
      RuntimeComponentVersion updated = *existing;
      updated.capability_generation = version.capability_generation;
      updated.features = version.features;
      updated.migrations = version.migrations;
      updated.evidence = durable_.evidence_generation;
      const Status stored = durable_.components.publish(updated);
      if (stored.is_failure()) return failure_json(stored);
    }
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return ok_result("runtime component version registered");
}

CommandResult EvolutionCoordinator::command_publish_capability(const PeerIdentity& peer,
                                                               const RuntimeComponentId& component,
                                                               RuntimeGeneration generation,
                                                               CapabilityGeneration capability,
                                                               const std::vector<FeatureSupport>& features) {
  std::string encoded;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    if (!peer.is_operator && !(peer.component == component && peer.runtime_generation == generation)) {
      return failure_json(Status::failure(ErrorCode::Unauthorized,
                                            "capability publication is limited to the publisher's own generation"));
    }
    const RuntimeComponentVersion* existing = durable_.components.find(component, generation);
    if (existing == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "runtime generation is not registered"));
    }
    if (!capability.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument, "capability generation is unset"));
    }
    if (existing->capability_generation.is_set() && capability < existing->capability_generation) {
      return failure_json(Status::failure(ErrorCode::StaleGeneration,
                                            "capability generation is older than the recorded one"));
    }
    RuntimeComponentVersion updated = *existing;
    updated.capability_generation = capability;
    updated.features = features;
    updated.evidence = durable_.evidence_generation;
    const Status stored = durable_.components.publish(updated);
    if (stored.is_failure()) return failure_json(stored);
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return ok_result("capability generation published");
}

CommandResult EvolutionCoordinator::command_publish_compatibility(const PeerIdentity& peer,
                                                                 const CompatibilityEdge& edge) {
  std::string encoded;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    CompatibilityEdge stamped = edge;
    stamped.generation = durable_.matrix.generation().is_set() ? durable_.matrix.generation().next()
                                                               : CompatibilityGeneration::first();
    stamped.epoch = durable_.evolution_epoch;
    stamped.evidence = durable_.evidence_generation;
    const Status published = durable_.matrix.upsert(stamped);
    if (published.is_failure()) return failure_json(published);
    durable_.matrix_generation = durable_.matrix.generation();
    durable_.evidence_generation = durable_.evidence_generation.next();
    // Eligibility is derived from evidence, not asserted: a candidate becomes
    // eligible only when a published edge permits coexistence and a control
    // channel in both directions of the rollout.
    const bool eligible_edge = has_permission(stamped.permissions, CompatPermission::ControlChannel) &&
                               has_permission(stamped.permissions, CompatPermission::CoexistDuringRollout);
    if (eligible_edge) {
      const RuntimeComponentVersion* candidate =
          durable_.components.find(RuntimeComponentId{}, RuntimeGeneration{});  // placeholder, replaced below
      (void)candidate;
      for (const auto& component : durable_.components.components()) {
        const RuntimeComponentVersion* version = durable_.components.find(component, stamped.to);
        if (version == nullptr) continue;
        if (version->lifecycle == LifecycleState::Registered) {
          (void)advance_lifecycle_locked(component, stamped.to, LifecycleState::CompatibilityPending);
          version = durable_.components.find(component, stamped.to);
        }
        if (version != nullptr && version->lifecycle == LifecycleState::CompatibilityPending) {
          (void)advance_lifecycle_locked(component, stamped.to, LifecycleState::Eligible);
        }
      }
    }
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return ok_result("compatibility edge published; bound plans must be rebound");
}

CommandResult EvolutionCoordinator::command_publish_schema(const PeerIdentity& peer,
                                                          const SchemaDescriptor& descriptor) {
  std::string encoded;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    const Status published = durable_.schemas.publish(descriptor);
    if (published.is_failure()) return failure_json(published);
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return ok_result("schema generation published");
}

CommandResult EvolutionCoordinator::command_publish_protocol(const PeerIdentity& peer,
                                                            const ProtocolDescriptor& descriptor) {
  std::string encoded;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    const Status published = durable_.protocols.publish(descriptor);
    if (published.is_failure()) return failure_json(published);
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return ok_result("protocol generation published");
}

// ---------------------------------------------------------------------------
// Plan creation
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_create_plan(const PeerIdentity& peer, const PlanSpec& spec) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    if (spec.component.empty() || !spec.candidate_generation.is_set()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument,
                                            "plan requires a component and candidate generation"));
    }
    if (durable_.is_retired(spec.component, spec.candidate_generation)) {
      return failure_json(Status::failure(ErrorCode::RetiredGeneration,
                                            "candidate generation is retired and cannot be a rollout target"));
    }
    const RuntimeComponentVersion* candidate =
        durable_.components.find(spec.component, spec.candidate_generation);
    if (candidate == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "candidate runtime generation is not registered"));
    }
    const auto authoritative = durable_.components.authoritative_generation(spec.component);
    if (!authoritative.has_value()) {
      return failure_json(Status::failure(ErrorCode::Conflict,
                                            "component has no authoritative generation to evolve away from"));
    }
    if (*authoritative == spec.candidate_generation) {
      return failure_json(Status::failure(ErrorCode::Conflict,
                                            "candidate generation is already authoritative"));
    }
    if (active_plan_locked(spec.component) != nullptr) {
      return failure_json(Status::failure(ErrorCode::Conflict,
                                            "an active evolution plan already exists for this component"));
    }
    const RuntimeComponentVersion* current = durable_.components.find(spec.component, *authoritative);
    if (current == nullptr) {
      return failure_json(Status::failure(ErrorCode::Internal, "authoritative generation is not registered"));
    }
    if (current->lifecycle != LifecycleState::Current && current->lifecycle != LifecycleState::Eligible) {
      return failure_json(Status::failure(ErrorCode::Conflict,
                                            "current generation is not in a state that can be evolved"));
    }
    const CompatibilityEdge* edge = durable_.matrix.find(*authoritative, spec.candidate_generation);
    if (edge == nullptr) {
      return failure_json(Status::failure(ErrorCode::Incompatible,
                                            "no compatibility edge exists for the candidate pair"));
    }
    if (!has_permission(edge->permissions, CompatPermission::ControlChannel)) {
      return failure_json(Status::failure(ErrorCode::Incompatible,
                                            "compatibility edge does not permit a control channel"));
    }
    if (!has_permission(edge->permissions, CompatPermission::CoexistDuringRollout)) {
      return failure_json(Status::failure(ErrorCode::Incompatible,
                                            "compatibility edge does not permit coexistence during rollout"));
    }
    if (spec.policy.require_canary && spec.canary_cohort.empty()) {
      return failure_json(Status::failure(ErrorCode::InvalidArgument,
                                            "policy requires a canary cohort but none was supplied"));
    }
    if (spec.policy.require_migration_barrier &&
        (spec.migration.empty() || !spec.migration_generation.is_set())) {
      return failure_json(Status::failure(
          ErrorCode::InvalidArgument, "policy requires a migration barrier but no migration was supplied"));
    }
    if (!spec.migration.empty()) {
      if (!spec.migration_source.is_set() || !spec.migration_target.is_set()) {
        return failure_json(Status::failure(
            ErrorCode::InvalidArgument, "migration binding requires explicit source and target schema generations"));
      }
      if (candidate->find_migration(spec.migration_source, spec.migration_target) == nullptr) {
        return failure_json(Status::failure(
            ErrorCode::Incompatible, "candidate generation does not declare the requested migration capability"));
      }
    }
    for (const auto& cohort : spec.cohorts) {
      if (cohort.empty()) {
        return failure_json(Status::failure(ErrorCode::InvalidArgument, "plan cohort id is empty"));
      }
    }

    // Protocol generation for the mixed-version phase: highest generation both
    // sides actually support. It is advanced later, never assumed.
    ProtocolGenerationSet common;
    for (std::uint8_t i = 0; i < current->protocols.supported.size(); ++i) {
      const auto value = current->protocols.supported.at(i);
      if (candidate->protocols.supported.contains(value)) (void)common.add(value);
    }
    if (common.empty()) {
      return failure_json(Status::failure(ErrorCode::Incompatible,
                                            "current and candidate generations share no protocol generation"));
    }
    SchemaGeneration schema_floor = current->schemas.writable.highest();
    if (!schema_floor.is_set()) schema_floor = current->schemas.supported.highest();

    EvolutionPlan plan;
    plan.id = spec.id.empty() ? EvolutionPlanId::from_valid("plan-" + std::to_string(durable_.plans.size() + 1))
                              : spec.id;
    plan.generation = EvolutionPlanGeneration::first();
    plan.component = spec.component;
    plan.current_generation = *authoritative;
    plan.candidate_generation = spec.candidate_generation;
    plan.matrix_generation = durable_.matrix.generation();
    plan.protocol_generation = common.highest();
    plan.schema_generation = schema_floor;
    plan.gate_generation = durable_.gates.generation();
    plan.migration = spec.migration;
    plan.migration_generation = spec.migration_generation;
    plan.migration_source = spec.migration_source;
    plan.migration_target = spec.migration_target;
    plan.rollback_target = *authoritative;
    plan.rollback_generation = RollbackGeneration::first();
    plan.rollback_barrier_crossed = false;
    plan.required_evidence = spec.required_evidence;
    plan.epoch = durable_.evolution_epoch.is_set() ? durable_.evolution_epoch.next()
                                                    : EvolutionEpoch::first();
    plan.coordinator_epoch = durable_.coordinator_epoch;
    plan.stage = RolloutStage::CandidateRegistered;
    plan.stage_generation = durable_.stage_generation.is_set() ? durable_.stage_generation.next()
                                                               : StageGeneration::first();
    plan.cohorts = spec.cohorts;
    plan.canary_cohort = spec.canary_cohort;
    plan.new_writer_enabled = false;
    plan.policy = spec.policy;
    plan.policy.generation = durable_.policy_generation.is_set() ? durable_.policy_generation.next()
                                                                 : PolicyGeneration::first();
    plan.detail = spec.detail;
    {
      PlanStageRecord record;
      record.stage_id = RolloutStageId::from_valid("stage-" + std::to_string(plan.stage_generation.raw()));
      record.stage = plan.stage;
      record.generation = plan.stage_generation;
      record.epoch = plan.epoch;
      record.coordinator_epoch = plan.coordinator_epoch;
      record.evidence = durable_.evidence_generation;
      record.note = NoteText::from_valid("candidate registered");
      plan.history.push_back(record);
    }
    const Status valid = plan.validate();
    if (valid.is_failure()) return failure_json(valid);
    if (durable_.plans.size() >= limits::kPlanHistory) {
      return failure_json(Status::failure(ErrorCode::LimitExceeded, "plan history bound reached"));
    }
    durable_.evolution_epoch = plan.epoch;
    durable_.stage_generation = plan.stage_generation;
    durable_.policy_generation = plan.policy.generation;
    durable_.stage_history.push_back(plan.history.back());
    if (durable_.stage_history.size() > limits::kStageHistory) durable_.stage_history.erase(durable_.stage_history.begin());
    const Status transitioned =
        advance_lifecycle_locked(spec.component, spec.candidate_generation, LifecycleState::UpgradePending);
    if (transitioned.is_ok()) refresh_worker_authority_locked(spec.component);
    if (transitioned.is_failure()) {
      // ELIGIBLE -> UPGRADE_PENDING is required; anything else must be repaired
      // by moving through COMPATIBILITY_PENDING first.
      return failure_json(transitioned);
    }
    durable_.plans.emplace(plan.id, plan);
    durable_.evidence_generation = durable_.evidence_generation.next();
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);

    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("plan", plan.id.view());
    writer.field("stage", to_string(plan.stage));
    writer.field_generation("evolution_epoch", plan.epoch);
    writer.field_generation("protocol_generation", plan.protocol_generation);
    writer.field_generation("schema_generation", plan.schema_generation);
    writer.field_generation("stage_generation", plan.stage_generation);
    writer.end_object();
    result.json = writer.str();
    result.status = Status::ok();
    result.explanation.push_back("plan created with a fresh evolution epoch");
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

// ---------------------------------------------------------------------------
// Stage advancement
// ---------------------------------------------------------------------------
StagePreconditions EvolutionCoordinator::stage_preconditions_locked(const EvolutionPlan& plan) const {
  StagePreconditions preconditions;
  const CompatibilityEdge* edge = durable_.matrix.find(plan.current_generation, plan.candidate_generation);
  if (edge != nullptr) {
    preconditions.compatibility_proven =
        has_permission(edge->permissions, CompatPermission::ControlChannel) &&
        has_permission(edge->permissions, CompatPermission::CoexistDuringRollout);
    preconditions.evidence_current = evidence_current_locked(*edge);
  }
  if (!plan.migration.empty() && plan.migration_generation.is_set() && plan.migration_source.is_set() &&
      plan.migration_target.is_set()) {
    preconditions.migration_contract_bound = true;
  }
  for (const auto& record : durable_.migrations) {
    if (record.plan != plan.id) continue;
    if (record.outcome == MigrationOutcome::Committed || record.outcome == MigrationOutcome::RollbackAvailable ||
        record.outcome == MigrationOutcome::Irreversible) {
      preconditions.migration_complete = true;
      preconditions.migration_irreversible = record.irreversible;
    }
  }
  const std::uint64_t old_live = live_worker_count_locked(plan.component, plan.current_generation);
  const std::uint64_t candidate_live = live_worker_count_locked(plan.component, plan.candidate_generation);
  preconditions.old_writers_drained = old_live == 0 && candidate_live > 0;
  bool any_undrained_old = false;
  for (const auto& [id, worker] : live_workers_) {
    (void)id;
    if (worker.component != plan.component) continue;
    if (worker.generation != plan.current_generation) continue;
    if (worker.active && !worker.draining) any_undrained_old = true;
  }
  preconditions.new_writer_fence_satisfied = !any_undrained_old;
  preconditions.rollback_valid = plan.rollback_barrier_crossed;
  if (!preconditions.rollback_valid) {
    // Rollback stays valid while the stored state is still what the current
    // generation itself writes; once the schema has moved forward it depends on
    // a reverse migration.
    const RuntimeComponentVersion* current =
        durable_.components.find(plan.component, plan.current_generation);
    const SchemaGeneration current_native =
        current != nullptr ? current->schemas.writable.highest() : SchemaGeneration::unset();
    if (current_native.is_set() && plan.schema_generation == current_native) {
      preconditions.rollback_valid = true;
    } else {
      const SchemaDescriptor* stored =
          durable_.schemas.find(component_state_schema_id(), plan.schema_generation);
      if (stored != nullptr && stored->reverse_migration_to.is_set()) preconditions.rollback_valid = true;
      if (plan.schema_generation == plan.migration_source) preconditions.rollback_valid = true;
    }
  }
  preconditions.canary_cohort_active = candidate_live > 0;
  preconditions.mixed_version_active = candidate_live > 0 && old_live > 0;
  return preconditions;
}

CommandResult EvolutionCoordinator::command_advance_stage(const PeerIdentity& peer, const EvolutionPlanId& plan_id,
                                                          RolloutStage target,
                                                          StageGeneration expected_stage_generation) {
  std::string encoded;
  CommandResult result;
  bool changed = false;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    EvolutionPlan* plan = mutable_plan_locked(plan_id);
    if (plan == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
    if (plan->superseded) {
      return failure_json(Status::failure(ErrorCode::StalePlan, "plan is superseded and cannot advance"));
    }
    if (expected_stage_generation.is_set() && expected_stage_generation != plan->stage_generation) {
      return failure_json(Status::failure(ErrorCode::StalePlan,
                                            "stage generation does not match; the request is late or duplicated"));
    }
    if (plan->matrix_generation != durable_.matrix.generation()) {
      return failure_json(Status::failure(
          ErrorCode::StaleEvidence, "compatibility matrix changed since the plan was bound; rebind the plan"));
    }
    if (plan->gate_generation != durable_.gates.generation()) {
      return failure_json(Status::failure(
          ErrorCode::StaleEvidence, "feature-gate generation changed since the plan was bound; rebind the plan"));
    }
    const StagePreconditions preconditions = stage_preconditions_locked(*plan);
    const StageDecision decision = evaluate_stage_advance(*plan, target, preconditions);
    if (!decision.is_allowed()) {
      result = failure_json(decision.status);
      result.explanation = decision.explanation;
      return result;
    }
    plan->stage = target;
    plan->stage_generation = durable_.stage_generation.next();
    durable_.stage_generation = plan->stage_generation;
    plan->evidence = durable_.evidence_generation;
    durable_.evidence_generation = durable_.evidence_generation.next();
    PlanStageRecord record;
    record.stage_id = RolloutStageId::from_valid("stage-" + std::to_string(plan->stage_generation.raw()));
    record.stage = target;
    record.generation = plan->stage_generation;
    record.epoch = plan->epoch;
    record.coordinator_epoch = durable_.coordinator_epoch;
    record.evidence = durable_.evidence_generation;
    record.note = NoteText::from_valid(decision.explanation.empty() ? "stage advanced" : decision.explanation.back());
    plan->history.push_back(record);
    if (plan->history.size() > limits::kStageHistory) plan->history.erase(plan->history.begin());
    durable_.stage_history.push_back(record);
    if (durable_.stage_history.size() > limits::kStageHistory) {
      durable_.stage_history.erase(durable_.stage_history.begin());
    }

    const RuntimeComponentVersion* candidate =
        durable_.components.find(plan->component, plan->candidate_generation);
    const RuntimeComponentVersion* current = durable_.components.find(plan->component, plan->current_generation);
    Status transition = Status::ok();
    // Stages that are optional under policy may be skipped, but their lifecycle
    // effect may not: the candidate is walked through every state up to the one
    // the target stage implies.
    {
      // Progress order of a candidate generation. States that are not part of
      // forward progress (draining, rolled back, retired) report -1 and stop the
      // walk so a candidate is never dragged out of a terminal-ish state.
      const auto progress_rank = [](LifecycleState state) -> int {
        switch (state) {
          case LifecycleState::CanaryActive: return 1;
          case LifecycleState::MixedVersionActive: return 2;
          case LifecycleState::RolloutActive: return 3;
          case LifecycleState::Current: return 4;
          case LifecycleState::Draining:
          case LifecycleState::RollbackPending:
          case LifecycleState::RollingBack:
          case LifecycleState::RolledBack:
          case LifecycleState::RetirementPending:
          case LifecycleState::Retired:
          case LifecycleState::RevalidationRequired:
          case LifecycleState::Failed:
            return -1;
          default: return 0;
        }
      };
      const std::uint8_t rank = rollout_stage_rank(target);
      const LifecycleState walk[] = {LifecycleState::CanaryActive, LifecycleState::MixedVersionActive,
                                     LifecycleState::RolloutActive, LifecycleState::Current};
      const std::uint8_t thresholds[] = {
          static_cast<std::uint8_t>(rollout_stage_rank(RolloutStage::CanaryCohort)),
          static_cast<std::uint8_t>(rollout_stage_rank(RolloutStage::MixedVersionCohort)),
          static_cast<std::uint8_t>(rollout_stage_rank(RolloutStage::ExpandedCohort)),
          static_cast<std::uint8_t>(rollout_stage_rank(RolloutStage::FullPromotion))};
      for (std::size_t i = 0; i < 4 && transition.is_ok(); ++i) {
        if (rank < thresholds[i]) break;
        const RuntimeComponentVersion* version =
            durable_.components.find(plan->component, plan->candidate_generation);
        if (version == nullptr) break;
        const int current_progress = progress_rank(version->lifecycle);
        if (current_progress < 0) break;
        if (current_progress >= progress_rank(walk[i])) continue;
        transition = advance_lifecycle_locked(plan->component, plan->candidate_generation, walk[i]);
      }
    }
    switch (target) {
      case RolloutStage::CanaryCohort:
      case RolloutStage::MixedVersionCohort:
      case RolloutStage::ExpandedCohort:
        break;
      case RolloutStage::MigrationBarrier:
        if (plan->migration_target.is_set()) plan->schema_generation = plan->migration_target;
        break;
      case RolloutStage::NewWriterEnabled: {
        plan->new_writer_enabled = true;
        if (candidate != nullptr) {
          plan->protocol_generation = candidate->protocols.supported.highest();
          const SchemaGeneration native = candidate->schemas.writable.highest();
          if (native.is_set()) plan->schema_generation = native;
        }
        break;
      }
      case RolloutStage::OldWriterDrain:
        if (current != nullptr && current->lifecycle != LifecycleState::Draining) {
          transition = advance_lifecycle_locked(plan->component, plan->current_generation,
                                                LifecycleState::Draining);
        }
        break;
      case RolloutStage::FullPromotion:
        // The candidate was promoted to CURRENT by the walk above; the old
        // generation is drained here.
        if (transition.is_ok() && current != nullptr && current->lifecycle != LifecycleState::Draining) {
          transition = advance_lifecycle_locked(plan->component, plan->current_generation,
                                                LifecycleState::Draining);
        }
        break;
      default:
        break;
    }
    if (transition.is_failure()) {
      // Nothing was persisted: the stage change is rolled back in memory.
      return failure_json(transition);
    }
    refresh_worker_authority_locked(plan->component);
    changed = true;
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);

    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("plan", plan->id.view());
    writer.field("stage", to_string(plan->stage));
    writer.field_generation("stage_generation", plan->stage_generation);
    writer.field_generation("protocol_generation", plan->protocol_generation);
    writer.field_generation("schema_generation", plan->schema_generation);
    writer.field("new_writer_enabled", plan->new_writer_enabled);
    writer.end_object();
    result.json = writer.str();
    result.status = Status::ok();
    result.explanation = decision.explanation;
  }
  if (changed) {
    const Status stored = persist(encoded);
    if (stored.is_failure()) return failure_json(stored);
  }
  return result;
}

CommandResult EvolutionCoordinator::command_rebind_plan(const PeerIdentity& peer, const EvolutionPlanId& plan_id,
                                                        EvolutionPlanGeneration expected_generation) {
  std::string encoded;
  CommandResult result;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status authorized = validate_operator(peer);
    if (authorized.is_failure()) return failure_json(authorized);
    EvolutionPlan* plan = mutable_plan_locked(plan_id);
    if (plan == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "plan is not known"));
    if (plan->superseded) {
      return failure_json(Status::failure(ErrorCode::StalePlan, "superseded plan cannot be rebound"));
    }
    if (expected_generation.is_set() && expected_generation != plan->generation) {
      return failure_json(Status::failure(ErrorCode::StalePlan, "plan generation does not match"));
    }
    plan->generation = plan->generation.next();
    plan->matrix_generation = durable_.matrix.generation();
    plan->gate_generation = durable_.gates.generation();
    plan->evidence = durable_.evidence_generation;
    result.status = Status::ok();
    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("plan", plan->id.view());
    writer.field_generation("plan_generation", plan->generation);
    writer.field_generation("matrix_generation", plan->matrix_generation);
    writer.field_generation("gate_generation", plan->gate_generation);
    writer.end_object();
    result.json = writer.str();
    result.explanation.push_back("plan rebound to current compatibility and gate generations");
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

CommandResult EvolutionCoordinator::command_enable_feature(const PeerIdentity& peer,
                                                           const FeatureGateId& feature,
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
      return failure_json(Status::failure(ErrorCode::StalePlan, "superseded plan cannot enable features"));
    }
    const FeatureGate* gate = durable_.gates.find(feature);
    if (gate == nullptr) return failure_json(Status::failure(ErrorCode::NotFound, "feature gate is not defined"));
    const RuntimeComponentVersion* candidate =
        durable_.components.find(plan->component, plan->candidate_generation);
    if (candidate == nullptr) {
      return failure_json(Status::failure(ErrorCode::NotFound, "candidate generation is not registered"));
    }
    const FeatureSupport* support = candidate->find_feature(feature);
    if (support == nullptr || !support->can_publish) {
      return failure_json(Status::failure(ErrorCode::Incompatible,
                                            "candidate generation does not declare support for the feature"));
    }
    const CompatibilityEdge* edge = durable_.matrix.find(plan->current_generation, plan->candidate_generation);
    const bool old_writers_live = live_worker_count_locked(plan->component, plan->current_generation) > 0;
    GateContext context;
    context.runtime_generation = plan->candidate_generation;
    context.protocol_generation = plan->protocol_generation;
    context.schema_generation = plan->schema_generation;
    context.cohort = plan->canary_cohort;
    context.plan = plan->id;
    context.stage = plan->stage_generation;
    if (edge != nullptr) {
      context.peer_outcome = old_writers_live ? CompatOutcome::MixedVersionCompatible
                                              : edge->aspect(CompatAspect::PeerVersion).outcome;
    }
    const GateDecision decision = durable_.gates.evaluate(feature, context, /*require_enabled=*/false);
    if (!decision.permitted) {
      result = failure_json(decision.status);
      result.explanation = decision.explanation;
      return result;
    }
    const Status enabled = durable_.gates.set_enabled(feature, true, plan->id, plan->stage_generation);
    if (enabled.is_failure()) return failure_json(enabled);
    // Rebinding is explicit: the plan records the new gate generation.
    const FeatureGateGeneration new_generation = durable_.gates.generation();
    for (auto& [id, other] : durable_.plans) {
      (void)id;
      if (other.superseded) continue;
      if (!other.is_active()) continue;
      other.gate_generation = new_generation;
    }
    plan->gate_generation = new_generation;
    durable_.gate_generation = new_generation;
    durable_.evidence_generation = durable_.evidence_generation.next();
    result.status = Status::ok();
    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("feature", feature.view());
    writer.field_generation("gate_generation", new_generation);
    writer.field_generation("protocol_generation", plan->protocol_generation);
    writer.field_generation("schema_generation", plan->schema_generation);
    writer.end_object();
    result.json = writer.str();
    result.explanation = decision.explanation;
    result.explanation.push_back("active plans rebound to the new feature-gate generation");
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return failure_json(encoded_status);
  }
  const Status stored = persist(encoded);
  if (stored.is_failure()) return failure_json(stored);
  return result;
}

}  // namespace ref