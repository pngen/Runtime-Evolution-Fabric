// Runtime Evolution Fabric - durable evolution state.
//
// What is persisted here is exactly the durable evolution structure: identities,
// generations, compatibility facts, plans, committed history, migration
// records, rollback barriers, feature gates, retirement records and worker boot
// fencing. What is deliberately NOT persisted: process liveness, connection
// state, worker authority and dynamic health. A coordinator restart restores
// structure and fences boots; it never restores authority.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/authority.hpp"
#include "ref/compat.hpp"
#include "ref/component.hpp"
#include "ref/ids.hpp"
#include "ref/migration.hpp"
#include "ref/plan.hpp"
#include "ref/protocol.hpp"
#include "ref/schema.hpp"
#include "ref/support.hpp"

namespace ref {

inline constexpr char kEvolutionStateMagic[8] = {'R', 'E', 'F', 'E', 'V', 'O', 'L', '1'};
inline constexpr std::uint32_t kEvolutionStateFormatVersion = 1;

// Durable record of a worker identity. Liveness is not stored.
struct WorkerRecord {
  WorkerId worker{};
  RuntimeComponentId component{};
  RuntimeGeneration generation{};
  WorkerBootId last_boot{};
  bool fenced{false};
  bool drained{false};
  EvidenceGeneration evidence{};
  DetailText detail{};
};

// A boot that was fenced. Fencing is durable: a fenced boot never regains
// authority by reconnecting, not even across a coordinator restart.
struct FencedBoot {
  WorkerId worker{};
  WorkerBootId boot{};
  RuntimeComponentId component{};
  RuntimeGeneration generation{};
  NoteText reason{};
  StageGeneration fenced_at{};
};

struct RollbackRecord {
  RollbackId id{};
  RollbackGeneration generation{};
  RuntimeComponentId component{};
  RuntimeGeneration from_generation{};
  RuntimeGeneration to_generation{};
  EvolutionPlanId plan{};
  RollbackOutcome outcome{RollbackOutcome::Failed};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  StageGeneration recorded_at{};
  DetailText detail{};
};

// The complete durable evolution structure.
struct DurableState {
  std::uint32_t format_version{kEvolutionStateFormatVersion};
  RuntimeVersionId writer_version{};
  EvolutionEpoch evolution_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  CompatibilityGeneration matrix_generation{};
  FeatureGateGeneration gate_generation{};
  EvidenceGeneration evidence_generation{};
  SnapshotGeneration snapshot_generation{};
  PolicyGeneration policy_generation{};
  StageGeneration stage_generation{};
  MigrationGeneration migration_generation{};
  RollbackGeneration rollback_generation{};
  SnapshotGeneration replay_watermark{};

  ComponentRegistry components{};
  CompatibilityMatrix matrix{};
  ProtocolRegistry protocols{};
  SchemaRegistry schemas{};
  FeatureGateRegistry gates{};
  std::map<EvolutionPlanId, EvolutionPlan> plans{};
  std::vector<MigrationRecord> migrations{};
  std::vector<RollbackRecord> rollbacks{};
  std::map<std::pair<RuntimeComponentId, std::uint64_t>, RetirementRecord> retirements{};
  std::map<std::pair<WorkerId, std::uint64_t>, FencedBoot> fenced_boots{};
  std::map<WorkerId, WorkerRecord> workers{};
  std::vector<PlanStageRecord> stage_history{};

  [[nodiscard]] bool is_retired(const RuntimeComponentId& component, RuntimeGeneration generation) const;
  [[nodiscard]] bool is_boot_fenced(const WorkerId& worker, WorkerBootId boot) const;
  [[nodiscard]] Status validate() const;
};

class DurableStore {
 public:
  explicit DurableStore(std::string path) : path_(std::move(path)) {}

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  void set_path(std::string path) { path_ = std::move(path); }

  [[nodiscard]] Status save(const DurableState& state) const;
  // Load into a temporary structure; the destination is untouched on failure.
  [[nodiscard]] Status load(DurableState& out) const;
  [[nodiscard]] bool exists() const noexcept { return file_exists(path_); }

  [[nodiscard]] static Status encode(const DurableState& state, std::string& out);
  [[nodiscard]] static Status decode(std::string_view bytes, DurableState& out);

 private:
  std::string path_;
};

}  // namespace ref
