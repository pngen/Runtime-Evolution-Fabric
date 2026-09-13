// Runtime Evolution Fabric - runtime component model and lifecycle.
//
// A runtime component version is a description of what a binary can do; it is
// deliberately independent of any physical process. Processes are described by
// WorkerRegistration (see worker.hpp) and bind to a component version.
#pragma once

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
// Lifecycle
// ---------------------------------------------------------------------------
enum class LifecycleState : std::uint8_t {
  Registered = 0,
  CompatibilityPending,
  Eligible,
  UpgradePending,
  CanaryActive,
  MixedVersionActive,
  RolloutActive,
  Current,
  Draining,
  RollbackPending,
  RollingBack,
  RolledBack,
  RetirementPending,
  Retired,
  RevalidationRequired,
  Failed,
};

[[nodiscard]] const char* to_string(LifecycleState state) noexcept;
[[nodiscard]] std::optional<LifecycleState> parse_lifecycle(std::string_view text) noexcept;

// RETIRED is terminal for a runtime generation: no transition out of it exists.
[[nodiscard]] bool is_terminal_lifecycle(LifecycleState state) noexcept;
// Only CURRENT may be reported as the authoritative generation for a component.
[[nodiscard]] bool is_authoritative_lifecycle(LifecycleState state) noexcept;
// Whether a process in this lifecycle may still mutate shared state.
[[nodiscard]] bool permits_mutation_lifecycle(LifecycleState state) noexcept;
// Explicit precondition table; every lifecycle change goes through this.
[[nodiscard]] bool is_transition_allowed(LifecycleState from, LifecycleState to) noexcept;
[[nodiscard]] Status validate_transition(LifecycleState from, LifecycleState to) noexcept;

// ---------------------------------------------------------------------------
// Declared support surface of a component version
// ---------------------------------------------------------------------------
struct ProtocolSupport {
  ProtocolGenerationSet supported{};    // generations this binary can speak
  ProtocolGenerationSet readable{};     // generations it can decode
  ProtocolGenerationSet writable{};     // generations it may legally emit
  ProtocolGeneration minimum_safety{};  // never negotiated below this generation
};

struct SchemaSupport {
  SchemaGenerationSet supported{};
  SchemaGenerationSet readable{};
  SchemaGenerationSet writable{};
  StateFormatGenerationSet readable_formats{};
  StateFormatGenerationSet writable_formats{};
};

// A migration function the component can actually execute.
struct MigrationCapability {
  MigrationId id{};
  MigrationGeneration generation{};
  SchemaId schema{};
  SchemaGeneration source{};
  SchemaGeneration target{};
  bool reversible{false};   // reverse migration function exists
  bool preserves_rollback_metadata{true};
};

struct RollbackCapability {
  bool supported{false};
  RuntimeGenerationRange target_window{};
  bool requires_state_restore{false};
  bool reverse_migration_available{false};
};

// Peer-version requirement: which generations of another component must be
// present for this component version to be eligible.
struct PeerRequirement {
  RuntimeComponentId peer{};
  RuntimeGenerationRange generations{};
  bool required{true};
};

struct FeatureSupport {
  FeatureGateId feature{};
  FeatureGateGeneration generation{};
  bool can_publish{false};  // may emit traffic/state for this feature
  bool can_consume{false};  // may process traffic/state for this feature
};

// One runtime component version, identified independently of semantic-version
// strings: runtime generation and artifact identity are authoritative.
struct RuntimeComponentVersion {
  RuntimeComponentId component{};
  RuntimeVersionId version{};
  RuntimeGeneration generation{};
  ProtocolSupport protocols{};
  SchemaSupport schemas{};
  std::vector<MigrationCapability> migrations{};
  RollbackCapability rollback{};
  std::vector<FeatureSupport> features{};
  CapabilityGeneration capability_generation{};
  std::vector<PeerRequirement> peers{};
  RuntimeGenerationRange compatible_generations{};  // min/max compatible runtime generations
  LifecycleState lifecycle{LifecycleState::Registered};
  EvidenceClass provenance{EvidenceClass::Unknown};
  EvidenceGeneration evidence{};
  EvolutionEpoch epoch{};
  DetailText notes{};

  [[nodiscard]] bool is_set() const noexcept {
    return !component.empty() && generation.is_set() && version.is_set();
  }
  // Structural validation independent of any other component.
  [[nodiscard]] Status validate() const noexcept;
  // Read/write asymmetry helpers -- these are separate questions on purpose.
  [[nodiscard]] bool can_read_protocol(ProtocolGeneration candidate) const noexcept {
    return protocols.readable.contains(candidate);
  }
  [[nodiscard]] bool can_write_protocol(ProtocolGeneration candidate) const noexcept {
    return protocols.writable.contains(candidate);
  }
  [[nodiscard]] bool can_read_schema(SchemaGeneration candidate) const noexcept {
    return schemas.readable.contains(candidate);
  }
  [[nodiscard]] bool can_write_schema(SchemaGeneration candidate) const noexcept {
    return schemas.writable.contains(candidate);
  }
  [[nodiscard]] const MigrationCapability* find_migration(SchemaGeneration source,
                                                          SchemaGeneration target) const noexcept;
  [[nodiscard]] const FeatureSupport* find_feature(const FeatureGateId& feature) const noexcept;
};

// Registry of component versions. Lookup is by (component, generation) and is
// logarithmic; iteration order is deterministic.
class ComponentRegistry {
 public:
  ComponentRegistry() = default;

  Status publish(const RuntimeComponentVersion& version);
  // Restores a version from durable state: structural validation without the
  // first-publication lifecycle restriction, and without accepting duplicates.
  Status restore(const RuntimeComponentVersion& version);
  Status set_lifecycle(const RuntimeComponentId& component, RuntimeGeneration generation,
                       LifecycleState next);
  Status remove(const RuntimeComponentId& component, RuntimeGeneration generation);

  [[nodiscard]] const RuntimeComponentVersion* find(const RuntimeComponentId& component,
                                                    RuntimeGeneration generation) const noexcept;
  [[nodiscard]] std::vector<const RuntimeComponentVersion*> versions_of(
      const RuntimeComponentId& component) const;
  [[nodiscard]] std::vector<RuntimeComponentId> components() const;
  [[nodiscard]] std::vector<const RuntimeComponentVersion*> all_versions() const;
  [[nodiscard]] std::size_t version_count() const noexcept;
  [[nodiscard]] std::size_t component_count() const noexcept { return by_component_.size(); }

  // Highest generation of a component that is in an authoritative lifecycle.
  [[nodiscard]] std::optional<RuntimeGeneration> authoritative_generation(
      const RuntimeComponentId& component) const;

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (const auto& [component, versions] : by_component_) {
      for (const auto& [generation, version] : versions) {
        fn(version);
      }
    }
  }

 private:
  std::map<RuntimeComponentId, std::map<std::uint64_t, RuntimeComponentVersion>> by_component_{};
  std::size_t version_count_{0};
};

}  // namespace ref
