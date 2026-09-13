// Runtime Evolution Fabric - authority, rollback, drain, retirement, reconciliation.
//
// This header answers the two questions the fabric exists for:
//   * which runtime generation is authoritative for a component right now, and
//     which generations may still participate, with which permissions;
//   * whether a rollback, drain or retirement is legal given what has already
//     happened to state and protocol generations.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/compat.hpp"
#include "ref/component.hpp"
#include "ref/ids.hpp"
#include "ref/plan.hpp"
#include "ref/protocol.hpp"
#include "ref/schema.hpp"
#include "ref/support.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Authority view
// ---------------------------------------------------------------------------
struct GenerationPermission {
  RuntimeGeneration generation{};
  LifecycleState lifecycle{LifecycleState::Registered};
  bool is_authoritative{false};
  bool may_read{false};
  bool may_write{false};
  OperationClass operation{OperationClass::None};
  ProtocolGeneration protocol{};
  SchemaGeneration schema{};
  DetailText reason{};
};

struct AuthorityView {
  RuntimeComponentId component{};
  bool has_authoritative{false};
  RuntimeGeneration authoritative{};
  std::vector<GenerationPermission> coexistence{};
  ProtocolGeneration protocol_generation{};
  SchemaGeneration schema_generation{};
  std::vector<FeatureGateId> enabled_features{};
  RolloutStage stage{RolloutStage::None};
  EvolutionPlanId plan{};
  bool new_writer_enabled{false};
  bool evidence_current{false};
  bool rollback_barrier_crossed{false};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  SnapshotGeneration snapshot_generation{};
  std::vector<std::string> explanation{};

  [[nodiscard]] const GenerationPermission* permission_for(RuntimeGeneration generation) const noexcept;
  [[nodiscard]] bool may_write(RuntimeGeneration generation) const noexcept;
  [[nodiscard]] std::string to_json() const;
};

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------
enum class RollbackOutcome : std::uint8_t {
  Allowed = 0,
  RequiresStateRestore,
  RequiresProtocolDowngrade,
  BlockedIrreversibleMigration,
  BlockedSchema,
  BlockedProtocol,
  BlockedRetiredTarget,
  RevalidationRequired,
  Committed,
  Failed,
};

inline constexpr std::size_t kRollbackOutcomeCount = 10;
[[nodiscard]] const char* to_string(RollbackOutcome outcome) noexcept;
[[nodiscard]] std::optional<RollbackOutcome> parse_rollback_outcome(std::string_view text) noexcept;

struct RollbackRequest {
  RollbackId id{};
  RollbackGeneration generation{};
  RuntimeComponentId component{};
  EvolutionPlanId plan{};
  EvolutionPlanGeneration plan_generation{};
  SchemaId schema{};
  RuntimeGeneration current_generation{};
  RuntimeGeneration target_generation{};
  ProtocolGeneration current_protocol{};
  ProtocolGeneration target_protocol{};
  SchemaGeneration current_schema{};
  SchemaGeneration target_schema{};
  bool irreversible_migration_crossed{false};
  bool reverse_migration_available{false};
  bool state_restore_available{false};
  bool target_process_available{false};
  bool plan_superseded{false};
  FeatureGateGeneration gate_generation{};
  PolicyGeneration policy{};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  EvidenceGeneration evidence{};
  EvidenceGeneration evidence_floor{};
  bool evidence_current{false};
  RolloutStage current_stage{RolloutStage::None};
};

struct RollbackAssessment {
  Status status{};
  RollbackOutcome outcome{RollbackOutcome::Failed};
  std::vector<std::string> explanation{};
  [[nodiscard]] bool is_allowed() const noexcept {
    return outcome == RollbackOutcome::Allowed || outcome == RollbackOutcome::RequiresStateRestore ||
           outcome == RollbackOutcome::RequiresProtocolDowngrade || outcome == RollbackOutcome::Committed;
  }
};

// Deterministic, ordered evaluation. The registry is consulted for the target
// generation's lifecycle; the matrix for control-channel compatibility in the
// reverse direction.
[[nodiscard]] RollbackAssessment evaluate_rollback(const RollbackRequest& request,
                                                   const CompatibilityMatrix& matrix,
                                                   const ComponentRegistry& components,
                                                   const SchemaRegistry& schemas);

// ---------------------------------------------------------------------------
// Drain and retirement
// ---------------------------------------------------------------------------
struct DrainStatus {
  RuntimeComponentId component{};
  RuntimeGeneration generation{};
  bool draining{false};
  std::uint64_t live_processes{0};
  std::uint64_t active_workers{0};
  bool accepts_new_work{false};
  bool accepting_new_state{false};
  bool blocks_retirement{true};
  DetailText detail{};

  [[nodiscard]] std::string to_json() const;
};

struct RetirementPreconditions {
  RuntimeComponentId component{};
  RuntimeGeneration generation{};
  std::uint64_t live_processes{0};
  std::uint64_t active_workers{0};
  bool pending_work{false};
  bool incompatible_writers_fenced{false};
  bool migration_complete{false};
  bool rollback_policy_allows_retirement{false};
  bool checkpoints_retained{false};
  bool superseded{false};
};

struct RetirementAssessment {
  Status status{};
  bool allowed{false};
  std::vector<std::string> explanation{};
};

[[nodiscard]] RetirementAssessment evaluate_retirement(const RetirementPreconditions& preconditions);

struct RetirementRecord {
  RuntimeComponentId component{};
  RuntimeGeneration generation{};
  StageGeneration recorded_at{};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  bool checkpoints_retained{false};
  DetailText detail{};
};

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------
enum class FindingKind : std::uint8_t {
  CandidateExpectedAliveButAbsent = 0,
  OldRuntimeStillAliveAfterDrain,
  MigrationMayHaveCommitted,
  FeatureGateDiffersFromExpectation,
  SchemaGenerationAdvancedExternally,
  ProtocolGenerationDiffers,
  CandidateStartedOutsideCoordinator,
  RetiredRuntimeReconnected,
  StaleEvidence,
  UnexpectedLifecycle,
};

inline constexpr std::size_t kFindingKindCount = 10;
[[nodiscard]] const char* to_string(FindingKind kind) noexcept;

struct ReconcileFinding {
  FindingKind kind{FindingKind::UnexpectedLifecycle};
  RuntimeComponentId component{};
  RuntimeGeneration expected_generation{};
  RuntimeGeneration observed_generation{};
  ProtocolGeneration expected_protocol{};
  ProtocolGeneration observed_protocol{};
  SchemaGeneration expected_schema{};
  SchemaGeneration observed_schema{};
  WorkerId worker{};
  WorkerBootId boot{};
  bool conservative_action_required{false};
  NoteText detail{};
};

struct ReconcileReport {
  std::vector<ReconcileFinding> findings{};
  bool requires_operator{false};
  [[nodiscard]] bool empty() const noexcept { return findings.empty(); }
  [[nodiscard]] std::string to_json() const;
};

// ---------------------------------------------------------------------------
// Immutable authority snapshot
// ---------------------------------------------------------------------------
struct AuthoritySnapshot {
  SnapshotGeneration generation{};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  CompatibilityGeneration matrix_generation{};
  FeatureGateGeneration gate_generation{};
  EvidenceGeneration evidence{};
  std::string canonical{};

  // A snapshot taken under a superseded epoch is never current: a snapshot is
  // evidence about a moment, not authority that survives restart.
  [[nodiscard]] bool is_current_for(EvolutionEpoch current_epoch,
                                    CoordinatorEpoch current_coordinator) const noexcept {
    return epoch == current_epoch && coordinator_epoch == current_coordinator;
  }
};

}  // namespace ref
