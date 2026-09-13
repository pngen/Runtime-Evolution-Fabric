#include "ref/compat.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace ref {
namespace {

constexpr std::array<const char*, kCompatAspectCount> kAspectNames{
    "BINARY_API", "ABI",           "WIRE_PROTOCOL",     "PROTOCOL_READ", "PROTOCOL_WRITE",
    "SCHEMA_READ", "SCHEMA_WRITE", "STATE_MIGRATION",   "PEER_VERSION",  "FEATURE_GATE",
    "PERSISTENCE_FORMAT", "SNAPSHOT", "ROLLBACK",       "CAPABILITY"};

constexpr std::array<const char*, kCompatOutcomeCount> kOutcomeNames{
    "FULLY_COMPATIBLE", "READ_COMPATIBLE", "WRITE_COMPATIBLE", "MIXED_VERSION_COMPATIBLE",
    "COMPATIBLE_WITH_FEATURE_GATE", "COMPATIBLE_AFTER_STATE_MIGRATION",
    "COMPATIBLE_AFTER_PROTOCOL_NEGOTIATION", "ROLLBACK_COMPATIBLE", "INCOMPATIBLE_PROTOCOL",
    "INCOMPATIBLE_SCHEMA", "INCOMPATIBLE_ABI", "INCOMPATIBLE_FEATURE_SET", "INCOMPATIBLE_ROLLBACK",
    "INCOMPATIBLE_PEER", "UNKNOWN", "STALE_EVIDENCE", "UNSUPPORTED"};

}  // namespace

const char* to_string(CompatAspect aspect) noexcept {
  const auto index = static_cast<std::size_t>(aspect);
  return index < kAspectNames.size() ? kAspectNames[index] : "UNKNOWN_ASPECT";
}

std::optional<CompatAspect> parse_compat_aspect(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kAspectNames.size(); ++i) {
    if (text == kAspectNames[i]) return static_cast<CompatAspect>(i);
  }
  return std::nullopt;
}

const char* to_string(CompatOutcome outcome) noexcept {
  const auto index = static_cast<std::size_t>(outcome);
  return index < kOutcomeNames.size() ? kOutcomeNames[index] : "UNKNOWN";
}

std::optional<CompatOutcome> parse_compat_outcome(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kOutcomeNames.size(); ++i) {
    if (text == kOutcomeNames[i]) return static_cast<CompatOutcome>(i);
  }
  return std::nullopt;
}

bool is_blocking_outcome(CompatOutcome outcome) noexcept {
  switch (outcome) {
    case CompatOutcome::IncompatibleProtocol:
    case CompatOutcome::IncompatibleSchema:
    case CompatOutcome::IncompatibleAbi:
    case CompatOutcome::IncompatibleFeatureSet:
    case CompatOutcome::IncompatibleRollback:
    case CompatOutcome::IncompatiblePeer:
    case CompatOutcome::Unknown:
    case CompatOutcome::StaleEvidence:
    case CompatOutcome::Unsupported:
      return true;
    default:
      return false;
  }
}

bool is_permissive_outcome(CompatOutcome outcome) noexcept { return !is_blocking_outcome(outcome); }

bool AspectAssessment::is_current(EvidenceGeneration floor, std::uint64_t max_age) const noexcept {
  if (provenance == EvidenceClass::Unknown) return false;
  if (outcome == CompatOutcome::Unknown || outcome == CompatOutcome::StaleEvidence) return false;
  if (evidence.is_stale_relative_to(floor)) return false;
  if (max_age == 0) return true;
  if (evidence.raw() < floor.raw()) return false;
  return (evidence.raw() - floor.raw()) <= max_age;
}

const char* to_string(CompatPermission permission) noexcept {
  switch (permission) {
    case CompatPermission::None: return "NONE";
    case CompatPermission::ControlChannel: return "CONTROL_CHANNEL";
    case CompatPermission::ReadOnlyMessages: return "READ_ONLY_MESSAGES";
    case CompatPermission::MutatingMessages: return "MUTATING_MESSAGES";
    case CompatPermission::ReadState: return "READ_STATE";
    case CompatPermission::WriteSharedState: return "WRITE_SHARED_STATE";
    case CompatPermission::ShareSnapshots: return "SHARE_SNAPSHOTS";
    case CompatPermission::JoinControlEpoch: return "JOIN_CONTROL_EPOCH";
    case CompatPermission::CoexistDuringRollout: return "COEXIST_DURING_ROLLOUT";
    case CompatPermission::RollbackAfterMutation: return "ROLLBACK_AFTER_MUTATION";
  }
  return "NONE";
}

std::string describe_permissions(CompatPermissions set) {
  if (set == 0) return "NONE";
  std::string out;
  constexpr std::array<CompatPermission, 9> kOrder{
      CompatPermission::ControlChannel,      CompatPermission::ReadOnlyMessages,
      CompatPermission::MutatingMessages,    CompatPermission::ReadState,
      CompatPermission::WriteSharedState,    CompatPermission::ShareSnapshots,
      CompatPermission::JoinControlEpoch,    CompatPermission::CoexistDuringRollout,
      CompatPermission::RollbackAfterMutation};
  for (const auto permission : kOrder) {
    if (!has_permission(set, permission)) continue;
    if (!out.empty()) out += '|';
    out += to_string(permission);
  }
  return out.empty() ? std::string("NONE") : out;
}

CompatPermissions derive_permissions(const CompatibilityEdge& edge) noexcept {
  const auto& binary_api = edge.aspect(CompatAspect::BinaryApi);
  const auto& abi = edge.aspect(CompatAspect::Abi);
  const auto& wire = edge.aspect(CompatAspect::WireProtocol);
  const auto& protocol_read = edge.aspect(CompatAspect::ProtocolRead);
  const auto& protocol_write = edge.aspect(CompatAspect::ProtocolWrite);
  const auto& schema_read = edge.aspect(CompatAspect::SchemaRead);
  const auto& schema_write = edge.aspect(CompatAspect::SchemaWrite);
  const auto& migration = edge.aspect(CompatAspect::StateMigration);
  const auto& peer = edge.aspect(CompatAspect::PeerVersion);
  const auto& feature = edge.aspect(CompatAspect::FeatureGate);
  const auto& persistence = edge.aspect(CompatAspect::PersistenceFormat);
  const auto& snapshot = edge.aspect(CompatAspect::Snapshot);
  const auto& rollback = edge.aspect(CompatAspect::Rollback);
  const auto& capability = edge.aspect(CompatAspect::Capability);

  CompatPermissions permissions = 0;
  const bool control = is_permissive_outcome(binary_api.outcome) && is_permissive_outcome(abi.outcome) &&
                       is_permissive_outcome(wire.outcome) && is_permissive_outcome(capability.outcome);
  if (control) permissions = add_permission(permissions, CompatPermission::ControlChannel);

  const bool read_only = control && is_permissive_outcome(protocol_read.outcome);
  if (read_only) permissions = add_permission(permissions, CompatPermission::ReadOnlyMessages);

  const bool mutating = read_only && is_permissive_outcome(protocol_write.outcome);
  if (mutating) permissions = add_permission(permissions, CompatPermission::MutatingMessages);

  const bool read_state = control && is_permissive_outcome(schema_read.outcome) &&
                          is_permissive_outcome(persistence.outcome);
  if (read_state) permissions = add_permission(permissions, CompatPermission::ReadState);

  const bool write_state = mutating && read_state && is_permissive_outcome(schema_write.outcome) &&
                           is_permissive_outcome(migration.outcome);
  if (write_state) permissions = add_permission(permissions, CompatPermission::WriteSharedState);

  const bool snapshots = read_state && is_permissive_outcome(snapshot.outcome);
  if (snapshots) permissions = add_permission(permissions, CompatPermission::ShareSnapshots);

  const bool join_epoch = mutating && is_permissive_outcome(peer.outcome) &&
                          is_permissive_outcome(feature.outcome);
  if (join_epoch) permissions = add_permission(permissions, CompatPermission::JoinControlEpoch);

  const bool coexist = control && read_only && is_permissive_outcome(peer.outcome);
  if (coexist) permissions = add_permission(permissions, CompatPermission::CoexistDuringRollout);

  if (rollback.outcome == CompatOutcome::RollbackCompatible ||
      rollback.outcome == CompatOutcome::FullyCompatible) {
    if (is_permissive_outcome(migration.outcome)) {
      permissions = add_permission(permissions, CompatPermission::RollbackAfterMutation);
    }
  }
  return permissions;
}

Status validate_edge(const CompatibilityEdge& edge) noexcept {
  if (!edge.from.is_set() || !edge.to.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "compatibility edge generations must be set");
  }
  if (edge.from == edge.to) {
    return Status::failure(ErrorCode::InvalidArgument, "compatibility edge must relate distinct generations");
  }
  if (!edge.generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "compatibility edge has no matrix generation");
  }
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    const auto raw = static_cast<std::uint8_t>(edge.aspects[i].outcome);
    if (raw >= kCompatOutcomeCount) {
      return Status::failure(ErrorCode::Corrupt, "compatibility aspect outcome out of range");
    }
    if (edge.aspects[i].provenance == EvidenceClass::Unknown &&
        edge.aspects[i].outcome != CompatOutcome::Unknown) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "aspect declares an outcome without evidence provenance");
    }
  }
  const CompatPermissions derived = derive_permissions(edge);
  if (edge.permissions != derived) {
    return Status::failure(ErrorCode::Corrupt,
                           "compatibility permissions disagree with derived aspect outcome");
  }
  if (edge.requires_feature_gate) {
    if (edge.gate.empty() || !edge.gate_generation.is_set()) {
      return Status::failure(ErrorCode::InvalidArgument, "feature-gated edge must name gate and generation");
    }
    if (edge.aspect(CompatAspect::FeatureGate).outcome != CompatOutcome::CompatibleWithFeatureGate) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "edge requires a feature gate but the feature aspect says otherwise");
    }
  }
  if (edge.requires_protocol_downgrade) {
    if (!edge.downgrade_to.is_set()) {
      return Status::failure(ErrorCode::InvalidArgument, "protocol downgrade edge must name a generation");
    }
    const auto outcome = edge.aspect(CompatAspect::WireProtocol).outcome;
    if (outcome != CompatOutcome::CompatibleAfterProtocolNegotiation &&
        outcome != CompatOutcome::CompatibleWithFeatureGate) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "edge requires protocol negotiation but the wire aspect says otherwise");
    }
  }
  if (edge.requires_state_translation) {
    if (edge.migration.empty() || !edge.migration_generation.is_set()) {
      return Status::failure(ErrorCode::InvalidArgument, "state translation edge must name a migration");
    }
    if (edge.aspect(CompatAspect::StateMigration).outcome != CompatOutcome::CompatibleAfterStateMigration) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "edge requires state translation but the migration aspect says otherwise");
    }
  }
  return Status::ok();
}

Status CompatibilityMatrix::upsert(const CompatibilityEdge& edge) {
  const Status valid = validate_edge(edge);
  if (valid.is_failure()) return valid;
  const Key key{edge.from.raw(), edge.to.raw()};
  const auto existing = edges_.find(key);
  if (existing == edges_.end()) {
    if (edges_.size() >= limits::kCompatibilityEdges) {
      return Status::failure(ErrorCode::LimitExceeded, "compatibility edge bound reached");
    }
    edges_.emplace(key, edge);
    auto& row = neighbours_[edge.from.raw()];
    const auto position = std::lower_bound(row.begin(), row.end(), edge.to.raw());
    row.insert(position, edge.to.raw());
  } else {
    existing->second = edge;
  }
  generation_ = generation_.is_set() ? generation_.next() : CompatibilityGeneration::first();
  return Status::ok();
}

Status CompatibilityMatrix::remove(RuntimeGeneration from, RuntimeGeneration to) {
  const Key key{from.raw(), to.raw()};
  if (edges_.erase(key) == 0) {
    return Status::failure(ErrorCode::NotFound, "compatibility edge is not present");
  }
  const auto row = neighbours_.find(from.raw());
  if (row != neighbours_.end()) {
    const auto position = std::lower_bound(row->second.begin(), row->second.end(), to.raw());
    if (position != row->second.end() && *position == to.raw()) row->second.erase(position);
    if (row->second.empty()) neighbours_.erase(row);
  }
  generation_ = generation_.is_set() ? generation_.next() : CompatibilityGeneration::first();
  return Status::ok();
}

const CompatibilityEdge* CompatibilityMatrix::find(RuntimeGeneration from,
                                                   RuntimeGeneration to) const noexcept {
  const auto it = edges_.find(Key{from.raw(), to.raw()});
  return it == edges_.end() ? nullptr : &it->second;
}

bool CompatibilityMatrix::permits(RuntimeGeneration from, RuntimeGeneration to,
                                  CompatPermission permission) const noexcept {
  const CompatibilityEdge* edge = find(from, to);
  if (edge == nullptr) return false;
  return has_permission(edge->permissions, permission);
}

std::vector<RuntimeGeneration> CompatibilityMatrix::neighbours(RuntimeGeneration from) const {
  std::vector<RuntimeGeneration> out;
  const auto row = neighbours_.find(from.raw());
  if (row == neighbours_.end()) return out;
  out.reserve(row->second.size());
  for (const auto raw : row->second) out.push_back(RuntimeGeneration::from_raw(raw));
  return out;
}

std::vector<const CompatibilityEdge*> CompatibilityMatrix::all_edges() const {
  std::vector<const CompatibilityEdge*> out;
  out.reserve(edges_.size());
  for (const auto& [key, edge] : edges_) {
    (void)key;
    out.push_back(&edge);
  }
  return out;
}

std::vector<std::string> CompatibilityMatrix::explain(RuntimeGeneration from,
                                                      RuntimeGeneration to) const {
  std::vector<std::string> lines;
  const CompatibilityEdge* edge = find(from, to);
  if (edge == nullptr) {
    lines.push_back("no compatibility edge recorded for generation " + std::to_string(from.raw()) +
                    " -> " + std::to_string(to.raw()));
    return lines;
  }
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    const auto& assessment = edge->aspects[i];
    std::string line = kAspectNames[i];
    line += " = ";
    line += to_string(assessment.outcome);
    line += " [";
    line += ref::to_string(assessment.provenance);
    line += " evidence=";
    line += std::to_string(assessment.evidence.raw());
    line += "]";
    lines.push_back(std::move(line));
  }
  lines.push_back("permissions = " + describe_permissions(edge->permissions));
  if (edge->requires_feature_gate) {
    lines.push_back("requires feature gate " + edge->gate.str() + " generation " +
                    std::to_string(edge->gate_generation.raw()));
  }
  if (edge->requires_protocol_downgrade) {
    lines.push_back("requires protocol negotiation down to generation " +
                    std::to_string(edge->downgrade_to.raw()));
  }
  if (edge->requires_state_translation) {
    lines.push_back("requires state translation via migration " + edge->migration.str() + " generation " +
                    std::to_string(edge->migration_generation.raw()));
  }
  return lines;
}

Status CompatibilityMatrix::validate() const {
  if (edges_.size() > limits::kCompatibilityEdges) {
    return Status::failure(ErrorCode::Corrupt, "compatibility matrix exceeds the edge bound");
  }
  for (const auto& [key, edge] : edges_) {
    if (key.first != edge.from.raw() || key.second != edge.to.raw()) {
      return Status::failure(ErrorCode::Corrupt, "compatibility matrix key does not match its edge");
    }
    const Status valid = validate_edge(edge);
    if (valid.is_failure()) return valid;
  }
  for (const auto& [from, row] : neighbours_) {
    if (!std::is_sorted(row.begin(), row.end())) {
      return Status::failure(ErrorCode::Corrupt, "compatibility neighbour index is not sorted");
    }
    std::vector<std::uint64_t> unique = row;
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.size() != row.size()) {
      return Status::failure(ErrorCode::Corrupt, "compatibility neighbour index has duplicates");
    }
    for (const auto to : row) {
      if (edges_.find(Key{from, to}) == edges_.end()) {
        return Status::failure(ErrorCode::Corrupt, "compatibility neighbour index is inconsistent");
      }
    }
  }
  return Status::ok();
}

}  // namespace ref
