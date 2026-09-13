// Runtime Evolution Fabric - compatibility model and generation-bound matrix.
//
// Compatibility is deliberately not one boolean. Every aspect of a runtime pair
// is assessed separately, and the permissions the pair receives are derived
// from those assessments by one deterministic function so that the same matrix
// always yields the same control decision.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/ids.hpp"
#include "ref/support.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Aspects
// ---------------------------------------------------------------------------
enum class CompatAspect : std::uint8_t {
  BinaryApi = 0,
  Abi,
  WireProtocol,
  ProtocolRead,
  ProtocolWrite,
  SchemaRead,
  SchemaWrite,
  StateMigration,
  PeerVersion,
  FeatureGate,
  PersistenceFormat,
  Snapshot,
  Rollback,
  Capability,
};

inline constexpr std::size_t kCompatAspectCount = 14;
[[nodiscard]] const char* to_string(CompatAspect aspect) noexcept;
[[nodiscard]] std::optional<CompatAspect> parse_compat_aspect(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------
enum class CompatOutcome : std::uint8_t {
  FullyCompatible = 0,
  ReadCompatible,
  WriteCompatible,
  MixedVersionCompatible,
  CompatibleWithFeatureGate,
  CompatibleAfterStateMigration,
  CompatibleAfterProtocolNegotiation,
  RollbackCompatible,
  IncompatibleProtocol,
  IncompatibleSchema,
  IncompatibleAbi,
  IncompatibleFeatureSet,
  IncompatibleRollback,
  IncompatiblePeer,
  Unknown,
  StaleEvidence,
  Unsupported,
};

inline constexpr std::size_t kCompatOutcomeCount = 17;
[[nodiscard]] const char* to_string(CompatOutcome outcome) noexcept;
[[nodiscard]] std::optional<CompatOutcome> parse_compat_outcome(std::string_view text) noexcept;

// Blocking outcomes stop the decision: they never silently become compatible.
[[nodiscard]] bool is_blocking_outcome(CompatOutcome outcome) noexcept;
// Permissive outcomes allow a decision to proceed, possibly with a condition.
[[nodiscard]] bool is_permissive_outcome(CompatOutcome outcome) noexcept;

struct AspectAssessment {
  CompatOutcome outcome{CompatOutcome::Unknown};
  EvidenceClass provenance{EvidenceClass::Unknown};
  EvidenceGeneration evidence{};
  DetailText detail{};

  [[nodiscard]] bool is_current(EvidenceGeneration floor, std::uint64_t max_age) const noexcept;
};

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------
enum class CompatPermission : std::uint16_t {
  None = 0,
  ControlChannel = 1u << 0,
  ReadOnlyMessages = 1u << 1,
  MutatingMessages = 1u << 2,
  ReadState = 1u << 3,
  WriteSharedState = 1u << 4,
  ShareSnapshots = 1u << 5,
  JoinControlEpoch = 1u << 6,
  CoexistDuringRollout = 1u << 7,
  RollbackAfterMutation = 1u << 8,
};

using CompatPermissions = std::uint16_t;
[[nodiscard]] constexpr CompatPermissions permission_bit(CompatPermission permission) noexcept {
  return static_cast<CompatPermissions>(permission);
}
[[nodiscard]] constexpr bool has_permission(CompatPermissions set, CompatPermission permission) noexcept {
  return (set & permission_bit(permission)) != 0;
}
[[nodiscard]] constexpr CompatPermissions add_permission(CompatPermissions set,
                                                         CompatPermission permission) noexcept {
  return static_cast<CompatPermissions>(set | permission_bit(permission));
}
[[nodiscard]] const char* to_string(CompatPermission permission) noexcept;
[[nodiscard]] std::string describe_permissions(CompatPermissions set);

// ---------------------------------------------------------------------------
// Compatibility edge
// ---------------------------------------------------------------------------
struct CompatibilityEdge {
  RuntimeGeneration from{};
  RuntimeGeneration to{};
  std::array<AspectAssessment, kCompatAspectCount> aspects{};
  CompatPermissions permissions{0};
  CompatibilityGeneration generation{};
  EvidenceGeneration evidence{};
  EvolutionEpoch epoch{};
  bool requires_feature_gate{false};
  FeatureGateId gate{};
  FeatureGateGeneration gate_generation{};
  bool requires_protocol_downgrade{false};
  ProtocolGeneration downgrade_to{};
  bool requires_state_translation{false};
  MigrationId migration{};
  MigrationGeneration migration_generation{};

  [[nodiscard]] const AspectAssessment& aspect(CompatAspect which) const noexcept {
    return aspects[static_cast<std::size_t>(which)];
  }
  [[nodiscard]] AspectAssessment& aspect(CompatAspect which) noexcept {
    return aspects[static_cast<std::size_t>(which)];
  }
};

// The single deterministic derivation from aspects to permissions.
[[nodiscard]] CompatPermissions derive_permissions(const CompatibilityEdge& edge) noexcept;
// Structural and cross-field validation; powers both publish and durable load.
[[nodiscard]] Status validate_edge(const CompatibilityEdge& edge) noexcept;

// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------
class CompatibilityMatrix {
 public:
  CompatibilityMatrix() = default;

  // Publishes or replaces an edge. Replacing an edge bumps the matrix
  // generation, which invalidates evolution plans bound to the older revision.
  Status upsert(const CompatibilityEdge& edge);
  Status remove(RuntimeGeneration from, RuntimeGeneration to);

  [[nodiscard]] const CompatibilityEdge* find(RuntimeGeneration from,
                                              RuntimeGeneration to) const noexcept;
  [[nodiscard]] bool permits(RuntimeGeneration from, RuntimeGeneration to,
                             CompatPermission permission) const noexcept;
  // Sorted, deterministic neighbour list.
  [[nodiscard]] std::vector<RuntimeGeneration> neighbours(RuntimeGeneration from) const;
  [[nodiscard]] std::vector<const CompatibilityEdge*> all_edges() const;

  [[nodiscard]] CompatibilityGeneration generation() const noexcept { return generation_; }
  void set_generation(CompatibilityGeneration generation) noexcept { generation_ = generation; }
  void bump_generation() noexcept { generation_ = generation_.next(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }

  // Deterministic explanation lines for a pair (empty when unknown).
  [[nodiscard]] std::vector<std::string> explain(RuntimeGeneration from, RuntimeGeneration to) const;
  [[nodiscard]] Status validate() const;

 private:
  using Key = std::pair<std::uint64_t, std::uint64_t>;
  std::map<Key, CompatibilityEdge> edges_{};
  std::map<std::uint64_t, std::vector<std::uint64_t>> neighbours_{};
  CompatibilityGeneration generation_{};
};

}  // namespace ref
