#include "ref/authority.hpp"

#include <algorithm>
#include <array>

namespace ref {
namespace {

constexpr std::array<const char*, kRollbackOutcomeCount> kRollbackOutcomeNames{
    "ROLLBACK_ALLOWED",
    "ROLLBACK_REQUIRES_STATE_RESTORE",
    "ROLLBACK_REQUIRES_PROTOCOL_DOWNGRADE",
    "ROLLBACK_BLOCKED_IRREVERSIBLE_MIGRATION",
    "ROLLBACK_BLOCKED_SCHEMA",
    "ROLLBACK_BLOCKED_PROTOCOL",
    "ROLLBACK_BLOCKED_RETIRED_TARGET",
    "ROLLBACK_REVALIDATION_REQUIRED",
    "ROLLBACK_COMMITTED",
    "ROLLBACK_FAILED"};

constexpr std::array<const char*, kFindingKindCount> kFindingNames{
    "CANDIDATE_EXPECTED_ALIVE_BUT_ABSENT", "OLD_RUNTIME_STILL_ALIVE_AFTER_DRAIN",
    "MIGRATION_MAY_HAVE_COMMITTED",        "FEATURE_GATE_DIFFERS_FROM_EXPECTATION",
    "SCHEMA_GENERATION_ADVANCED_EXTERNALLY", "PROTOCOL_GENERATION_DIFFERS",
    "CANDIDATE_STARTED_OUTSIDE_COORDINATOR", "RETIRED_RUNTIME_RECONNECTED",
    "STALE_EVIDENCE",                      "UNEXPECTED_LIFECYCLE"};

}  // namespace

const char* to_string(RollbackOutcome outcome) noexcept {
  const auto index = static_cast<std::size_t>(outcome);
  return index < kRollbackOutcomeNames.size() ? kRollbackOutcomeNames[index] : "ROLLBACK_FAILED";
}

std::optional<RollbackOutcome> parse_rollback_outcome(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kRollbackOutcomeNames.size(); ++i) {
    if (text == kRollbackOutcomeNames[i]) return static_cast<RollbackOutcome>(i);
  }
  return std::nullopt;
}

const char* to_string(FindingKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kFindingNames.size() ? kFindingNames[index] : "UNEXPECTED_LIFECYCLE";
}

const GenerationPermission* AuthorityView::permission_for(RuntimeGeneration generation) const noexcept {
  for (const auto& entry : coexistence) {
    if (entry.generation == generation) return &entry;
  }
  return nullptr;
}

bool AuthorityView::may_write(RuntimeGeneration generation) const noexcept {
  const GenerationPermission* permission = permission_for(generation);
  return permission != nullptr && permission->may_write;
}

std::string AuthorityView::to_json() const {
  JsonWriter writer;
  writer.begin_object();
  writer.field("component", component.view());
  writer.field("has_authoritative", has_authoritative);
  writer.field("authoritative_generation", authoritative.raw());
  writer.field_generation("protocol_generation", protocol_generation);
  writer.field_generation("schema_generation", schema_generation);
  writer.field("rollout_stage", to_string(stage));
  writer.field("plan", plan.view());
  writer.field("new_writer_enabled", new_writer_enabled);
  writer.field("evidence_current", evidence_current);
  writer.field("rollback_barrier_crossed", rollback_barrier_crossed);
  writer.field_generation("evolution_epoch", epoch);
  writer.field_generation("coordinator_epoch", coordinator_epoch);
  writer.field_generation("snapshot_generation", snapshot_generation);

  writer.key("enabled_features");
  writer.begin_array();
  for (const auto& feature : enabled_features) writer.string(feature.view());
  writer.end_array();

  writer.key("coexistence");
  writer.begin_array();
  for (const auto& entry : coexistence) {
    writer.begin_object();
    writer.field_generation("generation", entry.generation);
    writer.field("lifecycle", ref::to_string(entry.lifecycle));
    writer.field("authoritative", entry.is_authoritative);
    writer.field("may_read", entry.may_read);
    writer.field("may_write", entry.may_write);
    writer.field("operation_class", ref::to_string(entry.operation));
    writer.field_generation("protocol_generation", entry.protocol);
    writer.field_generation("schema_generation", entry.schema);
    writer.field("reason", entry.reason.view());
    writer.end_object();
  }
  writer.end_array();

  writer.key("explanation");
  writer.begin_array();
  for (const auto& line : explanation) writer.string(line);
  writer.end_array();

  writer.end_object();
  return writer.str();
}

RollbackAssessment evaluate_rollback(const RollbackRequest& request, const CompatibilityMatrix& matrix,
                                     const ComponentRegistry& components, const SchemaRegistry& schemas) {
  RollbackAssessment assessment;
  auto reject = [&assessment](RollbackOutcome outcome, ErrorCode code, std::string note) {
    assessment.outcome = outcome;
    assessment.status = Status::failure(code, note);
    assessment.explanation.push_back(std::move(note));
    return assessment;
  };

  if (request.id.empty() || !request.generation.is_set()) {
    return reject(RollbackOutcome::Failed, ErrorCode::InvalidArgument, "rollback request identity is incomplete");
  }
  if (request.component.empty() || !request.current_generation.is_set() ||
      !request.target_generation.is_set()) {
    return reject(RollbackOutcome::Failed, ErrorCode::InvalidArgument,
                  "rollback request does not bind component and generations");
  }
  if (request.current_generation == request.target_generation) {
    return reject(RollbackOutcome::Failed, ErrorCode::InvalidArgument,
                  "rollback target equals the current generation");
  }
  if (!request.epoch.is_set() || !request.coordinator_epoch.is_set()) {
    return reject(RollbackOutcome::Failed, ErrorCode::InvalidArgument,
                  "rollback request does not bind controller epochs");
  }

  if (request.plan_superseded) {
    return reject(RollbackOutcome::RevalidationRequired, ErrorCode::StalePlan,
                  "plan is superseded; rollback must be replanned against current authority");
  }

  const RuntimeComponentVersion* target =
      components.find(request.component, request.target_generation);
  if (target == nullptr) {
    return reject(RollbackOutcome::BlockedRetiredTarget, ErrorCode::RetiredGeneration,
                  "rollback target runtime generation is not registered");
  }
  if (is_terminal_lifecycle(target->lifecycle)) {
    return reject(RollbackOutcome::BlockedRetiredTarget, ErrorCode::RetiredGeneration,
                  "rollback target runtime generation is RETIRED and never regains authority");
  }

  if (request.irreversible_migration_crossed && !request.reverse_migration_available) {
    return reject(RollbackOutcome::BlockedIrreversibleMigration, ErrorCode::Incompatible,
                  "an irreversible state migration has been committed; rollback is not possible");
  }

  if (request.current_schema != request.target_schema) {
    if (!request.schema.empty()) {
      const SchemaCompatResult backward = schemas.read_compatibility(
          request.schema, request.target_schema, request.current_schema);
      if (backward.is_blocking() && !request.reverse_migration_available &&
          !request.state_restore_available) {
        std::string note = "target runtime generation cannot read the current stored schema: ";
        note += to_string(backward.outcome);
        return reject(RollbackOutcome::BlockedSchema, ErrorCode::Incompatible, note);
      }
    }
    if (!request.reverse_migration_available) {
      if (request.state_restore_available) {
        assessment.outcome = RollbackOutcome::RequiresStateRestore;
        assessment.status = Status::ok();
        assessment.explanation.push_back(
            "no reverse migration exists; rollback requires restoring a preserved state checkpoint");
        return assessment;
      }
      return reject(RollbackOutcome::BlockedSchema, ErrorCode::Incompatible,
                    "schema generation moved forward with no reverse migration and no checkpoint");
    }
  }

  if (request.current_protocol != request.target_protocol) {
    const CompatibilityEdge* edge = matrix.find(request.current_generation, request.target_generation);
    if (edge == nullptr) {
      return reject(RollbackOutcome::BlockedProtocol, ErrorCode::Incompatible,
                    "no compatibility edge permits the rollback pair to coexist");
    }
    if (!has_permission(edge->permissions, CompatPermission::CoexistDuringRollout) &&
        !has_permission(edge->permissions, CompatPermission::ControlChannel)) {
      return reject(RollbackOutcome::BlockedProtocol, ErrorCode::Incompatible,
                    "compatibility edge does not permit coexistence during rollback");
    }
    if (edge->requires_protocol_downgrade) {
      assessment.outcome = RollbackOutcome::RequiresProtocolDowngrade;
      assessment.status = Status::ok();
      assessment.explanation.push_back("rollback requires renegotiating protocol generation " +
                                       std::to_string(edge->downgrade_to.raw()));
      return assessment;
    }
  }

  if (!request.target_process_available) {
    return reject(RollbackOutcome::RevalidationRequired, ErrorCode::NotFound,
                  "target runtime process is not available to resume authority");
  }
  if (!request.evidence_current) {
    return reject(RollbackOutcome::RevalidationRequired, ErrorCode::StaleEvidence,
                  "compatibility evidence is stale; revalidate before rolling back");
  }

  assessment.outcome = RollbackOutcome::Allowed;
  assessment.status = Status::ok();
  assessment.explanation.push_back("rollback permitted from generation " +
                                   std::to_string(request.current_generation.raw()) + " to " +
                                   std::to_string(request.target_generation.raw()));
  return assessment;
}

std::string DrainStatus::to_json() const {
  JsonWriter writer;
  writer.begin_object();
  writer.field("component", component.view());
  writer.field_generation("generation", generation);
  writer.field("draining", draining);
  writer.field("live_processes", live_processes);
  writer.field("active_workers", active_workers);
  writer.field("accepts_new_work", accepts_new_work);
  writer.field("accepting_new_state", accepting_new_state);
  writer.field("blocks_retirement", blocks_retirement);
  writer.field("detail", detail.view());
  writer.end_object();
  return writer.str();
}

RetirementAssessment evaluate_retirement(const RetirementPreconditions& preconditions) {
  RetirementAssessment assessment;
  auto reject = [&assessment](std::string note) {
    assessment.allowed = false;
    assessment.status = Status::failure(ErrorCode::Conflict, note);
    assessment.explanation.push_back(std::move(note));
    return assessment;
  };

  if (!preconditions.generation.is_set() || preconditions.component.empty()) {
    return reject("retirement requires a component and runtime generation");
  }
  if (preconditions.superseded) {
    return reject("evolution plan is superseded; retirement must be replanned");
  }
  if (preconditions.live_processes > 0) {
    return reject("live runtime processes remain for the retiring generation");
  }
  if (preconditions.active_workers > 0) {
    return reject("workers of the retiring generation are still active");
  }
  if (preconditions.pending_work) {
    return reject("bounded existing work has not completed for the retiring generation");
  }
  if (!preconditions.incompatible_writers_fenced) {
    return reject("incompatible old writers are not fenced");
  }
  if (!preconditions.migration_complete) {
    return reject("required state migration is not complete");
  }
  if (!preconditions.rollback_policy_allows_retirement) {
    return reject("rollback policy does not permit retiring the rollback target");
  }
  if (!preconditions.checkpoints_retained) {
    return reject("required snapshots/checkpoints have not been retained");
  }
  assessment.allowed = true;
  assessment.status = Status::ok();
  assessment.explanation.push_back("retirement permitted for generation " +
                                   std::to_string(preconditions.generation.raw()));
  return assessment;
}

std::string ReconcileReport::to_json() const {
  JsonWriter writer;
  writer.begin_object();
  writer.field("requires_operator", requires_operator);
  writer.field("finding_count", static_cast<std::uint64_t>(findings.size()));
  writer.key("findings");
  writer.begin_array();
  for (const auto& finding : findings) {
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
    writer.field_generation("boot", finding.boot);
    writer.field("conservative_action_required", finding.conservative_action_required);
    writer.field("detail", finding.detail.view());
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return writer.str();
}

}  // namespace ref
