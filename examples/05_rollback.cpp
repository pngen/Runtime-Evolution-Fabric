// Example 5: rollback eligibility, allowed and then blocked by a barrier.
#include <cstdio>

#include "ref/ref.hpp"

using namespace ref;

namespace {

RollbackRequest base_request() {
  RollbackRequest request;
  request.id = RollbackId::from_valid("rollback-1");
  request.generation = RollbackGeneration::first();
  request.component = RuntimeComponentId::from_valid("orders");
  request.plan = EvolutionPlanId::from_valid("orders-upgrade");
  request.plan_generation = EvolutionPlanGeneration::first();
  request.schema = SchemaId::from_valid("component-state");
  request.current_generation = RuntimeGeneration::from_raw(2);
  request.target_generation = RuntimeGeneration::from_raw(1);
  request.current_protocol = ProtocolGeneration::from_raw(2);
  request.target_protocol = ProtocolGeneration::from_raw(1);
  request.current_schema = SchemaGeneration::from_raw(2);
  request.target_schema = SchemaGeneration::from_raw(1);
  request.reverse_migration_available = true;
  request.target_process_available = true;
  request.evidence_current = true;
  request.epoch = EvolutionEpoch::from_raw(2);
  request.coordinator_epoch = CoordinatorEpoch::first();
  return request;
}

RuntimeComponentVersion component(std::uint64_t generation, LifecycleState lifecycle) {
  RuntimeComponentVersion version;
  version.component = RuntimeComponentId::from_valid("orders");
  version.version.number = VersionNumber{static_cast<std::uint16_t>(generation), 0, 0};
  version.version.artifact.build = BuildId::from_valid("orders");
  version.generation = RuntimeGeneration::from_raw(generation);
  (void)version.protocols.supported.add(ProtocolGeneration::from_raw(1));
  version.protocols.readable = version.protocols.supported;
  version.protocols.writable = version.protocols.supported;
  version.protocols.minimum_safety = ProtocolGeneration::from_raw(1);
  (void)version.schemas.supported.add(SchemaGeneration::from_raw(1));
  version.schemas.readable = version.schemas.supported;
  version.schemas.writable = version.schemas.supported;
  (void)version.schemas.readable_formats.add(kStateFormatGenerationV1);
  (void)version.schemas.writable_formats.add(kStateFormatGenerationV1);
  version.capability_generation = CapabilityGeneration::from_raw(generation);
  version.provenance = EvidenceClass::Real;
  version.lifecycle = lifecycle;
  return version;
}

}  // namespace

int main() {
  ComponentRegistry components;
  (void)components.publish(component(1, LifecycleState::Draining));
  (void)components.publish(component(2, LifecycleState::Current));

  CompatibilityMatrix matrix;
  CompatibilityEdge edge;
  edge.from = RuntimeGeneration::from_raw(2);
  edge.to = RuntimeGeneration::from_raw(1);
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    edge.aspects[i].outcome = CompatOutcome::FullyCompatible;
    edge.aspects[i].provenance = EvidenceClass::Real;
    edge.aspects[i].evidence = EvidenceGeneration::first();
  }
  edge.permissions = derive_permissions(edge);
  edge.generation = CompatibilityGeneration::first();
  (void)matrix.upsert(edge);

  SchemaRegistry schemas;
  (void)install_builtin_state_schemas(schemas);

  const RollbackRequest request = base_request();
  const RollbackAssessment allowed = evaluate_rollback(request, matrix, components, schemas);
  std::printf("rollback while state is reversible: %s / %s\n", to_string(allowed.outcome),
              allowed.status.to_string().c_str());

  RollbackRequest after_barrier = request;
  after_barrier.irreversible_migration_crossed = true;
  after_barrier.reverse_migration_available = false;
  const RollbackAssessment blocked = evaluate_rollback(after_barrier, matrix, components, schemas);
  std::printf("rollback after the irreversible barrier: %s / %s\n", to_string(blocked.outcome),
              blocked.status.to_string().c_str());
  return 0;
}
