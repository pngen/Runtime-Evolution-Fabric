#include "ref/plan.hpp"

#include <algorithm>
#include <array>

namespace ref {
namespace {

constexpr std::array<const char*, kRolloutStageCount> kStageNames{
    "NONE",              "CANDIDATE_REGISTERED",      "COMPATIBILITY_PROVEN",
    "CANARY_COHORT",     "MIXED_VERSION_COHORT",      "EXPANDED_COHORT",
    "MIGRATION_BARRIER", "NEW_WRITER_ENABLED",        "OLD_WRITER_DRAIN",
    "FULL_PROMOTION",    "OLD_GENERATION_RETIREMENT", "SUPERSEDED",
    "ROLLED_BACK"};

}  // namespace

const char* to_string(RolloutStage stage) noexcept {
  const auto index = static_cast<std::size_t>(stage);
  return index < kStageNames.size() ? kStageNames[index] : "NONE";
}

std::optional<RolloutStage> parse_rollout_stage(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kStageNames.size(); ++i) {
    if (text == kStageNames[i]) return static_cast<RolloutStage>(i);
  }
  return std::nullopt;
}

std::uint8_t rollout_stage_rank(RolloutStage stage) noexcept {
  switch (stage) {
    case RolloutStage::None: return 0;
    case RolloutStage::CandidateRegistered: return 1;
    case RolloutStage::CompatibilityProven: return 2;
    case RolloutStage::CanaryCohort: return 3;
    case RolloutStage::MixedVersionCohort: return 4;
    case RolloutStage::ExpandedCohort: return 5;
    case RolloutStage::MigrationBarrier: return 6;
    case RolloutStage::NewWriterEnabled: return 7;
    case RolloutStage::OldWriterDrain: return 8;
    case RolloutStage::FullPromotion: return 9;
    case RolloutStage::OldGenerationRetirement: return 10;
    case RolloutStage::Superseded:
    case RolloutStage::RolledBack: return 0;
  }
  return 0;
}

bool is_terminal_stage(RolloutStage stage) noexcept {
  switch (stage) {
    case RolloutStage::Superseded:
    case RolloutStage::RolledBack:
    case RolloutStage::OldGenerationRetirement:
    case RolloutStage::FullPromotion:
      return true;
    default:
      return false;
  }
}

bool is_authority_barrier_stage(RolloutStage stage) noexcept {
  switch (stage) {
    case RolloutStage::MigrationBarrier:
    case RolloutStage::NewWriterEnabled:
    case RolloutStage::OldWriterDrain:
    case RolloutStage::FullPromotion:
    case RolloutStage::OldGenerationRetirement:
      return true;
    default:
      return false;
  }
}

Status EvolutionPlan::validate() const noexcept {
  if (id.empty()) return Status::failure(ErrorCode::InvalidArgument, "plan id is empty");
  if (!generation.is_set()) return Status::failure(ErrorCode::InvalidArgument, "plan generation is unset");
  if (component.empty()) return Status::failure(ErrorCode::InvalidArgument, "plan component is empty");
  if (!current_generation.is_set() || !candidate_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "plan must bind current and candidate generations");
  }
  if (current_generation == candidate_generation) {
    return Status::failure(ErrorCode::InvalidArgument, "plan current and candidate generations are equal");
  }
  if (!matrix_generation.is_set() || !protocol_generation.is_set() || !schema_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "plan must bind compatibility, protocol and schema generations");
  }
  if (!gate_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "plan must bind a feature-gate generation");
  }
  if (!epoch.is_set()) return Status::failure(ErrorCode::InvalidArgument, "plan must bind an evolution epoch");
  if (!coordinator_epoch.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "plan must bind a coordinator epoch");
  }
  if (rollback_barrier_crossed && !rollback_target.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "plan crossed a rollback barrier without a target");
  }
  if (history.size() > limits::kStageHistory) {
    return Status::failure(ErrorCode::LimitExceeded, "plan stage history exceeds the bound");
  }
  if (cohorts.size() > limits::kCohorts) {
    return Status::failure(ErrorCode::LimitExceeded, "plan declares too many cohorts");
  }
  if (policy.require_canary && canary_cohort.empty() && stage >= RolloutStage::CanaryCohort &&
      stage != RolloutStage::Superseded && stage != RolloutStage::RolledBack) {
    return Status::failure(ErrorCode::InvalidArgument, "plan requires a canary cohort but declares none");
  }
  if (policy.require_migration_barrier && !migration_generation.is_set() &&
      rollout_stage_rank(stage) >= rollout_stage_rank(RolloutStage::MigrationBarrier)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "plan requires a migration barrier but binds no migration generation");
  }
  return Status::ok();
}

bool EvolutionPlan::has_cohort(const CohortId& cohort) const noexcept {
  if (cohort.empty()) return false;
  for (const auto& entry : cohorts) {
    if (entry == cohort) return true;
  }
  return false;
}

StageDecision evaluate_stage_advance(const EvolutionPlan& plan, RolloutStage target,
                                     const StagePreconditions& preconditions) {
  StageDecision decision;
  auto reject = [&decision](ErrorCode code, std::string note) {
    decision.allowed = false;
    decision.status = Status::failure(code, note);
    decision.explanation.push_back(std::move(note));
    return decision;
  };

  if (plan.superseded) {
    return reject(ErrorCode::StalePlan, "plan is superseded and cannot advance stages");
  }
  if (is_terminal_stage(plan.stage)) {
    return reject(ErrorCode::Conflict, "plan has already reached a terminal rollout stage");
  }
  if (target == RolloutStage::None) {
    return reject(ErrorCode::InvalidArgument, "requested stage is not a real rollout stage");
  }
  if (target == RolloutStage::Superseded || target == RolloutStage::RolledBack) {
    return reject(ErrorCode::InvalidArgument, "side states are not reachable through stage advance");
  }
  const std::uint8_t current_rank = rollout_stage_rank(plan.stage);
  const std::uint8_t target_rank = rollout_stage_rank(target);
  if (target_rank <= current_rank) {
    return reject(ErrorCode::Conflict, "requested stage is not ahead of the current stage");
  }

  // Skipped stages must be optional under the plan's policy.
  for (std::uint8_t rank = static_cast<std::uint8_t>(current_rank + 1); rank < target_rank; ++rank) {
    RolloutStage skipped = RolloutStage::None;
    for (std::uint8_t candidate = 0; candidate < kRolloutStageCount; ++candidate) {
      const auto stage = static_cast<RolloutStage>(candidate);
      if (rollout_stage_rank(stage) == rank) {
        skipped = stage;
        break;
      }
    }
    const bool required = (skipped == RolloutStage::CanaryCohort && plan.policy.require_canary) ||
                          (skipped == RolloutStage::MixedVersionCohort && plan.policy.require_mixed_version_cohort) ||
                          (skipped == RolloutStage::MigrationBarrier && plan.policy.require_migration_barrier) ||
                          (skipped == RolloutStage::NewWriterEnabled && plan.policy.require_new_writer_barrier) ||
                          (skipped == RolloutStage::OldWriterDrain && plan.policy.require_old_writer_drain);
    if (required) {
      std::string note = "stage ";
      note += to_string(skipped);
      note += " is required by policy and may not be skipped";
      return reject(ErrorCode::Conflict, note);
    }
    decision.explanation.push_back(std::string("stage ") + to_string(skipped) +
                                   " is optional under policy and is skipped");
  }

  switch (target) {
    case RolloutStage::CandidateRegistered:
      break;
    case RolloutStage::CompatibilityProven:
      if (!preconditions.compatibility_proven) {
        return reject(ErrorCode::Incompatible, "compatibility evidence is not proven for the candidate pair");
      }
      if (!preconditions.evidence_current) {
        return reject(ErrorCode::StaleEvidence, "compatibility evidence is stale");
      }
      break;
    case RolloutStage::CanaryCohort:
      if (!preconditions.compatibility_proven) {
        return reject(ErrorCode::Incompatible, "canary requires proven compatibility");
      }
      if (plan.canary_cohort.empty()) {
        return reject(ErrorCode::InvalidArgument, "canary stage requires a canary cohort");
      }
      break;
    case RolloutStage::MixedVersionCohort:
      if (!preconditions.compatibility_proven) {
        return reject(ErrorCode::Incompatible, "mixed-version stage requires proven compatibility");
      }
      break;
    case RolloutStage::ExpandedCohort:
      if (!preconditions.mixed_version_active) {
        return reject(ErrorCode::Conflict, "expanded cohort requires an observed mixed-version phase");
      }
      break;
    case RolloutStage::MigrationBarrier:
      if (!preconditions.migration_contract_bound && !preconditions.migration_complete) {
        return reject(ErrorCode::Conflict,
                      "migration barrier requires a bound migration contract and a validated migration plan");
      }
      break;
    case RolloutStage::NewWriterEnabled:
      if (plan.policy.require_migration_barrier && !preconditions.migration_complete) {
        return reject(ErrorCode::Conflict, "new-writer enablement requires the migration barrier to be crossed");
      }
      if (!preconditions.new_writer_fence_satisfied) {
        return reject(ErrorCode::Unauthorized,
                      "new-writer enablement requires old incompatible writers drained or fenced");
      }
      if (plan.policy.require_rollback_proof && !preconditions.rollback_valid) {
        return reject(ErrorCode::Incompatible,
                      "new-writer enablement requires a valid rollback path or explicit barrier record");
      }
      break;
    case RolloutStage::OldWriterDrain:
      if (!preconditions.rollback_valid && !plan.rollback_barrier_crossed) {
        return reject(ErrorCode::Incompatible, "drain requires rollback validity or a recorded barrier");
      }
      break;
    case RolloutStage::FullPromotion:
      if (!preconditions.old_writers_drained) {
        return reject(ErrorCode::Conflict, "full promotion requires drained old writers");
      }
      if (!preconditions.evidence_current) {
        return reject(ErrorCode::StaleEvidence, "full promotion requires current compatibility evidence");
      }
      break;
    case RolloutStage::OldGenerationRetirement:
      if (!preconditions.old_writers_drained) {
        return reject(ErrorCode::Conflict, "retirement requires drained old generations");
      }
      break;
    case RolloutStage::None:
    case RolloutStage::Superseded:
    case RolloutStage::RolledBack:
      return reject(ErrorCode::InvalidArgument, "requested stage is not advanceable");
  }

  decision.allowed = true;
  decision.status = Status::ok();
  decision.explanation.push_back(std::string("stage advance ") + to_string(plan.stage) + " -> " +
                                 to_string(target) + " permitted");
  return decision;
}

// ---------------------------------------------------------------------------
// Feature gates
// ---------------------------------------------------------------------------
Status FeatureGate::validate() const noexcept {
  if (id.empty()) return Status::failure(ErrorCode::InvalidArgument, "feature gate id is empty");
  if (!generation.is_set()) return Status::failure(ErrorCode::InvalidArgument, "feature gate generation is unset");
  if (!min_runtime_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "feature gate must bind a minimum runtime generation");
  }
  if (!min_protocol_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "feature gate must bind a minimum protocol generation");
  }
  if (!min_schema_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "feature gate must bind a minimum schema generation");
  }
  if (is_blocking_outcome(required_peer_outcome)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "feature gate requires a peer outcome that can never be satisfied");
  }
  if (enabled && enabled_by.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "enabled feature gate must record the enabling plan");
  }
  if (provenance == EvidenceClass::Unknown) {
    return Status::failure(ErrorCode::InvalidArgument, "feature gate has no evidence provenance");
  }
  return Status::ok();
}

Status FeatureGateRegistry::define(const FeatureGate& gate) {
  const Status valid = gate.validate();
  if (valid.is_failure()) return valid;
  const auto existing = gates_.find(gate.id);
  if (existing == gates_.end()) {
    if (gates_.size() >= limits::kFeatureGates) {
      return Status::failure(ErrorCode::LimitExceeded, "feature gate bound reached");
    }
    gates_.emplace(gate.id, gate);
    generation_ = generation_.is_set() ? generation_.next() : FeatureGateGeneration::first();
    return Status::ok();
  }
  // Enabling and disabling must go through set_enabled.
  if (existing->second.enabled != gate.enabled) {
    return Status::failure(ErrorCode::Conflict,
                           "feature-gate enablement changes must use set_enabled to record authority");
  }
  const FeatureGate merged = gate;
  gates_[gate.id] = merged;
  generation_ = generation_.is_set() ? generation_.next() : FeatureGateGeneration::first();
  return Status::ok();
}

Status FeatureGateRegistry::restore(const FeatureGate& gate) {
  const Status valid = gate.validate();
  if (valid.is_failure()) return valid;
  if (gates_.find(gate.id) != gates_.end()) {
    return Status::failure(ErrorCode::AlreadyExists, "duplicate feature gate id in durable state");
  }
  if (gates_.size() >= limits::kFeatureGates) {
    return Status::failure(ErrorCode::LimitExceeded, "feature gate bound reached while loading");
  }
  gates_.emplace(gate.id, gate);
  return Status::ok();
}

Status FeatureGateRegistry::set_enabled(const FeatureGateId& id, bool enabled, const EvolutionPlanId& plan,
                                        StageGeneration stage) {
  const auto existing = gates_.find(id);
  if (existing == gates_.end()) {
    return Status::failure(ErrorCode::NotFound, "feature gate is not defined");
  }
  if (existing->second.enabled == enabled) {
    return Status::failure(ErrorCode::Conflict, "feature gate is already in the requested state");
  }
  if (enabled && plan.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "enabling a feature gate requires the enabling plan");
  }
  existing->second.enabled = enabled;
  existing->second.enabled_by = enabled ? plan : EvolutionPlanId{};
  existing->second.enabled_at = enabled ? stage : StageGeneration{};
  generation_ = generation_.is_set() ? generation_.next() : FeatureGateGeneration::first();
  return Status::ok();
}

const FeatureGate* FeatureGateRegistry::find(const FeatureGateId& id) const noexcept {
  const auto it = gates_.find(id);
  return it == gates_.end() ? nullptr : &it->second;
}

std::vector<const FeatureGate*> FeatureGateRegistry::all() const {
  std::vector<const FeatureGate*> out;
  out.reserve(gates_.size());
  for (const auto& [id, gate] : gates_) {
    (void)id;
    out.push_back(&gate);
  }
  return out;
}

Status FeatureGateRegistry::validate() const {
  if (gates_.size() > limits::kFeatureGates) {
    return Status::failure(ErrorCode::Corrupt, "feature gate registry exceeds the bound");
  }
  for (const auto& [id, gate] : gates_) {
    if (id != gate.id) return Status::failure(ErrorCode::Corrupt, "feature gate key does not match its id");
    const Status valid = gate.validate();
    if (valid.is_failure()) return valid;
  }
  return Status::ok();
}

GateDecision FeatureGateRegistry::evaluate(const FeatureGateId& id, const GateContext& context,
                                         bool require_enabled) const {
  GateDecision decision;
  const FeatureGate* gate = find(id);
  if (gate == nullptr) {
    decision.status = Status::failure(ErrorCode::NotFound, "feature gate is not defined");
    decision.explanation.push_back("feature gate is not defined");
    return decision;
  }
  if (require_enabled && !gate->enabled) {
    decision.status = Status::failure(ErrorCode::Unauthorized, "feature gate is not enabled");
    decision.explanation.push_back("feature gate is disabled");
    return decision;
  }
  if (!gate->scope.empty() && gate->scope != context.cohort) {
    decision.status = Status::failure(ErrorCode::Unauthorized, "feature gate does not cover this cohort");
    decision.explanation.push_back("feature gate scope excludes the requesting cohort");
    return decision;
  }
  if (context.runtime_generation < gate->min_runtime_generation) {
    decision.status = Status::failure(ErrorCode::StaleGeneration,
                                      "runtime generation is below the feature's minimum");
    decision.explanation.push_back("runtime generation below the feature minimum");
    return decision;
  }
  if (context.protocol_generation < gate->min_protocol_generation) {
    decision.status = Status::failure(ErrorCode::StaleGeneration,
                                      "protocol generation is below the feature's minimum");
    decision.explanation.push_back("protocol generation below the feature minimum");
    return decision;
  }
  if (context.schema_generation < gate->min_schema_generation) {
    decision.status = Status::failure(ErrorCode::StaleGeneration,
                                      "schema generation is below the feature's minimum");
    decision.explanation.push_back("schema generation below the feature minimum");
    return decision;
  }
  if (is_blocking_outcome(context.peer_outcome)) {
    decision.status = Status::failure(ErrorCode::Incompatible, "peer compatibility outcome blocks the feature");
    decision.explanation.push_back(std::string("peer outcome ") + to_string(context.peer_outcome) +
                                   " blocks the feature");
    return decision;
  }
  // The gate's own required peer outcome must actually hold.
  const auto required_rank = static_cast<std::uint8_t>(gate->required_peer_outcome);
  const auto actual_rank = static_cast<std::uint8_t>(context.peer_outcome);
  const bool outcome_ok =
      context.peer_outcome == gate->required_peer_outcome ||
      context.peer_outcome == CompatOutcome::FullyCompatible ||
      (gate->required_peer_outcome == CompatOutcome::FullyCompatible && actual_rank <= required_rank);
  if (!outcome_ok) {
    decision.status = Status::failure(ErrorCode::Incompatible, "peer outcome does not satisfy the feature gate");
    decision.explanation.push_back(std::string("peer outcome ") + to_string(context.peer_outcome) +
                                   " does not satisfy required " + to_string(gate->required_peer_outcome));
    return decision;
  }
  decision.permitted = true;
  decision.status = Status::ok();
  decision.explanation.push_back("feature gate satisfied at generation " +
                                 std::to_string(gate->generation.raw()));
  return decision;
}

}  // namespace ref
