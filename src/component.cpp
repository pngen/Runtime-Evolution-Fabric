#include "ref/component.hpp"

#include <array>

namespace ref {
namespace {

struct Transition {
  LifecycleState from;
  LifecycleState to;
};

// Explicit precondition table. A lifecycle change not listed here is rejected;
// there is no implicit "anything goes" path, and RETIRED appears only as a
// target, never as a source.
constexpr std::array<Transition, 46> kTransitions{{
    {LifecycleState::Registered, LifecycleState::CompatibilityPending},
    {LifecycleState::Registered, LifecycleState::RevalidationRequired},
    {LifecycleState::Registered, LifecycleState::Failed},
    {LifecycleState::CompatibilityPending, LifecycleState::Eligible},
    {LifecycleState::CompatibilityPending, LifecycleState::RevalidationRequired},
    {LifecycleState::CompatibilityPending, LifecycleState::Failed},
    {LifecycleState::Eligible, LifecycleState::UpgradePending},
    {LifecycleState::Eligible, LifecycleState::Current},
    {LifecycleState::Eligible, LifecycleState::Draining},
    {LifecycleState::Eligible, LifecycleState::RetirementPending},
    {LifecycleState::Eligible, LifecycleState::RevalidationRequired},
    {LifecycleState::Eligible, LifecycleState::Failed},
    {LifecycleState::UpgradePending, LifecycleState::CanaryActive},
    {LifecycleState::UpgradePending, LifecycleState::MixedVersionActive},
    {LifecycleState::UpgradePending, LifecycleState::RolloutActive},
    {LifecycleState::UpgradePending, LifecycleState::RollbackPending},
    {LifecycleState::UpgradePending, LifecycleState::Failed},
    {LifecycleState::CanaryActive, LifecycleState::MixedVersionActive},
    {LifecycleState::CanaryActive, LifecycleState::RolloutActive},
    {LifecycleState::CanaryActive, LifecycleState::RollbackPending},
    {LifecycleState::CanaryActive, LifecycleState::Failed},
    {LifecycleState::MixedVersionActive, LifecycleState::RolloutActive},
    {LifecycleState::MixedVersionActive, LifecycleState::RollbackPending},
    {LifecycleState::MixedVersionActive, LifecycleState::Draining},
    {LifecycleState::MixedVersionActive, LifecycleState::Failed},
    {LifecycleState::RolloutActive, LifecycleState::Current},
    {LifecycleState::RolloutActive, LifecycleState::RollbackPending},
    {LifecycleState::RolloutActive, LifecycleState::Draining},
    {LifecycleState::RolloutActive, LifecycleState::Failed},
    {LifecycleState::Current, LifecycleState::Draining},
    {LifecycleState::Current, LifecycleState::RollbackPending},
    {LifecycleState::Draining, LifecycleState::RetirementPending},
    {LifecycleState::Draining, LifecycleState::RolledBack},
    {LifecycleState::Draining, LifecycleState::Current},
    {LifecycleState::Draining, LifecycleState::Failed},
    {LifecycleState::RollbackPending, LifecycleState::RollingBack},
    {LifecycleState::RollbackPending, LifecycleState::Failed},
    {LifecycleState::RollingBack, LifecycleState::RolledBack},
    {LifecycleState::RollingBack, LifecycleState::Failed},
    {LifecycleState::RolledBack, LifecycleState::Eligible},
    {LifecycleState::RolledBack, LifecycleState::Current},
    {LifecycleState::RolledBack, LifecycleState::RevalidationRequired},
    {LifecycleState::RetirementPending, LifecycleState::Retired},
    {LifecycleState::RetirementPending, LifecycleState::Failed},
    {LifecycleState::RevalidationRequired, LifecycleState::CompatibilityPending},
    {LifecycleState::Failed, LifecycleState::RetirementPending},
}};

}  // namespace

const char* to_string(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Registered: return "REGISTERED";
    case LifecycleState::CompatibilityPending: return "COMPATIBILITY_PENDING";
    case LifecycleState::Eligible: return "ELIGIBLE";
    case LifecycleState::UpgradePending: return "UPGRADE_PENDING";
    case LifecycleState::CanaryActive: return "CANARY_ACTIVE";
    case LifecycleState::MixedVersionActive: return "MIXED_VERSION_ACTIVE";
    case LifecycleState::RolloutActive: return "ROLLOUT_ACTIVE";
    case LifecycleState::Current: return "CURRENT";
    case LifecycleState::Draining: return "DRAINING";
    case LifecycleState::RollbackPending: return "ROLLBACK_PENDING";
    case LifecycleState::RollingBack: return "ROLLING_BACK";
    case LifecycleState::RolledBack: return "ROLLED_BACK";
    case LifecycleState::RetirementPending: return "RETIREMENT_PENDING";
    case LifecycleState::Retired: return "RETIRED";
    case LifecycleState::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case LifecycleState::Failed: return "FAILED";
  }
  return "FAILED";
}

std::optional<LifecycleState> parse_lifecycle(std::string_view text) noexcept {
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(LifecycleState::Failed); ++i) {
    const auto state = static_cast<LifecycleState>(i);
    if (text == to_string(state)) return state;
  }
  return std::nullopt;
}

bool is_terminal_lifecycle(LifecycleState state) noexcept { return state == LifecycleState::Retired; }

bool is_authoritative_lifecycle(LifecycleState state) noexcept { return state == LifecycleState::Current; }

bool permits_mutation_lifecycle(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Current:
    case LifecycleState::RolloutActive:
    case LifecycleState::MixedVersionActive:
    case LifecycleState::CanaryActive:
      return true;
    default:
      return false;
  }
}

bool is_transition_allowed(LifecycleState from, LifecycleState to) noexcept {
  for (const auto& transition : kTransitions) {
    if (transition.from == from && transition.to == to) return true;
  }
  return false;
}

Status validate_transition(LifecycleState from, LifecycleState to) noexcept {
  if (from == to) return Status::failure(ErrorCode::Conflict, "lifecycle already in requested state");
  if (is_terminal_lifecycle(from)) {
    return Status::failure(ErrorCode::RetiredGeneration, "RETIRED is terminal for this runtime generation");
  }
  if (!is_transition_allowed(from, to)) {
    std::string detail = "lifecycle transition ";
    detail += ref::to_string(from);
    detail += " -> ";
    detail += ref::to_string(to);
    detail += " is not permitted";
    return Status::failure(ErrorCode::Conflict, detail);
  }
  return Status::ok();
}

Status RuntimeComponentVersion::validate() const noexcept {
  if (component.empty()) return Status::failure(ErrorCode::InvalidArgument, "component id is empty");
  if (!generation.is_set()) return Status::failure(ErrorCode::InvalidArgument, "runtime generation is unset");
  if (!version.is_set()) return Status::failure(ErrorCode::InvalidArgument, "version identity is unset");
  if (protocols.supported.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "component declares no supported protocol generation");
  }
  if (schemas.supported.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "component declares no supported schema generation");
  }
  if (protocols.minimum_safety.is_set() && !protocols.supported.contains(protocols.minimum_safety)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "minimum safety protocol generation is not in the supported set");
  }
  if (protocols.readable.empty() || protocols.writable.empty()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "readable and writable protocol sets must both be declared");
  }
  if (schemas.readable.empty() || schemas.writable.empty()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "readable and writable schema sets must both be declared");
  }
  if (compatible_generations.is_set() && !compatible_generations.is_valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "compatible generation range is inverted");
  }
  if (compatible_generations.is_set() && !compatible_generations.contains(generation)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "runtime generation is outside its own declared compatible range");
  }
  for (const auto& migration : migrations) {
    if (migration.id.empty() || !migration.source.is_set() || !migration.target.is_set()) {
      return Status::failure(ErrorCode::InvalidArgument, "migration capability is incomplete");
    }
    if (migration.source == migration.target) {
      return Status::failure(ErrorCode::InvalidArgument, "migration source equals target generation");
    }
  }
  if (rollback.supported && !rollback.target_window.is_valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "rollback window is not a valid range");
  }
  for (const auto& feature : features) {
    if (feature.feature.empty()) {
      return Status::failure(ErrorCode::InvalidArgument, "feature support entry has an empty feature id");
    }
  }
  return Status::ok();
}

const MigrationCapability* RuntimeComponentVersion::find_migration(SchemaGeneration source,
                                                                   SchemaGeneration target) const noexcept {
  for (const auto& migration : migrations) {
    if (migration.source == source && migration.target == target) return &migration;
  }
  return nullptr;
}

const FeatureSupport* RuntimeComponentVersion::find_feature(const FeatureGateId& feature) const noexcept {
  for (const auto& entry : features) {
    if (entry.feature == feature) return &entry;
  }
  return nullptr;
}

Status ComponentRegistry::publish(const RuntimeComponentVersion& version) {
  const Status valid = version.validate();
  if (valid.is_failure()) return valid;
  auto& versions = by_component_[version.component];
  const auto existing = versions.find(version.generation.raw());
  if (existing != versions.end()) {
    if (existing->second.version != version.version) {
      return Status::failure(ErrorCode::Conflict,
                             "runtime generation already bound to a different artifact identity");
    }
    // Republishing the same identity is idempotent, except lifecycle movement,
    // which must go through set_lifecycle so preconditions are enforced.
    if (existing->second.lifecycle != version.lifecycle) {
      return Status::failure(ErrorCode::Conflict,
                             "lifecycle change must use set_lifecycle to enforce preconditions");
    }
    existing->second = version;
    return Status::ok();
  }
  if (version_count_ >= limits::kComponentVersions) {
    return Status::failure(ErrorCode::LimitExceeded, "component version bound reached");
  }
  if (version.lifecycle != LifecycleState::Registered &&
      version.lifecycle != LifecycleState::Current &&
      version.lifecycle != LifecycleState::Eligible) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "first publication must use REGISTERED, ELIGIBLE or CURRENT lifecycle");
  }
  versions.emplace(version.generation.raw(), version);
  ++version_count_;
  return Status::ok();
}

Status ComponentRegistry::restore(const RuntimeComponentVersion& version) {
  const Status valid = version.validate();
  if (valid.is_failure()) return valid;
  auto& versions = by_component_[version.component];
  if (versions.find(version.generation.raw()) != versions.end()) {
    return Status::failure(ErrorCode::AlreadyExists, "duplicate runtime component generation in durable state");
  }
  if (version_count_ >= limits::kComponentVersions) {
    return Status::failure(ErrorCode::LimitExceeded, "component version bound reached while loading");
  }
  versions.emplace(version.generation.raw(), version);
  ++version_count_;
  return Status::ok();
}

Status ComponentRegistry::set_lifecycle(const RuntimeComponentId& component, RuntimeGeneration generation,
                                        LifecycleState next) {
  const auto component_it = by_component_.find(component);
  if (component_it == by_component_.end()) {
    return Status::failure(ErrorCode::NotFound, "component is not registered");
  }
  const auto version_it = component_it->second.find(generation.raw());
  if (version_it == component_it->second.end()) {
    return Status::failure(ErrorCode::NotFound, "runtime generation is not registered");
  }
  const Status valid = validate_transition(version_it->second.lifecycle, next);
  if (valid.is_failure()) return valid;
  version_it->second.lifecycle = next;
  return Status::ok();
}

Status ComponentRegistry::remove(const RuntimeComponentId& component, RuntimeGeneration generation) {
  const auto component_it = by_component_.find(component);
  if (component_it == by_component_.end()) {
    return Status::failure(ErrorCode::NotFound, "component is not registered");
  }
  const auto erased = component_it->second.erase(generation.raw());
  if (erased == 0) return Status::failure(ErrorCode::NotFound, "runtime generation is not registered");
  --version_count_;
  if (component_it->second.empty()) by_component_.erase(component_it);
  return Status::ok();
}

const RuntimeComponentVersion* ComponentRegistry::find(const RuntimeComponentId& component,
                                                       RuntimeGeneration generation) const noexcept {
  const auto component_it = by_component_.find(component);
  if (component_it == by_component_.end()) return nullptr;
  const auto version_it = component_it->second.find(generation.raw());
  if (version_it == component_it->second.end()) return nullptr;
  return &version_it->second;
}

std::vector<const RuntimeComponentVersion*> ComponentRegistry::versions_of(
    const RuntimeComponentId& component) const {
  std::vector<const RuntimeComponentVersion*> out;
  const auto component_it = by_component_.find(component);
  if (component_it == by_component_.end()) return out;
  out.reserve(component_it->second.size());
  for (const auto& [generation, version] : component_it->second) {
    (void)generation;
    out.push_back(&version);
  }
  return out;
}

std::vector<RuntimeComponentId> ComponentRegistry::components() const {
  std::vector<RuntimeComponentId> out;
  out.reserve(by_component_.size());
  for (const auto& [component, versions] : by_component_) {
    (void)versions;
    out.push_back(component);
  }
  return out;
}

std::vector<const RuntimeComponentVersion*> ComponentRegistry::all_versions() const {
  std::vector<const RuntimeComponentVersion*> out;
  out.reserve(version_count_);
  for (const auto& [component, versions] : by_component_) {
    (void)component;
    for (const auto& [generation, version] : versions) {
      (void)generation;
      out.push_back(&version);
    }
  }
  return out;
}

std::size_t ComponentRegistry::version_count() const noexcept { return version_count_; }

std::optional<RuntimeGeneration> ComponentRegistry::authoritative_generation(
    const RuntimeComponentId& component) const {
  const auto component_it = by_component_.find(component);
  if (component_it == by_component_.end()) return std::nullopt;
  std::optional<RuntimeGeneration> best;
  for (const auto& [generation, version] : component_it->second) {
    if (!is_authoritative_lifecycle(version.lifecycle)) continue;
    if (!best.has_value() || generation > best->raw()) {
      best = RuntimeGeneration::from_raw(generation);
    }
  }
  return best;
}

}  // namespace ref
