// Runtime Evolution Fabric - evolution plans, rollout stages and feature gates.
//
// A plan binds every generation that a live evolution depends on. When any
// bound generation moves, the plan is stale and cannot advance: that is the
// mechanism that keeps a rollout from silently drifting away from the
// compatibility evidence it was approved against.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/compat.hpp"
#include "ref/ids.hpp"
#include "ref/support.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Rollout stages
// ---------------------------------------------------------------------------
enum class RolloutStage : std::uint8_t {
  None = 0,
  CandidateRegistered,
  CompatibilityProven,
  CanaryCohort,
  MixedVersionCohort,
  ExpandedCohort,
  MigrationBarrier,
  NewWriterEnabled,
  OldWriterDrain,
  FullPromotion,
  OldGenerationRetirement,
  Superseded,
  RolledBack,
};

inline constexpr std::size_t kRolloutStageCount = 13;
[[nodiscard]] const char* to_string(RolloutStage stage) noexcept;
[[nodiscard]] std::optional<RolloutStage> parse_rollout_stage(std::string_view text) noexcept;
// Monotonic rank for forward progression; terminal side states report 0.
[[nodiscard]] std::uint8_t rollout_stage_rank(RolloutStage stage) noexcept;
[[nodiscard]] bool is_terminal_stage(RolloutStage stage) noexcept;
// Stages that separate authority classes: crossing them changes what peers may do.
[[nodiscard]] bool is_authority_barrier_stage(RolloutStage stage) noexcept;

struct PlanStageRecord {
  RolloutStageId stage_id{};
  RolloutStage stage{RolloutStage::None};
  StageGeneration generation{};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  EvidenceGeneration evidence{};
  bool completed{false};  // set exactly once when a completion is accepted
  NoteText note{};
};

// Which barriers a rollout is required to cross explicitly.
struct RolloutPolicy {
  bool require_canary{true};
  bool require_mixed_version_cohort{true};
  bool require_migration_barrier{false};
  bool require_new_writer_barrier{true};
  bool require_old_writer_drain{true};
  bool require_rollback_proof{true};
  PolicyGeneration generation{};
};

// ---------------------------------------------------------------------------
// Evolution plan
// ---------------------------------------------------------------------------
struct EvolutionPlan {
  EvolutionPlanId id{};
  EvolutionPlanGeneration generation{};
  RuntimeComponentId component{};
  RuntimeGeneration current_generation{};
  RuntimeGeneration candidate_generation{};
  CompatibilityGeneration matrix_generation{};
  ProtocolGeneration protocol_generation{};
  SchemaGeneration schema_generation{};
  FeatureGateGeneration gate_generation{};
  MigrationId migration{};
  MigrationGeneration migration_generation{};
  SchemaGeneration migration_source{};
  SchemaGeneration migration_target{};
  RuntimeGeneration rollback_target{};
  RollbackGeneration rollback_generation{};
  bool rollback_barrier_crossed{false};
  std::vector<RequiredEvidence> required_evidence{};
  CoordinatorEpoch coordinator_epoch{};
  EvolutionEpoch epoch{};
  RolloutStage stage{RolloutStage::None};
  StageGeneration stage_generation{};
  std::vector<CohortId> cohorts{};
  CohortId canary_cohort{};
  bool new_writer_enabled{false};
  RolloutPolicy policy{};
  std::vector<PlanStageRecord> history{};
  bool superseded{false};
  EvidenceGeneration evidence{};
  DetailText detail{};

  [[nodiscard]] Status validate() const noexcept;
  [[nodiscard]] bool is_active() const noexcept {
    return !superseded && !is_terminal_stage(stage) && stage != RolloutStage::None;
  }
  [[nodiscard]] bool has_cohort(const CohortId& cohort) const noexcept;
};

// Preconditions evaluated by the coordinator before a stage advance.
struct StagePreconditions {
  bool compatibility_proven{false};
  bool migration_contract_bound{false};
  bool migration_complete{false};
  bool migration_irreversible{false};
  bool old_writers_drained{false};
  bool new_writer_fence_satisfied{false};
  bool rollback_valid{false};
  bool evidence_current{false};
  bool canary_cohort_active{false};
  bool mixed_version_active{false};
};

struct StageDecision {
  Status status{};
  bool allowed{false};
  std::vector<std::string> explanation{};

  [[nodiscard]] bool is_allowed() const noexcept { return allowed; }
};

// Pure, deterministic stage-transition decision. No state is mutated here.
[[nodiscard]] StageDecision evaluate_stage_advance(const EvolutionPlan& plan, RolloutStage target,
                                                   const StagePreconditions& preconditions);

// ---------------------------------------------------------------------------
// Feature gates
// ---------------------------------------------------------------------------
struct FeatureGate {
  FeatureGateId id{};
  FeatureGateGeneration generation{};
  RuntimeGeneration min_runtime_generation{};
  ProtocolGeneration min_protocol_generation{};
  SchemaGeneration min_schema_generation{};
  CompatAspect required_peer_aspect{CompatAspect::PeerVersion};
  CompatOutcome required_peer_outcome{CompatOutcome::FullyCompatible};
  CohortId scope{};      // empty == every cohort in the plan
  bool enabled{false};
  EvolutionPlanId enabled_by{};
  StageGeneration enabled_at{};
  EvidenceClass provenance{EvidenceClass::Unknown};
  EvidenceGeneration evidence{};
  DetailText notes{};

  [[nodiscard]] Status validate() const noexcept;
};

struct GateDecision {
  Status status{};
  bool permitted{false};
  std::vector<std::string> explanation{};
};

// Evaluated against concrete runtime facts rather than against optimism.
struct GateContext {
  RuntimeGeneration runtime_generation{};
  ProtocolGeneration protocol_generation{};
  SchemaGeneration schema_generation{};
  CompatOutcome peer_outcome{CompatOutcome::Unknown};
  CohortId cohort{};
  EvolutionPlanId plan{};
  StageGeneration stage{};
};

class FeatureGateRegistry {
 public:
  FeatureGateRegistry() = default;

  Status define(const FeatureGate& gate);
  // Restores a gate from durable state without bumping the table generation.
  Status restore(const FeatureGate& gate);
  // Enabling or disabling bumps the table generation, which invalidates plans
  // bound to the previous revision.
  Status set_enabled(const FeatureGateId& id, bool enabled, const EvolutionPlanId& plan,
                     StageGeneration stage);
  [[nodiscard]] const FeatureGate* find(const FeatureGateId& id) const noexcept;
  [[nodiscard]] std::vector<const FeatureGate*> all() const;
  [[nodiscard]] FeatureGateGeneration generation() const noexcept { return generation_; }
  void set_generation(FeatureGateGeneration generation) noexcept { generation_ = generation; }
  [[nodiscard]] std::size_t size() const noexcept { return gates_.size(); }
  [[nodiscard]] Status validate() const;

  // When require_enabled is false the gate's own enablement is not part of the
  // decision, which is what an enable request needs to evaluate.
  [[nodiscard]] GateDecision evaluate(const FeatureGateId& id, const GateContext& context,
                                      bool require_enabled = true) const;

 private:
  std::map<FeatureGateId, FeatureGate> gates_{};
  FeatureGateGeneration generation_{};
};

}  // namespace ref
