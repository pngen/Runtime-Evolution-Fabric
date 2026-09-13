#include "support/fixtures.hpp"

#include <chrono>
#include <cstdio>

#include "ref/ref.hpp"

namespace reftest {
namespace {

std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(static_cast<unsigned long long>(now)) + "-" +
         std::to_string(counter.fetch_add(1));
}

}  // namespace

TempDir::TempDir(const std::string& name) {
  std::error_code error;
  const auto base = std::filesystem::current_path(error) / "ref-test-tmp";
  std::filesystem::create_directories(base, error);
  path_ = (base / (name + "-" + unique_suffix())).string();
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::file(const std::string& name) const { return path_ + "/" + name; }

ref::RuntimeComponentVersion make_component(
    const char* component, std::uint64_t generation, const char* protocols, const char* schemas,
    ref::LifecycleState lifecycle,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& migrations,
    const std::vector<ref::FeatureSupport>& features, const char* artifact) {
  ref::RuntimeComponentVersion version;
  version.component = ref::RuntimeComponentId::from_valid(component);
  version.version.number.major = static_cast<std::uint16_t>(generation);
  version.version.number.minor = 0;
  version.version.number.patch = 0;
  version.version.artifact.build = ref::BuildId::from_valid(artifact);
  version.version.artifact.content_hash = ref::IntegrityDigest::of(std::string(artifact) + component);
  version.generation = ref::RuntimeGeneration::from_raw(generation);
  (void)ref::decode_generation_set(protocols, version.protocols.supported);
  version.protocols.readable = version.protocols.supported;
  version.protocols.writable = version.protocols.supported;
  version.protocols.minimum_safety = version.protocols.supported.lowest();
  (void)ref::decode_generation_set(schemas, version.schemas.supported);
  version.schemas.readable = version.schemas.supported;
  version.schemas.writable = version.schemas.supported;
  (void)version.schemas.readable_formats.add(ref::kStateFormatGenerationV1);
  (void)version.schemas.writable_formats.add(ref::kStateFormatGenerationV1);
  version.capability_generation = ref::CapabilityGeneration::from_raw(generation);
  version.lifecycle = lifecycle;
  version.provenance = ref::EvidenceClass::Real;
  version.provenance = ref::EvidenceClass::Real;
  for (const auto& [source, target] : migrations) {
    ref::MigrationCapability capability;
    capability.id = ref::MigrationId::from_valid("migration-" + std::to_string(source) + "-" +
                                                 std::to_string(target));
    capability.generation = ref::MigrationGeneration::from_raw(1);
    capability.schema = ref::SchemaId::from_valid("component-state");
    capability.source = ref::SchemaGeneration::from_raw(source);
    capability.target = ref::SchemaGeneration::from_raw(target);
    capability.reversible = target < source;
    capability.preserves_rollback_metadata = true;
    version.migrations.push_back(capability);
  }
  version.features = features;
  return version;
}

ref::CompatibilityEdge make_edge(std::uint64_t from, std::uint64_t to, const char* profile) {
  ref::CompatibilityEdge edge;
  edge.from = ref::RuntimeGeneration::from_raw(from);
  edge.to = ref::RuntimeGeneration::from_raw(to);
  const bool mixed = std::string(profile) == "mixed-version";
  for (std::size_t i = 0; i < ref::kCompatAspectCount; ++i) {
    const auto aspect = static_cast<ref::CompatAspect>(i);
    ref::CompatOutcome outcome = ref::CompatOutcome::FullyCompatible;
    if (mixed) {
      switch (aspect) {
        case ref::CompatAspect::WireProtocol:
        case ref::CompatAspect::ProtocolRead:
        case ref::CompatAspect::PeerVersion:
        case ref::CompatAspect::Capability:
        case ref::CompatAspect::PersistenceFormat:
        case ref::CompatAspect::Snapshot:
        case ref::CompatAspect::FeatureGate:
        case ref::CompatAspect::StateMigration:
          outcome = ref::CompatOutcome::MixedVersionCompatible;
          break;
        case ref::CompatAspect::ProtocolWrite:
        case ref::CompatAspect::SchemaWrite:
          outcome = ref::CompatOutcome::ReadCompatible;
          break;
        case ref::CompatAspect::SchemaRead:
          outcome = ref::CompatOutcome::CompatibleAfterStateMigration;
          break;
        case ref::CompatAspect::Rollback:
          outcome = ref::CompatOutcome::RollbackCompatible;
          break;
        default:
          outcome = ref::CompatOutcome::FullyCompatible;
          break;
      }
    }
    edge.aspects[i].outcome = outcome;
    edge.aspects[i].provenance = ref::EvidenceClass::Real;
    edge.aspects[i].evidence = ref::EvidenceGeneration::first();
    edge.aspects[i].detail = ref::DetailText::from_valid("fixture evidence");
  }
  edge.permissions = ref::derive_permissions(edge);
  edge.generation = ref::CompatibilityGeneration::first();
  edge.evidence = ref::EvidenceGeneration::first();
  edge.epoch = ref::EvolutionEpoch::first();
  return edge;
}

ref::ProtocolRegistry make_protocol_registry() {
  ref::ProtocolRegistry registry;
  (void)registry.publish(ref::build_wire_protocol(ref::ProtocolGeneration::from_raw(1)));
  (void)registry.publish(ref::build_wire_protocol(ref::ProtocolGeneration::from_raw(2)));
  return registry;
}

ref::SchemaRegistry make_schema_registry() {
  ref::SchemaRegistry registry;
  (void)ref::install_builtin_state_schemas(registry);
  return registry;
}

ref::PeerIdentity operator_peer() {
  ref::PeerIdentity peer;
  peer.session = ref::SessionId::from_valid("test-operator");
  peer.is_operator = true;
  peer.authenticated = true;
  return peer;
}

ref::FeatureSupport feature(const char* id, std::uint64_t generation, bool publish, bool consume) {
  ref::FeatureSupport support;
  support.feature = ref::FeatureGateId::from_valid(id);
  support.generation = ref::FeatureGateGeneration::from_raw(generation);
  support.can_publish = publish;
  support.can_consume = consume;
  return support;
}

ref::Status make_worker_state(const std::string& path, const char* owner, std::uint64_t counter) {
  ref::StateObject object;
  ref::Status status = object.set_text(ref::FieldId::from_valid("owner"), owner);
  if (status.is_failure()) return status;
  status = object.set_u64(ref::FieldId::from_valid("mutation_counter"), counter);
  if (status.is_failure()) return status;
  status = object.set_text(ref::FieldId::from_valid("legacy_token"), std::string("token-") + owner);
  if (status.is_failure()) return status;
  ref::StateFile file;
  file.header.format = ref::kStateFormatGenerationV1;
  file.header.schema = ref::SchemaId::from_valid("component-state");
  file.header.generation = ref::SchemaGeneration::from_raw(1);
  file.header.epoch = ref::EvolutionEpoch::from_raw(1);
  file.payload = object.encode();
  return ref::write_state_file(path, file);
}

}  // namespace reftest
