// Example 1: a basic compatible upgrade, driven entirely in process.
//
// Shows: component registration, evidence-derived eligibility, plan creation,
// stage advancement with explicit preconditions, and the authority view.
#include <cstdio>
#include <memory>

#include "ref/ref.hpp"

using namespace ref;

int main() {
  EvolutionCoordinator coordinator(CoordinatorConfig{});
  const Status started = coordinator.start();
  if (started.is_failure()) {
    std::printf("start failed: %s\n", started.to_string().c_str());
    return 1;
  }
  const PeerIdentity admin = coordinator.operator_identity().peer;

  RuntimeComponentVersion v1;
  v1.component = RuntimeComponentId::from_valid("orders");
  v1.version.number = VersionNumber{1, 0, 0};
  v1.version.artifact.build = BuildId::from_valid("orders-1.0.0");
  v1.generation = RuntimeGeneration::from_raw(1);
  (void)v1.protocols.supported.add(ProtocolGeneration::from_raw(1));
  v1.protocols.readable = v1.protocols.supported;
  v1.protocols.writable = v1.protocols.supported;
  v1.protocols.minimum_safety = ProtocolGeneration::from_raw(1);
  (void)v1.schemas.supported.add(SchemaGeneration::from_raw(1));
  v1.schemas.readable = v1.schemas.supported;
  v1.schemas.writable = v1.schemas.supported;
  (void)v1.schemas.readable_formats.add(kStateFormatGenerationV1);
  (void)v1.schemas.writable_formats.add(kStateFormatGenerationV1);
  v1.capability_generation = CapabilityGeneration::from_raw(1);
  v1.provenance = EvidenceClass::Real;
  v1.lifecycle = LifecycleState::Current;

  RuntimeComponentVersion v2 = v1;
  v2.version.number = VersionNumber{2, 0, 0};
  v2.version.artifact.build = BuildId::from_valid("orders-2.0.0");
  v2.generation = RuntimeGeneration::from_raw(2);
  (void)v2.protocols.supported.add(ProtocolGeneration::from_raw(2));
  (void)v2.schemas.supported.add(SchemaGeneration::from_raw(2));
  v2.schemas.readable = v2.schemas.supported;
  v2.schemas.writable = v2.schemas.supported;
  v2.capability_generation = CapabilityGeneration::from_raw(2);
  v2.lifecycle = LifecycleState::Registered;

  std::printf("register v1: %s\n", coordinator.command_register_component(admin, v1).status.to_string().c_str());
  std::printf("register v2: %s\n", coordinator.command_register_component(admin, v2).status.to_string().c_str());

  CompatibilityEdge edge;
  edge.from = RuntimeGeneration::from_raw(1);
  edge.to = RuntimeGeneration::from_raw(2);
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    edge.aspects[i].outcome = CompatOutcome::FullyCompatible;
    edge.aspects[i].provenance = EvidenceClass::Real;
    edge.aspects[i].evidence = EvidenceGeneration::first();
    edge.aspects[i].detail = DetailText::from_valid("verified by the local compatibility suite");
  }
  edge.permissions = derive_permissions(edge);
  edge.generation = CompatibilityGeneration::first();
  edge.evidence = EvidenceGeneration::first();
  edge.epoch = EvolutionEpoch::first();
  std::printf("publish compatibility: %s\n",
              coordinator.command_publish_compatibility(admin, edge).status.to_string().c_str());

  PlanSpec spec;
  spec.id = EvolutionPlanId::from_valid("orders-upgrade");
  spec.component = RuntimeComponentId::from_valid("orders");
  spec.candidate_generation = RuntimeGeneration::from_raw(2);
  spec.canary_cohort = CohortId::from_valid("canary-a");
  spec.cohorts.push_back(spec.canary_cohort);
  spec.policy.require_migration_barrier = false;
  spec.policy.require_canary = false;
  spec.policy.require_mixed_version_cohort = false;
  spec.policy.require_old_writer_drain = false;
  const CommandResult plan = coordinator.command_create_plan(admin, spec);
  std::printf("create plan: %s\n", plan.status.to_string().c_str());

  const EvolutionPlanId plan_id = EvolutionPlanId::from_valid("orders-upgrade");
  for (const RolloutStage stage : {RolloutStage::CompatibilityProven, RolloutStage::CanaryCohort,
                                   RolloutStage::FullPromotion}) {
    const CommandResult advanced =
        coordinator.command_advance_stage(admin, plan_id, stage, StageGeneration::unset());
    std::printf("advance to %s: %s\n", to_string(stage), advanced.status.to_string().c_str());
  }

  std::printf("authority: %s\n", coordinator.authority(RuntimeComponentId::from_valid("orders")).to_json().c_str());
  coordinator.stop();
  return 0;
}
